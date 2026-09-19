#include "nstu/client_control.hpp"

#include "nstu/multicast.hpp"

#include <algorithm>
#include <chrono>
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

std::optional<ConnectedControlChannel> connect_with_discovery(
    const ClientRuntimeConfig& config,
    const ClientConnectionOptions& options, std::string* error) {
    const auto connect_timeout = std::clamp<std::uint32_t>(
        options.connect_timeout_ms, 100, 10000);
    const discovery::ServerEndpoint configured{
        config.server_address, config.server_port};
    std::string direct_error;
    if (auto connected = connect_authenticated(config, configured,
                                                connect_timeout,
                                                &direct_error)) {
        if (error != nullptr) {
            error->clear();
        }
        return connected;
    }
    const std::string configured_failure =
        direct_error.empty() ? "server connection failed" : direct_error;
    if (!options.enable_authenticated_discovery) {
        if (error != nullptr) {
            *error = configured_failure;
        }
        return std::nullopt;
    }

    auto discovery_options = options.discovery;
    // UDP and TCP deliberately share the configured numeric port, so no
    // second installer field or firewall port is required.
    discovery_options.discovery_port = config.server_port;
    std::string discovery_error;
    const auto endpoints = discovery::discover_authenticated_servers(
        config.client_id, config.key_id, config.pre_shared_key,
        discovery_options, &discovery_error);
    std::string endpoint_error;
    for (const auto& endpoint : endpoints) {
        if (auto connected = connect_authenticated(
                config, endpoint, connect_timeout, &endpoint_error)) {
            if (error != nullptr) {
                error->clear();
            }
            return connected;
        }
    }
    if (error != nullptr) {
        if (!endpoint_error.empty()) {
            *error = "authenticated server was discovered but TCP handshake "
                     "failed: " + endpoint_error;
        } else if (!discovery_error.empty()) {
            *error = "configured endpoint failed (" + configured_failure +
                     "); " + discovery_error;
        } else {
            *error = configured_failure;
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
