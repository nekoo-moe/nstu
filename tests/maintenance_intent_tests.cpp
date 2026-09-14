#include "nstu/maintenance_intent.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

template <std::size_t Size>
std::array<std::byte, Size> filled(std::uint8_t seed) {
    std::array<std::byte, Size> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(seed + index);
    }
    return output;
}

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

nstu::maintenance::RestoreState enabled_state(std::uint64_t revision = 4) {
    using namespace nstu::maintenance;
    return RestoreState{
        .feature = UwfFeatureState::enabled,
        .current_filter = UwfFilterState::enabled,
        .next_filter = UwfFilterState::enabled,
        .servicing = UwfServicingState::inactive,
        .policy_revision = revision,
    };
}

nstu::maintenance::MaintenanceIntent base_intent() {
    using namespace nstu::maintenance;
    MaintenanceIntent intent;
    intent.intent_id = filled<kMaintenanceIntentIdBytes>(0x10);
    intent.target_client = filled<nstu::security::kClientIdBytes>(0x30);
    intent.nonce = filled<nstu::security::kNonceBytes>(0x50);
    intent.deployment_key_id = 7;
    intent.issued_at_unix_seconds = 1'000;
    intent.expires_at_unix_seconds = 1'300;
    intent.policy_revision = 5;
    intent.expected_state = enabled_state();
    intent.operation = MaintenanceOperation::apply_restore_policy;
    intent.parameters = PolicyMaintenanceParameters{
        filled<nstu::security::kSha256Bytes>(0x80)};
    return intent;
}

nstu::maintenance::DeploymentAdminCredentialView credential() {
    static constexpr std::string_view key =
        "deployment-administrator-key-material-v1";
    return {7, bytes(key)};
}

void assert_round_trip(const nstu::maintenance::MaintenanceIntent& intent) {
    using namespace nstu::maintenance;
    assert(validate_maintenance_intent_shape(intent));
    const auto wire = encode_maintenance_intent(intent);
    assert(!wire.empty());
    const auto decoded = decode_maintenance_intent(wire);
    assert(decoded.has_value());
    assert(*decoded == intent);
    assert(encode_maintenance_intent(*decoded) == wire);

    const auto signed_intent = sign_maintenance_intent(credential(), intent);
    assert(signed_intent.has_value());
    assert(verify_maintenance_intent_tag(credential(), *signed_intent));
    const auto signed_wire = encode_signed_maintenance_intent(*signed_intent);
    assert(!signed_wire.empty());
    const auto decoded_signed = decode_signed_maintenance_intent(signed_wire);
    assert(decoded_signed.has_value());
    assert(*decoded_signed == *signed_intent);
    assert(verify_maintenance_intent_tag(credential(), *decoded_signed));
}

} // namespace

