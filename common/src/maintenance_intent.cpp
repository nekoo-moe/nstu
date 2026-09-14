#include "nstu/maintenance_intent.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::maintenance {
namespace {

inline constexpr std::uint32_t kMaintenanceIntentMagic =
    0x31494d4eu; // "NMI1"
inline constexpr std::uint16_t kMaintenanceIntentVersion = 1;
inline constexpr std::uint16_t kMaintenanceIntentFlags = 0;
inline constexpr std::size_t kRestartParameterBytes = 8;
inline constexpr std::size_t kDecommissionParameterBytes = 4;
inline constexpr std::size_t kTransferParameterBytes =
    kMaintenanceAuthorityIdBytes + sizeof(std::uint32_t) +
    security::kSha256Bytes;
inline constexpr std::size_t kComputedMaintenanceIntentHeaderBytes =
    sizeof(std::uint32_t) * 4 + sizeof(std::uint16_t) * 4 +
    kMaintenanceIntentIdBytes + security::kClientIdBytes +
    security::kNonceBytes + sizeof(std::uint64_t) * 4 +
    sizeof(std::uint8_t) * 4;

static_assert(kComputedMaintenanceIntentHeaderBytes ==
              kMaintenanceIntentHeaderBytes);
static_assert(kTransferParameterBytes <= kMaximumMaintenanceParameterBytes);

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || input.size() - offset < sizeof(T)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

template <std::size_t Size>
bool read_array(std::span<const std::byte> input, std::size_t& offset,
                std::array<std::byte, Size>& output) {
    if (offset > input.size() || input.size() - offset < output.size()) {
        return false;
    }
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                output.size(), output.begin());
    offset += output.size();
    return true;
}

void append_bytes(std::vector<std::byte>& output,
                  std::span<const std::byte> input) {
    output.insert(output.end(), input.begin(), input.end());
}

bool non_zero(std::span<const std::byte> value) noexcept {
    return !value.empty() && std::any_of(
        value.begin(), value.end(),
        [](std::byte byte) { return byte != std::byte{0}; });
}

bool valid_feature_state(UwfFeatureState state) noexcept {
    return state == UwfFeatureState::disabled ||
           state == UwfFeatureState::enabled;
}

bool valid_filter_state(UwfFilterState state) noexcept {
    return state == UwfFilterState::not_applicable ||
           state == UwfFilterState::disabled ||
           state == UwfFilterState::enabled;
}

bool valid_servicing_state(UwfServicingState state) noexcept {
    return state == UwfServicingState::not_applicable ||
           state == UwfServicingState::inactive ||
           state == UwfServicingState::active;
}

bool valid_restart_path(RestartPath path) noexcept {
    return path == RestartPath::windows || path == RestartPath::uwf;
}

bool valid_decommission_disposition(
    DecommissionDisposition disposition) noexcept {
    return disposition == DecommissionDisposition::retain_uwf_configuration ||
           disposition == DecommissionDisposition::disable_protection ||
           disposition == DecommissionDisposition::remove_uwf_feature;
}

bool valid_credential(
    const DeploymentAdminCredentialView& credential) noexcept {
    return credential.key_id != 0 &&
           credential.key.size() >= security::kMinimumProtocolKeyBytes;
}

bool no_parameter_operation(MaintenanceOperation operation) noexcept {
    switch (operation) {
    case MaintenanceOperation::install_uwf_feature:
    case MaintenanceOperation::remove_uwf_feature:
    case MaintenanceOperation::enable_filter:
    case MaintenanceOperation::disable_filter:
    case MaintenanceOperation::enter_servicing_mode:
    case MaintenanceOperation::leave_servicing_mode:
        return true;
    default:
        return false;
    }
}

