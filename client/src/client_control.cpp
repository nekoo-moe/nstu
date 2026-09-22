#include "nstu/client_control.hpp"

#include "nstu/multicast.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::uint64_t unix_seconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

bool send_status(control::AuthenticatedControlChannel& channel,
                 const ClientStatusProvider& provider,
                 protocol::CommandType type, std::uint64_t request_id,
                 std::string* error) {
    const auto payload = control::encode_status_report(provider());
    return !payload.empty() && channel.send(type, request_id, payload, error);
}

struct ConnectedControlChannel {
    control::AuthenticatedControlChannel channel;
    discovery::ServerEndpoint endpoint;
};

std::optional<ConnectedControlChannel> connect_authenticated(
    const ClientRuntimeConfig& config,
    const discovery::ServerEndpoint& endpoint,
    std::uint32_t connect_timeout_ms, std::string* error) {
    net::TcpSocket socket;
    if (!socket.connect_with_timeout(endpoint.address, endpoint.port,
                                     connect_timeout_ms, error) ||
        !socket.set_io_timeouts(5000, 5000, error)) {
        return std::nullopt;
    }
    auto session = control::client_handshake(
        socket, config.client_id, config.key_id, config.pre_shared_key,
        unix_seconds_now(), std::chrono::seconds(120), error);
    if (!session) {
        return std::nullopt;
    }
    return ConnectedControlChannel{
        control::AuthenticatedControlChannel(std::move(socket),
                                             std::move(*session)),
        endpoint};
}

// Random jitter, capped at kMaxProbeJitter, so that a whole room losing the
// server at once does not fire 40 synchronized broadcasts on the same tick.
// A crypto RNG is overkill for jitter but is the generator already linked, and
// a predictable value here is harmless.
std::chrono::milliseconds discovery_probe_jitter() {
    constexpr unsigned int kMaxProbeJitter = 500;
    std::array<std::byte, 2> bytes{};
    if (!security::generate_random(bytes)) {
        return std::chrono::milliseconds(0);
    }
    const unsigned int value =
        std::to_integer<unsigned int>(bytes[0]) |
        (std::to_integer<unsigned int>(bytes[1]) << 8u);
    return std::chrono::milliseconds(value % (kMaxProbeJitter + 1u));
}

// Reconnect race: the cached last-known endpoint and authenticated UDP
// discovery run concurrently, and the first branch to produce a mutually
// authenticated channel wins. A transient packet drop that left the server IP
// unchanged is served by the direct branch in milliseconds; a changed server
// IP stalls that branch and the discovery branch supplies the new endpoint.
// Neither branch re-exchanges the pre-shared key - discovery only relocates
// the endpoint and the TCP handshake still proves possession of the key.
std::optional<ConnectedControlChannel> connect_with_discovery(
    const ClientRuntimeConfig& config,
    const ClientConnectionOptions& options, std::string* error) {
    const auto connect_timeout = std::clamp<std::uint32_t>(
        options.connect_timeout_ms, 100, 10000);
    const auto direct_timeout = std::clamp<std::uint32_t>(
        options.direct_connect_timeout_ms, 100, connect_timeout);
    const discovery::ServerEndpoint configured{
        config.server_address, config.server_port};

    if (!options.enable_authenticated_discovery) {
        std::string direct_error;
        if (auto connected = connect_authenticated(config, configured,
                                                   connect_timeout,
                                                   &direct_error)) {
            if (error != nullptr) {
                error->clear();
            }
            return connected;
        }
        set_error(error, direct_error.empty() ? "server connection failed"
                                              : direct_error.c_str());
        return std::nullopt;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::optional<ConnectedControlChannel> winner;
    bool direct_done = false;
    bool discovery_done = false;
    std::string direct_error;
    std::string discovery_error;

    // Takes ownership of the win under the lock, or returns the losing
    // connection to the caller so its socket is closed off the lock.
    const auto claim = [&](std::optional<ConnectedControlChannel>&& candidate)
        -> std::optional<ConnectedControlChannel> {
        std::unique_lock lock(mutex);
        if (!winner && candidate) {
            winner = std::move(candidate);
            lock.unlock();
            ready.notify_all();
            return std::nullopt;
        }
        return std::move(candidate);
    };

    {
        std::jthread direct_branch([&] {
            std::string local_error;
            auto candidate = connect_authenticated(config, configured,
                                                   direct_timeout,
                                                   &local_error);
            auto loser = claim(std::move(candidate));
            std::scoped_lock lock(mutex);
            direct_error = std::move(local_error);
            direct_done = true;
            ready.notify_all();
            // `loser` closes its socket here, outside claim()'s lock.
            (void)loser;
        });

        std::jthread discovery_branch([&] {
            // Yield to the direct branch for the common transient-drop case,
            // and smear the broadcast across the room. Wake early if the
            // direct branch has already won.
            {
                std::unique_lock lock(mutex);
                ready.wait_for(lock, discovery_probe_jitter(),
                               [&] { return winner.has_value(); });
                if (winner) {
                    discovery_done = true;
                    return;
                }
            }
            auto discovery_options = options.discovery;
            // UDP and TCP deliberately share the configured numeric port, so
            // no second installer field or firewall port is required.
            discovery_options.discovery_port = config.server_port;
            std::string local_error;
            const auto endpoints = discovery::discover_authenticated_servers(
                config.client_id, config.key_id, config.pre_shared_key,
                discovery_options, &local_error);
            std::string endpoint_error;
            for (const auto& endpoint : endpoints) {
                {
                    std::scoped_lock lock(mutex);
                    if (winner) {
                        break;
                    }
                }
                auto candidate = connect_authenticated(
                    config, endpoint, connect_timeout, &endpoint_error);
                if (candidate) {
                    auto loser = claim(std::move(candidate));
                    if (!loser) {
                        break;  // We won.
                    }
                    (void)loser;  // Direct branch beat us; drop this one.
                    break;
                }
            }
            std::scoped_lock lock(mutex);
            if (!endpoint_error.empty()) {
                discovery_error = "authenticated server was discovered but TCP "
                                  "handshake failed: " + endpoint_error;
            } else {
                discovery_error = std::move(local_error);
            }
            discovery_done = true;
            ready.notify_all();
        });

        std::unique_lock lock(mutex);
        ready.wait(lock, [&] {
            return winner.has_value() || (direct_done && discovery_done);
        });
    }  // jthreads join here before winner/errors are read without the lock.

    if (winner) {
        if (error != nullptr) {
            error->clear();
        }
        return winner;
    }
    if (error != nullptr) {
        const std::string direct_failure =
            direct_error.empty() ? "server connection failed" : direct_error;
        if (!discovery_error.empty()) {
            *error = "configured endpoint failed (" + direct_failure + "); " +
                     discovery_error;
        } else {
            *error = direct_failure;
        }
    }
    return std::nullopt;
}

} // namespace