int main() {
    using namespace nstu::maintenance;

    const auto intent = base_intent();
    assert(validate_restore_state(intent.expected_state));
    assert(validate_maintenance_intent_shape(intent));

    const auto wire = encode_maintenance_intent(intent);
    assert(wire.size() == kMaintenanceIntentHeaderBytes +
                              nstu::security::kSha256Bytes);
    assert(wire[0] == std::byte{'N'});
    assert(wire[1] == std::byte{'M'});
    assert(wire[2] == std::byte{'I'});
    assert(wire[3] == std::byte{'1'});
    assert(wire[4] == std::byte{1});
    assert(wire[5] == std::byte{0});
    assert(wire[8] == std::byte{7});
    assert(wire[9] == std::byte{0});
    const auto decoded = decode_maintenance_intent(wire);
    assert(decoded.has_value());
    assert(*decoded == intent);

    auto operation = intent;
    operation.operation = MaintenanceOperation::install_uwf_feature;
    operation.parameters = NoMaintenanceParameters{};
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::remove_uwf_feature;
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::enable_filter;
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::disable_filter;
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::enter_servicing_mode;
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::leave_servicing_mode;
    assert_round_trip(operation);

    operation.operation = MaintenanceOperation::protect_volume;
    operation.parameters = VolumeMaintenanceParameters{
        filled<kMaintenanceVolumeIdBytes>(0xa0)};
    assert_round_trip(operation);
    operation.operation = MaintenanceOperation::unprotect_volume;
    assert_round_trip(operation);

    operation.operation = MaintenanceOperation::apply_restore_policy;
    operation.parameters = PolicyMaintenanceParameters{
        filled<nstu::security::kSha256Bytes>(0xb0)};
    assert_round_trip(operation);

    operation.operation = MaintenanceOperation::restart_client;
    operation.parameters = RestartMaintenanceParameters{
        .path = RestartPath::uwf,
        .grace_seconds = kMaximumRestartGraceSeconds,
    };
    assert_round_trip(operation);

    operation.operation = MaintenanceOperation::decommission_client;
    operation.parameters = DecommissionMaintenanceParameters{
        DecommissionDisposition::disable_protection};
    assert_round_trip(operation);

    operation.operation = MaintenanceOperation::transfer_management;
    operation.parameters = TransferMaintenanceParameters{
        .new_authority_id = filled<kMaintenanceAuthorityIdBytes>(0xc0),
        .new_key_id = 91,
        .new_key_digest = filled<nstu::security::kSha256Bytes>(0xd0),
    };
    assert_round_trip(operation);

    auto invalid = intent;
    invalid.intent_id = {};
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.target_client = {};
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.nonce = {};
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.deployment_key_id = 0;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.issued_at_unix_seconds = 0;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.expires_at_unix_seconds = invalid.issued_at_unix_seconds - 1;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.expires_at_unix_seconds =
        invalid.issued_at_unix_seconds +
        kMaximumMaintenanceIntentLifetimeSeconds + 1;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.policy_revision = 0;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.policy_revision = invalid.expected_state.policy_revision - 1;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.operation = MaintenanceOperation::protect_volume;
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.operation = MaintenanceOperation::restart_client;
    invalid.parameters = RestartMaintenanceParameters{
        .path = RestartPath::windows,
        .grace_seconds = kMaximumRestartGraceSeconds + 1,
    };
    assert(!validate_maintenance_intent_shape(invalid));
    invalid = intent;
    invalid.operation = MaintenanceOperation::transfer_management;
    invalid.parameters = TransferMaintenanceParameters{};
    assert(!validate_maintenance_intent_shape(invalid));

    auto disabled = RestoreState{
        .feature = UwfFeatureState::disabled,
        .current_filter = UwfFilterState::not_applicable,
        .next_filter = UwfFilterState::not_applicable,
        .servicing = UwfServicingState::not_applicable,
        .policy_revision = 0,
    };
    assert(validate_restore_state(disabled));
    disabled.current_filter = UwfFilterState::disabled;
    assert(!validate_restore_state(disabled));
    auto bad_enabled = enabled_state();
    bad_enabled.servicing = UwfServicingState::not_applicable;
    assert(!validate_restore_state(bad_enabled));

    auto malformed = wire;
    malformed[4] = std::byte{2};
    assert(!decode_maintenance_intent(malformed).has_value());
    malformed = wire;
    malformed[10] = std::byte{1};
    assert(!decode_maintenance_intent(malformed).has_value());
    malformed = wire;
    malformed[120] = std::byte{1};
    assert(!decode_maintenance_intent(malformed).has_value());
    malformed = wire;
    malformed.push_back(std::byte{0});
    assert(!decode_maintenance_intent(malformed).has_value());
    assert(!decode_maintenance_intent({}).has_value());

    auto restart = intent;
    restart.operation = MaintenanceOperation::restart_client;
    restart.parameters = RestartMaintenanceParameters{};
    auto restart_wire = encode_maintenance_intent(restart);
    restart_wire[kMaintenanceIntentHeaderBytes + 1] = std::byte{1};
    assert(!decode_maintenance_intent(restart_wire).has_value());

    const auto signed_intent = sign_maintenance_intent(credential(), intent);
    assert(signed_intent.has_value());
    assert(verify_maintenance_intent_tag(credential(), *signed_intent));
    const auto raw_hmac = nstu::security::hmac_sha256(credential().key, wire);
    assert(raw_hmac.has_value());
    assert(!nstu::security::constant_time_equal(*raw_hmac,
                                                signed_intent->tag));

    const DeploymentAdminCredentialView short_credential{7, bytes("short")};
    const DeploymentAdminCredentialView wrong_id_credential{
        8, credential().key};
    assert(!sign_maintenance_intent(short_credential, intent).has_value());
    assert(!sign_maintenance_intent(wrong_id_credential, intent).has_value());
    assert(!verify_maintenance_intent_tag(short_credential, *signed_intent));
    assert(!verify_maintenance_intent_tag(wrong_id_credential,
                                          *signed_intent));

    auto tampered_signature = *signed_intent;
    tampered_signature.tag[0] ^= std::byte{1};
    assert(!verify_maintenance_intent_tag(credential(), tampered_signature));

    auto signed_wire = encode_signed_maintenance_intent(*signed_intent);
    assert(signed_wire.size() == wire.size() + kMaintenanceAuthTagBytes);
    const auto authenticated_wire = signed_wire;
    for (std::size_t index = 0; index < authenticated_wire.size(); ++index) {
        auto single_byte_tamper = authenticated_wire;
        single_byte_tamper[index] ^= std::byte{1};
        const auto parsed =
            decode_signed_maintenance_intent(single_byte_tamper);
        assert(!parsed ||
               !verify_maintenance_intent_tag(credential(), *parsed));
    }
    signed_wire[100] ^= std::byte{1};
    const auto tampered_body =
        decode_signed_maintenance_intent(signed_wire);
    assert(tampered_body.has_value());
    assert(!verify_maintenance_intent_tag(credential(), *tampered_body));
    signed_wire.push_back(std::byte{0});
    assert(!decode_signed_maintenance_intent(signed_wire).has_value());
    assert(!decode_signed_maintenance_intent({}).has_value());

    MaintenanceReplayProtector authorization_replay(16);
    assert(authorize_maintenance_intent(
               short_credential, *signed_intent, intent.target_client,
               intent.expected_state, 1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::invalid_credential);
    assert(authorize_maintenance_intent(
               wrong_id_credential, *signed_intent, intent.target_client,
               intent.expected_state, 1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::credential_mismatch);
    assert(authorize_maintenance_intent(
               credential(), tampered_signature, intent.target_client,
               intent.expected_state, 1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::authentication_failed);

    auto wrong_target = intent.target_client;
    wrong_target[0] ^= std::byte{1};
    assert(authorize_maintenance_intent(
               credential(), *signed_intent, wrong_target,
               intent.expected_state, 1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::target_mismatch);
    auto wrong_state = intent.expected_state;
    wrong_state.next_filter = UwfFilterState::disabled;
    assert(authorize_maintenance_intent(
               credential(), *signed_intent, intent.target_client,
               wrong_state, 1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::expected_state_mismatch);
    assert(authorize_maintenance_intent(
               credential(), *signed_intent, {}, intent.expected_state,
               1'100, authorization_replay).result ==
           MaintenanceAuthorizationResult::invalid_local_context);
    assert(authorize_maintenance_intent(
               credential(), *signed_intent, intent.target_client,
               intent.expected_state, 1'301, authorization_replay).result ==
           MaintenanceAuthorizationResult::expired);

    auto future_intent = intent;
    future_intent.issued_at_unix_seconds =
        1'100 + kMaintenanceClockSkewSeconds + 1;
    future_intent.expires_at_unix_seconds =
        future_intent.issued_at_unix_seconds + 60;
    const auto future_signed =
        sign_maintenance_intent(credential(), future_intent);
    assert(future_signed.has_value());
    assert(authorize_maintenance_intent(
               credential(), *future_signed, future_intent.target_client,
               future_intent.expected_state, 1'100,
               authorization_replay).result ==
           MaintenanceAuthorizationResult::not_yet_valid);

    auto authorized = authorize_maintenance_intent(
        credential(), *signed_intent, intent.target_client,
        intent.expected_state, intent.expires_at_unix_seconds,
        authorization_replay);
    assert(authorized.authorized());
    assert(authorized.result == MaintenanceAuthorizationResult::authorized);
    assert(authorized.authorized_intent->intent() == intent);
    assert(authorize_maintenance_intent(
               credential(), *signed_intent, intent.target_client,
               intent.expected_state, intent.expires_at_unix_seconds,
               authorization_replay).result ==
           MaintenanceAuthorizationResult::replayed);

    MaintenanceReplayProtector replay(2);
    auto first = intent;
    auto second = intent;
    second.intent_id[0] ^= std::byte{1};
    second.nonce[0] ^= std::byte{1};
    auto third = intent;
    third.intent_id[0] ^= std::byte{2};
    third.nonce[0] ^= std::byte{2};
    assert(replay.accept(first, 1'100) == MaintenanceReplayResult::accepted);
    assert(replay.accept(first, 1'100) == MaintenanceReplayResult::replayed);
    assert(replay.accept(second, 1'100) == MaintenanceReplayResult::accepted);
    assert(replay.size() == 2);
    assert(replay.accept(third, 1'100) ==
           MaintenanceReplayResult::capacity_exhausted);

    auto reused_intent_id = first;
    reused_intent_id.nonce[0] ^= std::byte{9};
    assert(replay.accept(reused_intent_id, 1'100) ==
           MaintenanceReplayResult::replayed);
    auto reused_nonce = first;
    reused_nonce.intent_id[0] ^= std::byte{9};
    assert(replay.accept(reused_nonce, 1'100) ==
           MaintenanceReplayResult::replayed);
    auto rotated_key_same_identity = first;
    rotated_key_same_identity.deployment_key_id = 8;
    assert(replay.accept(rotated_key_same_identity, 1'100) ==
           MaintenanceReplayResult::replayed);

    third.issued_at_unix_seconds = 1'301;
    third.expires_at_unix_seconds = 1'361;
    assert(replay.accept(third, 1'301) == MaintenanceReplayResult::accepted);
    assert(replay.size() == 1);
    replay.clear();
    assert(replay.size() == 0);

    MaintenanceReplayProtector minimum_capacity(0);
    assert(minimum_capacity.accept(first, 1'100) ==
           MaintenanceReplayResult::accepted);
    assert(minimum_capacity.accept(second, 1'100) ==
           MaintenanceReplayResult::capacity_exhausted);

    return 0;
}