bool valid_operation_parameters(const MaintenanceIntent& intent) noexcept {
    if (no_parameter_operation(intent.operation)) {
        return std::holds_alternative<NoMaintenanceParameters>(
            intent.parameters);
    }

    switch (intent.operation) {
    case MaintenanceOperation::protect_volume:
    case MaintenanceOperation::unprotect_volume: {
        const auto* parameters =
            std::get_if<VolumeMaintenanceParameters>(&intent.parameters);
        return parameters != nullptr && non_zero(parameters->volume_id);
    }
    case MaintenanceOperation::apply_restore_policy: {
        const auto* parameters =
            std::get_if<PolicyMaintenanceParameters>(&intent.parameters);
        return parameters != nullptr && non_zero(parameters->policy_digest);
    }
    case MaintenanceOperation::restart_client: {
        const auto* parameters =
            std::get_if<RestartMaintenanceParameters>(&intent.parameters);
        return parameters != nullptr && valid_restart_path(parameters->path) &&
               parameters->grace_seconds <= kMaximumRestartGraceSeconds;
    }
    case MaintenanceOperation::decommission_client: {
        const auto* parameters =
            std::get_if<DecommissionMaintenanceParameters>(
                &intent.parameters);
        return parameters != nullptr &&
               valid_decommission_disposition(parameters->disposition);
    }
    case MaintenanceOperation::transfer_management: {
        const auto* parameters =
            std::get_if<TransferMaintenanceParameters>(&intent.parameters);
        return parameters != nullptr &&
               non_zero(parameters->new_authority_id) &&
               parameters->new_key_id != 0 &&
               non_zero(parameters->new_key_digest);
    }
    default:
        return false;
    }
}

std::optional<std::vector<std::byte>> encode_parameters(
    const MaintenanceIntent& intent) {
    if (!valid_operation_parameters(intent)) {
        return std::nullopt;
    }

    std::vector<std::byte> output;
    if (no_parameter_operation(intent.operation)) {
        return output;
    }

    switch (intent.operation) {
    case MaintenanceOperation::protect_volume:
    case MaintenanceOperation::unprotect_volume:
        append_bytes(output,
                     std::get<VolumeMaintenanceParameters>(intent.parameters)
                         .volume_id);
        break;
    case MaintenanceOperation::apply_restore_policy:
        append_bytes(output,
                     std::get<PolicyMaintenanceParameters>(intent.parameters)
                         .policy_digest);
        break;
    case MaintenanceOperation::restart_client: {
        const auto& parameters =
            std::get<RestartMaintenanceParameters>(intent.parameters);
        append_le(output, static_cast<std::uint8_t>(parameters.path));
        append_le(output, std::uint8_t{0});
        append_le(output, std::uint8_t{0});
        append_le(output, std::uint8_t{0});
        append_le(output, parameters.grace_seconds);
        break;
    }
    case MaintenanceOperation::decommission_client: {
        const auto& parameters =
            std::get<DecommissionMaintenanceParameters>(intent.parameters);
        append_le(output, static_cast<std::uint8_t>(parameters.disposition));
        append_le(output, std::uint8_t{0});
        append_le(output, std::uint8_t{0});
        append_le(output, std::uint8_t{0});
        break;
    }
    case MaintenanceOperation::transfer_management: {
        const auto& parameters =
            std::get<TransferMaintenanceParameters>(intent.parameters);
        append_bytes(output, parameters.new_authority_id);
        append_le(output, parameters.new_key_id);
        append_bytes(output, parameters.new_key_digest);
        break;
    }
    default:
        return std::nullopt;
    }
    return output;
}