bool run_client_control_session(
    const ClientRuntimeConfig& config, std::stop_token stop_token,
    const ClientStatusProvider& status_provider,
    const ClientCommandHandler& command_handler,
    const ClientOutboundProvider& outbound_provider, std::string* error,
    const ClientEndpointObserver& endpoint_observer,
    const ClientConnectionOptions& connection_options) {
    if (!status_provider || !command_handler) {
        set_error(error, "client control callbacks are missing");
        return false;
    }
    net::WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "Winsock initialization failed");
        return false;
    }
    auto connected = connect_with_discovery(config, connection_options, error);
    if (!connected) {
        return false;
    }
    if (endpoint_observer) {
        endpoint_observer(connected->endpoint);
    }
    auto channel = std::move(connected->channel);
    std::uint64_t request_id = 1;
    if (!send_status(channel, status_provider, protocol::CommandType::hello,
                     request_id++, error)) {
        return false;
    }
    auto next_heartbeat = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    auto next_status = std::chrono::steady_clock::now() +
                       std::chrono::seconds(2);
    while (!stop_token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
            if (!channel.send(protocol::CommandType::heartbeat, request_id++,
                              {}, error)) {
                return false;
            }
            next_heartbeat = now + std::chrono::seconds(5);
        }
        if (now >= next_status) {
            if (!send_status(channel, status_provider,
                             protocol::CommandType::status_report,
                             request_id++, error)) {
                return false;
            }
            next_status = now + std::chrono::seconds(2);
        }
        if (outbound_provider) {
            for (int sent = 0; sent < 4; ++sent) {
                auto outbound = outbound_provider();
                if (!outbound) {
                    break;
                }
                if (!channel.send(outbound->type, request_id++,
                                  outbound->payload, error)) {
                    return false;
                }
            }
        }
        std::string wait_error;
        if (!channel.wait_readable(100, &wait_error)) {
            if (!wait_error.empty()) {
                if (error != nullptr) {
                    *error = wait_error;
                }
                return false;
            }
            continue;
        }
        const auto command = channel.receive(error);
        if (!command) {
            return false;
        }
        command_handler(*command);
        if (command->envelope.type == protocol::CommandType::status_request) {
            if (!send_status(channel, status_provider,
                             protocol::CommandType::status_report,
                             command->envelope.request_id, error)) {
                return false;
            }
        }
    }
    channel.close();
    return true;
}

} // namespace nstu::client
