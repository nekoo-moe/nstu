#pragma once

#include "nstu/client_config.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/discovery.hpp"

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace nstu::client {

using ClientStatusProvider = std::function<control::ClientStatusReport()>;
using ClientCommandHandler =
    std::function<void(const control::AuthenticatedCommand&)>;
struct ClientOutboundCommand {
    protocol::CommandType type = protocol::CommandType::heartbeat;
    std::vector<std::byte> payload;
};
using ClientOutboundProvider =
    std::function<std::optional<ClientOutboundCommand>()>;
using ClientEndpointObserver =
    std::function<void(const discovery::ServerEndpoint&)>;

struct ClientConnectionOptions {
    bool enable_authenticated_discovery = true;
    // Timeout for connecting to a discovered endpoint, whose IP is fresh.
    std::uint32_t connect_timeout_ms = 2000;
    // Aggressive budget for the cached last-known endpoint in the parallel
    // reconnect race. A same-subnet transient drop completes in well under
    // this; a changed server IP stalls, and the authenticated discovery branch
    // wins instead. Clamped to at most connect_timeout_ms.
    std::uint32_t direct_connect_timeout_ms = 500;
    discovery::ClientDiscoveryOptions discovery;
};

[[nodiscard]] bool run_client_control_session(
    const ClientRuntimeConfig& config, std::stop_token stop_token,
    const ClientStatusProvider& status_provider,
    const ClientCommandHandler& command_handler,
    const ClientOutboundProvider& outbound_provider = {},
    std::string* error = nullptr,
    const ClientEndpointObserver& endpoint_observer = {},
    const ClientConnectionOptions& connection_options = {});

} // namespace nstu::client
