#pragma once

#include "nstu/auth.hpp"
#include "nstu/key_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::discovery {

inline constexpr std::uint16_t kDiscoveryVersion = 1;
inline constexpr std::size_t kDiscoveryPacketBytes = 104;

struct DiscoveryRequest {
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
    security::Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
    security::Sha256Digest authenticator{};
};

struct DiscoveryResponse {
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
    security::Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
    std::uint16_t control_port = 0;
    security::Sha256Digest authenticator{};
};

[[nodiscard]] std::optional<DiscoveryRequest> create_request(
    const security::ClientId& client_id, std::uint32_t key_id,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> pre_shared_key) noexcept;

[[nodiscard]] std::vector<std::byte> encode_request(
    const DiscoveryRequest& request);
[[nodiscard]] std::optional<DiscoveryRequest> decode_request(
    std::span<const std::byte> wire);
[[nodiscard]] bool verify_request(
    const DiscoveryRequest& request,
    std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120))
    noexcept;

[[nodiscard]] std::optional<DiscoveryResponse> create_response(
    const DiscoveryRequest& request, std::uint16_t control_port,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> pre_shared_key) noexcept;

[[nodiscard]] std::vector<std::byte> encode_response(
    const DiscoveryResponse& response);
[[nodiscard]] std::optional<DiscoveryResponse> decode_response(
    std::span<const std::byte> wire);
[[nodiscard]] bool verify_response(
    const DiscoveryResponse& response, const DiscoveryRequest& request,
    std::span<const std::byte> pre_shared_key,
    std::uint64_t now_unix_seconds,
    std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120))
    noexcept;

struct ServerEndpoint {
    std::string address;
    std::uint16_t port = 0;

    [[nodiscard]] bool operator==(const ServerEndpoint&) const noexcept =
        default;
};

struct ClientDiscoveryOptions {
    std::uint16_t discovery_port = 47001;
    std::chrono::milliseconds timeout{1200};
    std::size_t maximum_endpoints = 8;
    // Empty uses the directed broadcasts of active IPv4 LAN adapters. Tests
    // and tightly managed deployments may provide explicit IPv4 targets.
    std::vector<std::string> target_addresses;
};

[[nodiscard]] std::vector<ServerEndpoint> discover_authenticated_servers(
    const security::ClientId& client_id, std::uint32_t key_id,
    std::span<const std::byte> pre_shared_key,
    const ClientDiscoveryOptions& options = {},
    std::string* error = nullptr);

class AuthenticatedDiscoveryResponder {
public:
    AuthenticatedDiscoveryResponder();
    ~AuthenticatedDiscoveryResponder();
    AuthenticatedDiscoveryResponder(const AuthenticatedDiscoveryResponder&) =
        delete;
    AuthenticatedDiscoveryResponder& operator=(
        const AuthenticatedDiscoveryResponder&) = delete;

    [[nodiscard]] bool start(std::uint16_t discovery_port,
                             std::uint16_t control_port,
                             const security::KeyStore& key_store,
                             std::string* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::discovery
