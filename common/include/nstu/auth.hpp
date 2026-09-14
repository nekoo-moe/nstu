#pragma once

#include "nstu/protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace nstu::security {

inline constexpr std::size_t kClientIdBytes = 16;
inline constexpr std::size_t kNonceBytes = 32;
inline constexpr std::size_t kSha256Bytes = 32;
inline constexpr std::size_t kVideoAuthTagBytes = 16;
inline constexpr std::size_t kControlAuthTagBytes = 16;
inline constexpr std::size_t kMinimumProtocolKeyBytes = 32;
inline constexpr std::size_t kAuthHelloBytes = 60;
inline constexpr std::size_t kAuthChallengeBytes = 40;

using ClientId = std::array<std::byte, kClientIdBytes>;
using Nonce = std::array<std::byte, kNonceBytes>;
using Sha256Digest = std::array<std::byte, kSha256Bytes>;
using VideoAuthTag = std::array<std::byte, kVideoAuthTagBytes>;
using ControlAuthTag = std::array<std::byte, kControlAuthTagBytes>;

// Direction-bound MAC helpers are available for the next negotiated control
// protocol revision.  The current on-wire channel remains on the legacy
// neutral domain until both peers advertise the directional capability.
enum class ControlDirection : std::uint8_t {
    client_to_server = 1,
    server_to_client = 2,
};

struct AuthHello {
    ClientId client_id{};
    Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
    std::uint32_t key_id = 0;
};

struct AuthChallenge {
    Nonce server_nonce{};
    std::uint64_t unix_time_seconds = 0;
};

[[nodiscard]] std::vector<std::byte> encode_auth_hello(const AuthHello& hello);
[[nodiscard]] std::optional<AuthHello> decode_auth_hello(
    std::span<const std::byte> wire);
[[nodiscard]] std::vector<std::byte> encode_auth_challenge(
    const AuthChallenge& challenge);
[[nodiscard]] std::optional<AuthChallenge> decode_auth_challenge(
    std::span<const std::byte> wire);
[[nodiscard]] bool validate_auth_hello(const AuthHello& hello) noexcept;
[[nodiscard]] bool validate_auth_challenge(
    const AuthChallenge& challenge) noexcept;

[[nodiscard]] bool generate_random(std::span<std::byte> output) noexcept;
[[nodiscard]] std::optional<Sha256Digest> sha256(
    std::span<const std::byte> message) noexcept;
[[nodiscard]] std::optional<Sha256Digest> hmac_sha256(
    std::span<const std::byte> key, std::span<const std::byte> message) noexcept;
[[nodiscard]] bool constant_time_equal(std::span<const std::byte> left,
                                       std::span<const std::byte> right) noexcept;
void secure_zero(std::span<std::byte> bytes) noexcept;

[[nodiscard]] std::optional<Sha256Digest> compute_client_proof(
    std::span<const std::byte> pre_shared_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept;
[[nodiscard]] std::optional<Sha256Digest> derive_session_key(
    std::span<const std::byte> pre_shared_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept;
[[nodiscard]] std::optional<Sha256Digest> compute_server_proof(
    std::span<const std::byte> session_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept;

[[nodiscard]] std::optional<VideoAuthTag> compute_video_auth_tag(
    std::span<const std::byte> key,
    const protocol::VideoPacketHeader& header,
    std::span<const std::byte> payload) noexcept;
[[nodiscard]] bool verify_video_auth_tag(
    std::span<const std::byte> key,
    const protocol::VideoPacketHeader& header,
    std::span<const std::byte> payload,
    std::span<const std::byte> tag) noexcept;

[[nodiscard]] std::optional<ControlAuthTag> compute_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload,
    ControlDirection direction) noexcept;
// Legacy compatibility overload.  Do not use it for a newly negotiated
// directional channel; it is retained so existing v1 peers remain usable.
[[nodiscard]] std::optional<ControlAuthTag> compute_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload) noexcept;
[[nodiscard]] bool verify_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload,
    ControlDirection direction,
    std::span<const std::byte> tag) noexcept;
[[nodiscard]] bool verify_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload,
    std::span<const std::byte> tag) noexcept;

class ControlSequenceGuard {
public:
    void reset(std::uint64_t first_expected_sequence = 0) noexcept;
    [[nodiscard]] bool accept(std::uint64_t sequence) noexcept;
    [[nodiscard]] std::uint64_t next_expected_sequence() const noexcept;

private:
    std::uint64_t next_expected_ = 0;
    bool exhausted_ = false;
};

class ReplayProtector {
public:
    explicit ReplayProtector(
        std::size_t capacity = 4096,
        std::chrono::seconds maximum_clock_skew = std::chrono::seconds(120));

    // Call only after the client's MAC has been verified.
    [[nodiscard]] bool accept(const AuthHello& hello,
                              std::uint64_t now_unix_seconds);
    void clear();

private:
    struct Entry {
        std::string key;
        std::uint64_t expires_at = 0;
    };

    std::size_t capacity_ = 0;
    std::uint64_t maximum_clock_skew_ = 0;
    std::mutex mutex_;
    std::deque<Entry> entries_;
    std::unordered_set<std::string> keys_;
};

} // namespace nstu::security