std::optional<MaintenanceParameters> decode_parameters(
    MaintenanceOperation operation, std::span<const std::byte> wire) {
    if (no_parameter_operation(operation)) {
        if (!wire.empty()) {
            return std::nullopt;
        }
        return NoMaintenanceParameters{};
    }

    switch (operation) {
    case MaintenanceOperation::protect_volume:
    case MaintenanceOperation::unprotect_volume: {
        if (wire.size() != kMaintenanceVolumeIdBytes) {
            return std::nullopt;
        }
        VolumeMaintenanceParameters parameters;
        std::copy(wire.begin(), wire.end(), parameters.volume_id.begin());
        return parameters;
    }
    case MaintenanceOperation::apply_restore_policy: {
        if (wire.size() != security::kSha256Bytes) {
            return std::nullopt;
        }
        PolicyMaintenanceParameters parameters;
        std::copy(wire.begin(), wire.end(), parameters.policy_digest.begin());
        return parameters;
    }
    case MaintenanceOperation::restart_client: {
        if (wire.size() != kRestartParameterBytes) {
            return std::nullopt;
        }
        std::size_t offset = 0;
        std::uint8_t path = 0;
        std::uint8_t reserved_one = 0;
        std::uint8_t reserved_two = 0;
        std::uint8_t reserved_three = 0;
        RestartMaintenanceParameters parameters;
        if (!read_le(wire, offset, path) ||
            !read_le(wire, offset, reserved_one) ||
            !read_le(wire, offset, reserved_two) ||
            !read_le(wire, offset, reserved_three) ||
            !read_le(wire, offset, parameters.grace_seconds) ||
            reserved_one != 0 || reserved_two != 0 || reserved_three != 0) {
            return std::nullopt;
        }
        parameters.path = static_cast<RestartPath>(path);
        return parameters;
    }
    case MaintenanceOperation::decommission_client: {
        if (wire.size() != kDecommissionParameterBytes) {
            return std::nullopt;
        }
        std::size_t offset = 0;
        std::uint8_t disposition = 0;
        std::uint8_t reserved_one = 0;
        std::uint8_t reserved_two = 0;
        std::uint8_t reserved_three = 0;
        if (!read_le(wire, offset, disposition) ||
            !read_le(wire, offset, reserved_one) ||
            !read_le(wire, offset, reserved_two) ||
            !read_le(wire, offset, reserved_three) ||
            reserved_one != 0 || reserved_two != 0 || reserved_three != 0) {
            return std::nullopt;
        }
        return DecommissionMaintenanceParameters{
            static_cast<DecommissionDisposition>(disposition)};
    }
    case MaintenanceOperation::transfer_management: {
        if (wire.size() != kTransferParameterBytes) {
            return std::nullopt;
        }
        std::size_t offset = 0;
        TransferMaintenanceParameters parameters;
        if (!read_array(wire, offset, parameters.new_authority_id) ||
            !read_le(wire, offset, parameters.new_key_id) ||
            !read_array(wire, offset, parameters.new_key_digest) ||
            offset != wire.size()) {
            return std::nullopt;
        }
        return parameters;
    }
    default:
        return std::nullopt;
    }
}

