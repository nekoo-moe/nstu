#include "nstu/client_control.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/multicast.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    nstu::client::ClientRuntimeConfig client_config;
    client_config.server_address = "192.0.2.1";
    client_config.client_id.fill(std::byte{0});
    for (std::size_t index = 0; index < client_config.client_id.size(); ++index) {
        client_config.client_id[index] = static_cast<std::byte>(index + 21);
    }
    client_config.key_id = 8;
    client_config.pre_shared_key.resize(
        nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < client_config.pre_shared_key.size();
         ++index) {
        client_config.pre_shared_key[index] =
            static_cast<std::byte>(0x60 + index);
    }

    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(client_config.client_id, client_config.key_id,
                            client_config.pre_shared_key, &error));
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig server_config;
    server_config.port = 0;
    assert(server.start(std::move(server_config), &error));
    client_config.server_port = server.local_port();

    std::stop_source stop_source;
    std::atomic_bool lock_received = false;
    std::atomic_bool snapshot_sent = false;
    std::atomic_bool session_succeeded = false;
    std::atomic_bool discovered_loopback = false;
    std::atomic<std::uint16_t> discovered_port = 0;
    nstu::client::ClientConnectionOptions connection_options;
    connection_options.connect_timeout_ms = 100;
    connection_options.discovery.timeout = std::chrono::milliseconds(500);
    connection_options.discovery.target_addresses = {"127.0.0.1"};
    std::thread client([&] {
        std::string client_error;
        const bool result = nstu::client::run_client_control_session(
            client_config, stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "SERVICE-PC";
                status.session_id = 2;
                return status;
            },
            [&](const nstu::control::AuthenticatedCommand& command) {
                if (command.envelope.type ==
                    nstu::protocol::CommandType::lock) {
                    lock_received = true;
                    stop_source.request_stop();
                }
            },
            [&]() -> std::optional<nstu::client::ClientOutboundCommand> {
                if (snapshot_sent.exchange(true)) {
                    return std::nullopt;
                }
                nstu::control::SnapshotFrame frame;
                frame.width = 320;
                frame.height = 180;
                frame.captured_at_unix_milliseconds = 42;
                frame.jpeg = {std::byte{0xff}, std::byte{0xd8},
                              std::byte{0xff}, std::byte{0xd9}};
                return nstu::client::ClientOutboundCommand{
                    nstu::protocol::CommandType::snapshot_frame,
                    nstu::control::encode_snapshot_frame(frame)};
            },
            &client_error,
            [&](const nstu::discovery::ServerEndpoint& endpoint) {
                discovered_loopback = endpoint.address == "127.0.0.1";
                discovered_port = endpoint.port;
            },
            connection_options);
        session_succeeded = result;
    });
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return clients.size() == 1 && clients[0].hostname == "SERVICE-PC";
    }));
    const auto registry_id = registry.snapshot()[0].id;
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return !clients.empty() && clients[0].snapshot_generation == 1 &&
               clients[0].snapshot_width == 320;
    }));
    assert(server.set_locked(registry_id, true, &error));
    assert(wait_until([&] { return lock_received.load(); }));
    client.join();
    assert(session_succeeded.load());
    assert(discovered_loopback.load());
    assert(discovered_port.load() == server.local_port());

    auto rejected_config = client_config;
    rejected_config.server_address = "127.0.0.1";
    rejected_config.pre_shared_key[0] ^= static_cast<std::byte>(0x5a);
    nstu::client::ClientConnectionOptions rejected_options;
    rejected_options.connect_timeout_ms = 100;
    rejected_options.discovery.timeout = std::chrono::milliseconds(250);
    rejected_options.discovery.target_addresses = {"127.0.0.1"};
    std::atomic_bool unverified_endpoint_observed = false;
    std::stop_source rejected_stop_source;
    std::string rejected_error;
    const bool rejected = nstu::client::run_client_control_session(
        rejected_config, rejected_stop_source.get_token(),
        [] {
            nstu::control::ClientStatusReport status;
            status.hostname = "REJECTED-PC";
            return status;
        },
        [](const nstu::control::AuthenticatedCommand&) {}, {}, &rejected_error,
        [&](const nstu::discovery::ServerEndpoint&) {
            unverified_endpoint_observed = true;
        },
        rejected_options);
    assert(!rejected);
    assert(!rejected_error.empty());
    assert(!unverified_endpoint_observed.load());
    nstu::client::clear_client_runtime_config(rejected_config);

    // Direct-hit reconnect: the cached endpoint is correct, so the direct
    // branch of the parallel race wins on loopback without needing discovery.
    // The pre-shared key is untouched from the first session - reconnect never
    // re-exchanges the secret.
    auto direct_config = client_config;
    direct_config.server_address = "127.0.0.1";
    nstu::client::ClientConnectionOptions direct_options;
    direct_options.connect_timeout_ms = 1000;
    direct_options.direct_connect_timeout_ms = 500;
    // No discovery targets: if the direct branch did not carry this, the race
    // would have nothing to fall back to and the case would fail.
    direct_options.discovery.timeout = std::chrono::milliseconds(250);
    std::stop_source direct_stop_source;
    std::atomic_bool direct_lock_received = false;
    std::atomic_bool direct_session_succeeded = false;
    std::atomic_bool direct_endpoint_observed = false;
    std::thread direct_client([&] {
        std::string direct_error;
        const bool result = nstu::client::run_client_control_session(
            direct_config, direct_stop_source.get_token(),
            [] {
                nstu::control::ClientStatusReport status;
                status.hostname = "DIRECT-PC";
                return status;
            },
            [&](const nstu::control::AuthenticatedCommand& command) {
                if (command.envelope.type ==
                    nstu::protocol::CommandType::lock) {
                    direct_lock_received = true;
                    direct_stop_source.request_stop();
                }
            },
            {}, &direct_error,
            [&](const nstu::discovery::ServerEndpoint& endpoint) {
                if (endpoint.address == "127.0.0.1" &&
                    endpoint.port == server.local_port()) {
                    direct_endpoint_observed = true;
                }
            },
            direct_options);
        direct_session_succeeded = result;
    });
    assert(wait_until([&] {
        const auto clients = registry.snapshot();
        return !clients.empty() && clients[0].hostname == "DIRECT-PC";
    }));
    const auto direct_registry_id = registry.snapshot()[0].id;
    assert(server.set_locked(direct_registry_id, true, &error));
    assert(wait_until([&] { return direct_lock_received.load(); }));
    direct_client.join();
    assert(direct_session_succeeded.load());
    assert(direct_endpoint_observed.load());
    // The reconnect cache still holds the same secret it started with.
    assert(direct_config.pre_shared_key == client_config.pre_shared_key);
    nstu::client::clear_client_runtime_config(direct_config);

    server.stop();

    // Server-absent reconnect: with the server stopped, neither the cached
    // endpoint nor authenticated discovery can produce a channel, so the race
    // fails cleanly with a reported error and observes no endpoint.
    auto absent_config = client_config;
    absent_config.server_address = "192.0.2.1";
    nstu::client::ClientConnectionOptions absent_options;
    absent_options.connect_timeout_ms = 200;
    absent_options.direct_connect_timeout_ms = 150;
    absent_options.discovery.timeout = std::chrono::milliseconds(250);
    absent_options.discovery.target_addresses = {"127.0.0.1"};
    std::atomic_bool absent_endpoint_observed = false;
    std::stop_source absent_stop_source;
    std::string absent_error;
    const bool absent = nstu::client::run_client_control_session(
        absent_config, absent_stop_source.get_token(),
        [] {
            nstu::control::ClientStatusReport status;
            status.hostname = "ABSENT-PC";
            return status;
        },
        [](const nstu::control::AuthenticatedCommand&) {}, {}, &absent_error,
        [&](const nstu::discovery::ServerEndpoint&) {
            absent_endpoint_observed = true;
        },
        absent_options);
    assert(!absent);
    assert(!absent_error.empty());
    assert(!absent_endpoint_observed.load());
    nstu::client::clear_client_runtime_config(absent_config);

    nstu::client::clear_client_runtime_config(client_config);
    return 0;
}
