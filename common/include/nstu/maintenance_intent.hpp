#pragma once

#include "nstu/auth.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nstu::maintenance {

inline constexpr std::size_t kMaintenanceIntentIdBytes = 16;
inline constexpr std::size_t kMaintenanceAuthorityIdBytes = 16;
inline constexpr std::size_t kMaintenanceVolumeIdBytes = 16;
inline constexpr std::size_t kMaintenanceIntentHeaderBytes = 124;
inline constexpr std::size_t kMaximumMaintenanceParameterBytes = 64;
inline constexpr std::size_t kMaintenanceAuthTagBytes =
    security::kSha256Bytes;
inline constexpr std::uint64_t kMaximumMaintenanceIntentLifetimeSeconds = 900;
inline constexpr std::uint64_t kMaintenanceClockSkewSeconds = 120;
inline constexpr std::uint32_t kMaximumRestartGraceSeconds = 300;
inline constexpr std::size_t kMaximumMaintenanceReplayEntries = 65'536;

using MaintenanceIntentId =
    std::array<std::byte, kMaintenanceIntentIdBytes>;
using MaintenanceAuthorityId =
    std::array<std::byte, kMaintenanceAuthorityIdBytes>;
using MaintenanceVolumeId =
    std::array<std::byte, kMaintenanceVolumeIdBytes>;
using MaintenanceAuthTag =
    std::array<std::byte, kMaintenanceAuthTagBytes>;

enum class MaintenanceOperation : std::uint16_t {
    install_uwf_feature = 1,
    remove_uwf_feature = 2,
    protect_volume = 3,
    unprotect_volume = 4,
    enable_filter = 5,
    disable_filter = 6,
    apply_restore_policy = 7,
    enter_servicing_mode = 8,
    leave_servicing_mode = 9,
    restart_client = 10,
    decommission_client = 11,
    transfer_management = 12,
};

enum class UwfFeatureState : std::uint8_t {
    disabled = 1,
    enabled = 2,
};

enum class UwfFilterState : std::uint8_t {
    not_applicable = 0,
    disabled = 1,
    enabled = 2,
};

enum class UwfServicingState : std::uint8_t {
    not_applicable = 0,
    inactive = 1,
    active = 2,
};

enum class RestartPath : std::uint8_t {
    windows = 1,
    uwf = 2,
};

enum class DecommissionDisposition : std::uint8_t {
    retain_uwf_configuration = 1,
    disable_protection = 2,
    remove_uwf_feature = 3,
};

struct RestoreState {
    UwfFeatureState feature = UwfFeatureState::disabled;
    UwfFilterState current_filter = UwfFilterState::not_applicable;
    UwfFilterState next_filter = UwfFilterState::not_applicable;
    UwfServicingState servicing = UwfServicingState::not_applicable;
    std::uint64_t policy_revision = 0;

    bool operator==(const RestoreState&) const = default;
};

struct NoMaintenanceParameters {
    bool operator==(const NoMaintenanceParameters&) const = default;
};

struct VolumeMaintenanceParameters {
    MaintenanceVolumeId volume_id{};

    bool operator==(const VolumeMaintenanceParameters&) const = default;
};

struct PolicyMaintenanceParameters {
    security::Sha256Digest policy_digest{};

    bool operator==(const PolicyMaintenanceParameters&) const = default;
};

struct RestartMaintenanceParameters {
    RestartPath path = RestartPath::windows;
    std::uint32_t grace_seconds = 0;

    bool operator==(const RestartMaintenanceParameters&) const = default;
};

struct DecommissionMaintenanceParameters {
    DecommissionDisposition disposition =
        DecommissionDisposition::retain_uwf_configuration;

    bool operator==(const DecommissionMaintenanceParameters&) const = default;
};

struct TransferMaintenanceParameters {
    MaintenanceAuthorityId new_authority_id{};
    std::uint32_t new_key_id = 0;
    security::Sha256Digest new_key_digest{};

    bool operator==(const TransferMaintenanceParameters&) const = default;
};

using MaintenanceParameters = std::variant<
    NoMaintenanceParameters, VolumeMaintenanceParameters,
    PolicyMaintenanceParameters, RestartMaintenanceParameters,
    DecommissionMaintenanceParameters, TransferMaintenanceParameters>;

struct MaintenanceIntent {
    MaintenanceIntentId intent_id{};
    security::ClientId target_client{};
    security::Nonce nonce{};
    std::uint32_t deployment_key_id = 0;
    std::uint64_t issued_at_unix_seconds = 0;
    std::uint64_t expires_at_unix_seconds = 0;
    std::uint64_t policy_revision = 0;
    RestoreState expected_state{};
    MaintenanceOperation operation =
        MaintenanceOperation::install_uwf_feature;
    MaintenanceParameters parameters = NoMaintenanceParameters{};

    bool operator==(const MaintenanceIntent&) const = default;
};