void append_binary(std::string& output, std::span<const std::byte> value) {
    output.append(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string replay_intent_key(const MaintenanceIntent& intent) {
    std::string key;
    key.reserve(1 + intent.target_client.size() + intent.intent_id.size());
    key.push_back('\x01');
    append_binary(key, intent.target_client);
    append_binary(key, intent.intent_id);
    return key;
}

std::string replay_nonce_key(const MaintenanceIntent& intent) {
    std::string key;
    key.reserve(1 + intent.target_client.size() + intent.nonce.size());
    key.push_back('\x02');
    append_binary(key, intent.target_client);
    append_binary(key, intent.nonce);
    return key;
}

} // namespace

bool validate_restore_state(const RestoreState& state) noexcept {
    if (!valid_feature_state(state.feature) ||
        !valid_filter_state(state.current_filter) ||
        !valid_filter_state(state.next_filter) ||
        !valid_servicing_state(state.servicing)) {
        return false;
    }

    if (state.feature == UwfFeatureState::disabled) {
        return state.current_filter == UwfFilterState::not_applicable &&
               state.next_filter == UwfFilterState::not_applicable &&
               state.servicing == UwfServicingState::not_applicable;
    }
    return state.current_filter != UwfFilterState::not_applicable &&
           state.next_filter != UwfFilterState::not_applicable &&
           state.servicing != UwfServicingState::not_applicable;
}

bool validate_maintenance_intent_shape(
    const MaintenanceIntent& intent) noexcept {
    if (!non_zero(intent.intent_id) || !non_zero(intent.target_client) ||
        !non_zero(intent.nonce) || intent.deployment_key_id == 0 ||
        intent.issued_at_unix_seconds == 0 ||
        intent.expires_at_unix_seconds < intent.issued_at_unix_seconds ||
        intent.expires_at_unix_seconds - intent.issued_at_unix_seconds >
            kMaximumMaintenanceIntentLifetimeSeconds ||
        intent.policy_revision == 0 ||
        intent.policy_revision < intent.expected_state.policy_revision ||
        !validate_restore_state(intent.expected_state)) {
        return false;
    }
    return valid_operation_parameters(intent);
}

std::vector<std::byte> encode_maintenance_intent(
    const MaintenanceIntent& intent) {
    if (!validate_maintenance_intent_shape(intent)) {
        return {};
    }
    const auto parameters = encode_parameters(intent);
    if (!parameters ||
        parameters->size() > kMaximumMaintenanceParameterBytes ||
        parameters->size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::vector<std::byte> output;
    output.reserve(kMaintenanceIntentHeaderBytes + parameters->size());
    append_le(output, kMaintenanceIntentMagic);
    append_le(output, kMaintenanceIntentVersion);
    append_le(output,
              static_cast<std::uint16_t>(kMaintenanceIntentHeaderBytes));
    append_le(output, static_cast<std::uint16_t>(intent.operation));
    append_le(output, kMaintenanceIntentFlags);
    append_le(output, static_cast<std::uint32_t>(parameters->size()));
    append_le(output, intent.deployment_key_id);
    append_bytes(output, intent.intent_id);
    append_bytes(output, intent.target_client);
    append_bytes(output, intent.nonce);
    append_le(output, intent.issued_at_unix_seconds);
    append_le(output, intent.expires_at_unix_seconds);
    append_le(output, intent.policy_revision);
    append_le(output, intent.expected_state.policy_revision);
    append_le(output, static_cast<std::uint8_t>(intent.expected_state.feature));
    append_le(output,
              static_cast<std::uint8_t>(intent.expected_state.current_filter));
    append_le(output,
              static_cast<std::uint8_t>(intent.expected_state.next_filter));
    append_le(output,
              static_cast<std::uint8_t>(intent.expected_state.servicing));
    append_le(output, std::uint32_t{0});
    append_bytes(output, *parameters);
    return output;
}

std::optional<MaintenanceIntent> decode_maintenance_intent(
    std::span<const std::byte> wire) {
    if (wire.size() < kMaintenanceIntentHeaderBytes ||
        wire.size() >
            kMaintenanceIntentHeaderBytes + kMaximumMaintenanceParameterBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint16_t operation = 0;
    std::uint16_t flags = 0;
    std::uint32_t parameter_bytes = 0;
    std::uint8_t feature = 0;
    std::uint8_t current_filter = 0;
    std::uint8_t next_filter = 0;
    std::uint8_t servicing = 0;
    std::uint32_t reserved = 0;
    MaintenanceIntent intent;
    if (!read_le(wire, offset, magic) || !read_le(wire, offset, version) ||
        !read_le(wire, offset, header_bytes) ||
        !read_le(wire, offset, operation) || !read_le(wire, offset, flags) ||
        !read_le(wire, offset, parameter_bytes) ||
        !read_le(wire, offset, intent.deployment_key_id) ||
        !read_array(wire, offset, intent.intent_id) ||
        !read_array(wire, offset, intent.target_client) ||
        !read_array(wire, offset, intent.nonce) ||
        !read_le(wire, offset, intent.issued_at_unix_seconds) ||
        !read_le(wire, offset, intent.expires_at_unix_seconds) ||
        !read_le(wire, offset, intent.policy_revision) ||
        !read_le(wire, offset, intent.expected_state.policy_revision) ||
        !read_le(wire, offset, feature) ||
        !read_le(wire, offset, current_filter) ||
        !read_le(wire, offset, next_filter) ||
        !read_le(wire, offset, servicing) ||
        !read_le(wire, offset, reserved) ||
        magic != kMaintenanceIntentMagic ||
        version != kMaintenanceIntentVersion ||
        header_bytes != kMaintenanceIntentHeaderBytes ||
        flags != kMaintenanceIntentFlags || reserved != 0 ||
        parameter_bytes > kMaximumMaintenanceParameterBytes ||
        wire.size() - kMaintenanceIntentHeaderBytes != parameter_bytes ||
        offset != kMaintenanceIntentHeaderBytes) {
        return std::nullopt;
    }

    intent.operation = static_cast<MaintenanceOperation>(operation);
    intent.expected_state.feature = static_cast<UwfFeatureState>(feature);
    intent.expected_state.current_filter =
        static_cast<UwfFilterState>(current_filter);
    intent.expected_state.next_filter =
        static_cast<UwfFilterState>(next_filter);
    intent.expected_state.servicing =
        static_cast<UwfServicingState>(servicing);
    auto parameters = decode_parameters(
        intent.operation, wire.subspan(kMaintenanceIntentHeaderBytes));
    if (!parameters) {
        return std::nullopt;
    }
    intent.parameters = std::move(*parameters);
    if (!validate_maintenance_intent_shape(intent)) {
        return std::nullopt;
    }
    return intent;
}

std::optional<MaintenanceAuthTag> compute_maintenance_intent_tag(
    const DeploymentAdminCredentialView& credential,
    const MaintenanceIntent& intent) {
    if (!valid_credential(credential) ||
        credential.key_id != intent.deployment_key_id) {
        return std::nullopt;
    }
    const auto wire = encode_maintenance_intent(intent);
    if (wire.empty()) {
        return std::nullopt;
    }

    constexpr char label[] = "NSTU-DEPLOYMENT-MAINTENANCE-INTENT-V1";
    std::vector<std::byte> message;
    message.reserve(sizeof(label) - 1 + wire.size());
    message.insert(message.end(),
                   reinterpret_cast<const std::byte*>(label),
                   reinterpret_cast<const std::byte*>(label) +
                       sizeof(label) - 1);
    append_bytes(message, wire);
    const auto digest = security::hmac_sha256(credential.key, message);
    if (!digest) {
        return std::nullopt;
    }
    MaintenanceAuthTag tag{};
    std::copy(digest->begin(), digest->end(), tag.begin());
    return tag;
}

bool verify_maintenance_intent_tag(
    const DeploymentAdminCredentialView& credential,
    const SignedMaintenanceIntent& signed_intent) {
    const auto expected =
        compute_maintenance_intent_tag(credential, signed_intent.intent);
    return expected && security::constant_time_equal(*expected,
                                                      signed_intent.tag);
}

std::optional<SignedMaintenanceIntent> sign_maintenance_intent(
    const DeploymentAdminCredentialView& credential,
    const MaintenanceIntent& intent) {
    const auto tag = compute_maintenance_intent_tag(credential, intent);
    if (!tag) {
        return std::nullopt;
    }
    return SignedMaintenanceIntent{intent, *tag};
}

std::vector<std::byte> encode_signed_maintenance_intent(
    const SignedMaintenanceIntent& signed_intent) {
    auto output = encode_maintenance_intent(signed_intent.intent);
    if (output.empty()) {
        return {};
    }
    append_bytes(output, signed_intent.tag);
    return output;
}

std::optional<SignedMaintenanceIntent> decode_signed_maintenance_intent(
    std::span<const std::byte> wire) {
    if (wire.size() <
        kMaintenanceIntentHeaderBytes + kMaintenanceAuthTagBytes) {
        return std::nullopt;
    }

    std::size_t parameter_offset = 12;
    std::uint32_t parameter_bytes = 0;
    if (!read_le(wire, parameter_offset, parameter_bytes) ||
        parameter_bytes > kMaximumMaintenanceParameterBytes ||
        parameter_bytes >
            std::numeric_limits<std::size_t>::max() -
                kMaintenanceIntentHeaderBytes) {
        return std::nullopt;
    }
    const auto intent_bytes = kMaintenanceIntentHeaderBytes + parameter_bytes;
    if (wire.size() != intent_bytes + kMaintenanceAuthTagBytes) {
        return std::nullopt;
    }

    auto intent = decode_maintenance_intent(wire.first(intent_bytes));
    if (!intent) {
        return std::nullopt;
    }
    SignedMaintenanceIntent signed_intent;
    signed_intent.intent = std::move(*intent);
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(intent_bytes),
                signed_intent.tag.size(), signed_intent.tag.begin());
    return signed_intent;
}

MaintenanceReplayProtector::MaintenanceReplayProtector(std::size_t capacity)
    : capacity_(std::clamp<std::size_t>(
          capacity, 1, kMaximumMaintenanceReplayEntries)) {}

MaintenanceReplayResult MaintenanceReplayProtector::accept(
    const MaintenanceIntent& intent, std::uint64_t now_unix_seconds) {
    if (!validate_maintenance_intent_shape(intent) || now_unix_seconds == 0 ||
        intent.expires_at_unix_seconds < now_unix_seconds) {
        return MaintenanceReplayResult::invalid;
    }

    const auto intent_key = replay_intent_key(intent);
    const auto nonce_key = replay_nonce_key(intent);
    std::scoped_lock lock(mutex_);
    for (auto iterator = intents_.begin(); iterator != intents_.end();) {
        if (iterator->second.expires_at < now_unix_seconds) {
            nonces_.erase(iterator->second.nonce_key);
            iterator = intents_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    if (intents_.contains(intent_key) || nonces_.contains(nonce_key)) {
        return MaintenanceReplayResult::replayed;
    }
    if (intents_.size() >= capacity_) {
        return MaintenanceReplayResult::capacity_exhausted;
    }

    auto [intent_iterator, inserted] = intents_.emplace(
        intent_key,
        Entry{intent.expires_at_unix_seconds, nonce_key});
    if (!inserted) {
        return MaintenanceReplayResult::replayed;
    }
    try {
        const auto [nonce_iterator, nonce_inserted] =
            nonces_.emplace(nonce_key, intent.expires_at_unix_seconds);
        static_cast<void>(nonce_iterator);
        if (!nonce_inserted) {
            intents_.erase(intent_iterator);
            return MaintenanceReplayResult::replayed;
        }
    } catch (...) {
        intents_.erase(intent_iterator);
        throw;
    }
    return MaintenanceReplayResult::accepted;
}

void MaintenanceReplayProtector::clear() {
    std::scoped_lock lock(mutex_);
    intents_.clear();
    nonces_.clear();
}

std::size_t MaintenanceReplayProtector::size() const {
    std::scoped_lock lock(mutex_);
    return intents_.size();
}

MaintenanceAuthorizationDecision authorize_maintenance_intent(
    const DeploymentAdminCredentialView& credential,
    const SignedMaintenanceIntent& signed_intent,
    const security::ClientId& local_client_id,
    const RestoreState& current_state, std::uint64_t now_unix_seconds,
    MaintenanceReplayProtector& replay_protector) {
    const auto& intent = signed_intent.intent;
    if (!validate_maintenance_intent_shape(intent)) {
        return {MaintenanceAuthorizationResult::invalid_intent, std::nullopt};
    }
    if (!valid_credential(credential)) {
        return {MaintenanceAuthorizationResult::invalid_credential,
                std::nullopt};
    }
    if (credential.key_id != intent.deployment_key_id) {
        return {MaintenanceAuthorizationResult::credential_mismatch,
                std::nullopt};
    }
    if (!verify_maintenance_intent_tag(credential, signed_intent)) {
        return {MaintenanceAuthorizationResult::authentication_failed,
                std::nullopt};
    }
    if (!non_zero(local_client_id) || !validate_restore_state(current_state) ||
        now_unix_seconds == 0) {
        return {MaintenanceAuthorizationResult::invalid_local_context,
                std::nullopt};
    }
    if (intent.issued_at_unix_seconds > now_unix_seconds &&
        intent.issued_at_unix_seconds - now_unix_seconds >
            kMaintenanceClockSkewSeconds) {
        return {MaintenanceAuthorizationResult::not_yet_valid, std::nullopt};
    }
    if (now_unix_seconds > intent.expires_at_unix_seconds) {
        return {MaintenanceAuthorizationResult::expired, std::nullopt};
    }
    if (intent.target_client != local_client_id) {
        return {MaintenanceAuthorizationResult::target_mismatch, std::nullopt};
    }
    if (intent.expected_state != current_state) {
        return {MaintenanceAuthorizationResult::expected_state_mismatch,
                std::nullopt};
    }

    switch (replay_protector.accept(intent, now_unix_seconds)) {
    case MaintenanceReplayResult::accepted:
        return {MaintenanceAuthorizationResult::authorized,
                AuthorizedMaintenanceIntent(intent)};
    case MaintenanceReplayResult::replayed:
        return {MaintenanceAuthorizationResult::replayed, std::nullopt};
    case MaintenanceReplayResult::capacity_exhausted:
        return {MaintenanceAuthorizationResult::replay_capacity_exhausted,
                std::nullopt};
    case MaintenanceReplayResult::invalid:
    default:
        return {MaintenanceAuthorizationResult::invalid_intent, std::nullopt};
    }
}

} // namespace nstu::maintenance