struct SignedMaintenanceIntent {
    MaintenanceIntent intent;
    MaintenanceAuthTag tag{};

    bool operator==(const SignedMaintenanceIntent&) const = default;
};

// This distinct view prevents maintenance code from accidentally accepting a
// live teacher/control session key through an untyped byte-span API. Key
// storage and role assignment remain deployment responsibilities.
struct DeploymentAdminCredentialView {
    std::uint32_t key_id = 0;
    std::span<const std::byte> key;
};

[[nodiscard]] bool validate_restore_state(
    const RestoreState& state) noexcept;
[[nodiscard]] bool validate_maintenance_intent_shape(
    const MaintenanceIntent& intent) noexcept;

[[nodiscard]] std::vector<std::byte> encode_maintenance_intent(
    const MaintenanceIntent& intent);
[[nodiscard]] std::optional<MaintenanceIntent> decode_maintenance_intent(
    std::span<const std::byte> wire);

[[nodiscard]] std::optional<MaintenanceAuthTag>
compute_maintenance_intent_tag(
    const DeploymentAdminCredentialView& credential,
    const MaintenanceIntent& intent);
[[nodiscard]] bool verify_maintenance_intent_tag(
    const DeploymentAdminCredentialView& credential,
    const SignedMaintenanceIntent& signed_intent);

[[nodiscard]] std::optional<SignedMaintenanceIntent>
sign_maintenance_intent(
    const DeploymentAdminCredentialView& credential,
    const MaintenanceIntent& intent);
[[nodiscard]] std::vector<std::byte> encode_signed_maintenance_intent(
    const SignedMaintenanceIntent& signed_intent);
[[nodiscard]] std::optional<SignedMaintenanceIntent>
decode_signed_maintenance_intent(std::span<const std::byte> wire);

enum class MaintenanceReplayResult {
    accepted,
    invalid,
    replayed,
    capacity_exhausted,
};

class MaintenanceReplayProtector {
public:
    explicit MaintenanceReplayProtector(std::size_t capacity = 4096);

    // Call only after the HMAC and local target/state binding are verified.
    // This process-local cache is not a durable maintenance transaction log;
    // the future execution layer must persist accepted intents across reboot.
    [[nodiscard]] MaintenanceReplayResult accept(
        const MaintenanceIntent& intent, std::uint64_t now_unix_seconds);
    void clear();
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        std::uint64_t expires_at = 0;
        std::string nonce_key;
    };

    std::size_t capacity_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> intents_;
    std::unordered_map<std::string, std::uint64_t> nonces_;
};

enum class MaintenanceAuthorizationResult {
    authorized,
    invalid_intent,
    invalid_credential,
    credential_mismatch,
    authentication_failed,
    invalid_local_context,
    not_yet_valid,
    expired,
    target_mismatch,
    expected_state_mismatch,
    replayed,
    replay_capacity_exhausted,
};

struct MaintenanceAuthorizationDecision;

class AuthorizedMaintenanceIntent {
public:
    AuthorizedMaintenanceIntent(const AuthorizedMaintenanceIntent&) = delete;
    AuthorizedMaintenanceIntent& operator=(
        const AuthorizedMaintenanceIntent&) = delete;
    AuthorizedMaintenanceIntent(AuthorizedMaintenanceIntent&&) noexcept =
        default;
    AuthorizedMaintenanceIntent& operator=(
        AuthorizedMaintenanceIntent&&) noexcept = default;

    [[nodiscard]] const MaintenanceIntent& intent() const noexcept {
        return intent_;
    }

private:
    explicit AuthorizedMaintenanceIntent(MaintenanceIntent intent)
        : intent_(std::move(intent)) {}

    friend MaintenanceAuthorizationDecision authorize_maintenance_intent(
        const DeploymentAdminCredentialView& credential,
        const SignedMaintenanceIntent& signed_intent,
        const security::ClientId& local_client_id,
        const RestoreState& current_state, std::uint64_t now_unix_seconds,
        MaintenanceReplayProtector& replay_protector);

    MaintenanceIntent intent_;
};

struct MaintenanceAuthorizationDecision {
    MaintenanceAuthorizationResult result =
        MaintenanceAuthorizationResult::invalid_intent;
    std::optional<AuthorizedMaintenanceIntent> authorized_intent;

    [[nodiscard]] bool authorized() const noexcept {
        return result == MaintenanceAuthorizationResult::authorized &&
               authorized_intent.has_value();
    }
};

[[nodiscard]] MaintenanceAuthorizationDecision authorize_maintenance_intent(
    const DeploymentAdminCredentialView& credential,
    const SignedMaintenanceIntent& signed_intent,
    const security::ClientId& local_client_id,
    const RestoreState& current_state, std::uint64_t now_unix_seconds,
    MaintenanceReplayProtector& replay_protector);

} // namespace nstu::maintenance
