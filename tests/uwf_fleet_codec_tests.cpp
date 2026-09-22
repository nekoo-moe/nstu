#include "nstu/control_messages.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using nstu::control::BootId;
using nstu::control::kBootIdBytes;
using nstu::control::kMaximumUwfDetailBytes;
using nstu::control::UwfFleetConfigureRequest;
using nstu::control::UwfFleetPhase;
using nstu::control::UwfFleetStatusReport;
using nstu::control::UwfProtectionProbe;

// Offsets inside an encoded status report.  Spelled out so a malformed-payload
// test mutates the field it means to, and so a layout change breaks the tests
// rather than silently making them test nothing.
constexpr std::size_t kPhaseOffset = sizeof(std::uint64_t) + kBootIdBytes;
constexpr std::size_t kProbeFlagsOffset = kPhaseOffset + 1;
constexpr std::size_t kDetailLengthOffset = kProbeFlagsOffset + 1;

BootId boot_id(unsigned char seed) {
    BootId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(seed + index);
    }
    return id;
}

UwfProtectionProbe protected_probe() {
    UwfProtectionProbe probe;
    probe.probe_succeeded = true;
    probe.filter_current_enabled = true;
    probe.filter_next_enabled = true;
    probe.system_volume_current_protected = true;
    probe.data_exclusion_present = true;
    probe.registry_exclusion_present = true;
    return probe;
}

UwfFleetStatusReport verified_report() {
    UwfFleetStatusReport report;
    report.operation_id = 0x0123456789abcdefull;
    report.boot_id = boot_id(0x40);
    report.phase = UwfFleetPhase::verified_protected;
    report.probe = protected_probe();
    report.detail = "UWF is protecting the NSTU volume in this session";
    return report;
}

void configure_request_round_trip() {
    for (const bool checkpoint : {false, true}) {
        for (const bool restart : {false, true}) {
            UwfFleetConfigureRequest request;
            request.operation_id = 0xfeedfacecafebeefull;
            request.checkpoint_acknowledged = checkpoint;
            request.restart_requested = restart;
            const auto payload =
                nstu::control::encode_uwf_fleet_configure_request(request);
            assert(!payload.empty());
            const auto decoded =
                nstu::control::decode_uwf_fleet_configure_request(payload);
            assert(decoded.has_value());
            assert(decoded->operation_id == request.operation_id);
            assert(decoded->checkpoint_acknowledged == checkpoint);
            assert(decoded->restart_requested == restart);
        }
    }
}

void configure_request_rejections() {
    // An unattributable request is refused by the encoder rather than sent,
    // because every later status report is matched back by operation id.
    UwfFleetConfigureRequest unattributable;
    unattributable.operation_id = 0;
    assert(nstu::control::encode_uwf_fleet_configure_request(unattributable)
               .empty());

    UwfFleetConfigureRequest request;
    request.operation_id = 9;
    request.checkpoint_acknowledged = true;
    const auto payload =
        nstu::control::encode_uwf_fleet_configure_request(request);
    assert(payload.size() == sizeof(std::uint64_t) + 1);

    auto zero_operation = payload;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        zero_operation[index] = std::byte{0};
    }
    assert(!nstu::control::decode_uwf_fleet_configure_request(zero_operation)
                .has_value());

    // Reserved flag bits are refused rather than ignored, so a future sender
    // cannot have meaning silently dropped by an older peer.
    auto reserved_flags = payload;
    reserved_flags.back() |= std::byte{0x04};
    assert(!nstu::control::decode_uwf_fleet_configure_request(reserved_flags)
                .has_value());

    auto trailing = payload;
    trailing.push_back(std::byte{0});
    assert(!nstu::control::decode_uwf_fleet_configure_request(trailing)
                .has_value());

    auto truncated = payload;
    truncated.pop_back();
    assert(!nstu::control::decode_uwf_fleet_configure_request(truncated)
                .has_value());

    assert(!nstu::control::decode_uwf_fleet_configure_request({}).has_value());
}

void status_report_round_trip() {
    const auto report = verified_report();
    const auto payload = nstu::control::encode_uwf_fleet_status_report(report);
    assert(!payload.empty());
    const auto decoded = nstu::control::decode_uwf_fleet_status_report(payload);
    assert(decoded.has_value());
    assert(decoded->operation_id == report.operation_id);
    assert(decoded->boot_id == report.boot_id);
    assert(decoded->phase == UwfFleetPhase::verified_protected);
    assert(decoded->probe.probe_succeeded);
    assert(decoded->probe.filter_current_enabled);
    assert(decoded->probe.filter_next_enabled);
    assert(decoded->probe.system_volume_current_protected);
    assert(decoded->probe.data_exclusion_present);
    assert(decoded->probe.registry_exclusion_present);
    assert(decoded->detail == report.detail);
    assert(nstu::control::probe_proves_protection(decoded->probe));

    // A rebooted client lost the operation id along with everything else it
    // held in memory.  Its first report is unsolicited current state and must
    // still encode, so the server can re-attach it by boot identity.
    UwfFleetStatusReport unsolicited = report;
    unsolicited.operation_id = 0;
    const auto unsolicited_payload =
        nstu::control::encode_uwf_fleet_status_report(unsolicited);
    assert(!unsolicited_payload.empty());
    const auto decoded_unsolicited =
        nstu::control::decode_uwf_fleet_status_report(unsolicited_payload);
    assert(decoded_unsolicited.has_value());
    assert(decoded_unsolicited->operation_id == 0);
    assert(decoded_unsolicited->boot_id == report.boot_id);

    // Every phase survives the trip, so the server never has to guess which
    // step of the operation a client is on.
    for (std::uint8_t phase = static_cast<std::uint8_t>(UwfFleetPhase::idle);
         phase <= static_cast<std::uint8_t>(UwfFleetPhase::unsupported);
         ++phase) {
        UwfFleetStatusReport in_progress;
        in_progress.operation_id = 12;
        in_progress.boot_id = boot_id(0x11);
        in_progress.phase = static_cast<UwfFleetPhase>(phase);
        in_progress.detail = "in progress";
        if (in_progress.phase == UwfFleetPhase::verified_protected) {
            in_progress.probe = protected_probe();
        }
        const auto encoded =
            nstu::control::encode_uwf_fleet_status_report(in_progress);
        assert(!encoded.empty());
        const auto round_tripped =
            nstu::control::decode_uwf_fleet_status_report(encoded);
        assert(round_tripped.has_value());
        assert(round_tripped->phase == in_progress.phase);
        assert(round_tripped->boot_id == in_progress.boot_id);
    }
}

void status_report_rejections() {
    const auto report = verified_report();
    const auto payload = nstu::control::encode_uwf_fleet_status_report(report);
    assert(payload.size() == kDetailLengthOffset + 2 + report.detail.size());

    // Phases outside the enumeration are refused on both sides.
    for (const std::byte phase :
         {std::byte{0x00}, std::byte{0x0a}, std::byte{0xff}}) {
        auto bad_phase = payload;
        bad_phase[kPhaseOffset] = phase;
        assert(!nstu::control::decode_uwf_fleet_status_report(bad_phase)
                    .has_value());
    }
    UwfFleetStatusReport out_of_range = report;
    out_of_range.phase = static_cast<UwfFleetPhase>(0);
    assert(nstu::control::encode_uwf_fleet_status_report(out_of_range).empty());

    auto reserved_probe_bits = payload;
    reserved_probe_bits[kProbeFlagsOffset] |= std::byte{0x40};
    assert(!nstu::control::decode_uwf_fleet_status_report(reserved_probe_bits)
                .has_value());

    // A probe that did not complete cannot also have made observations:
    // "unknown" must never decode into something that reads as evidence.
    auto observations_without_probe = payload;
    observations_without_probe[kProbeFlagsOffset] = std::byte{0x3e};
    assert(!nstu::control::decode_uwf_fleet_status_report(
                observations_without_probe)
                .has_value());

    // The verified phase is the exam gate.  A report claiming it while its own
    // probe does not support it is refused, not quietly downgraded.
    for (const std::byte flags :
         {std::byte{0x01}, std::byte{0x03}, std::byte{0x09}, std::byte{0x3d}}) {
        auto unsupported_claim = payload;
        unsupported_claim[kProbeFlagsOffset] = flags;
        assert(!nstu::control::decode_uwf_fleet_status_report(unsupported_claim)
                    .has_value());
    }
    // The same flags decode fine under a phase that claims nothing.
    auto verifying_claim = payload;
    verifying_claim[kPhaseOffset] =
        static_cast<std::byte>(UwfFleetPhase::verifying);
    verifying_claim[kProbeFlagsOffset] = std::byte{0x03};
    const auto verifying =
        nstu::control::decode_uwf_fleet_status_report(verifying_claim);
    assert(verifying.has_value());
    assert(!nstu::control::probe_proves_protection(verifying->probe));

    // Detail is bounded, non-empty, and printable: it is rendered in the
    // teacher UI and written to the audit log.
    UwfFleetStatusReport empty_detail = report;
    empty_detail.detail.clear();
    assert(nstu::control::encode_uwf_fleet_status_report(empty_detail).empty());
    UwfFleetStatusReport long_detail = report;
    long_detail.detail.assign(kMaximumUwfDetailBytes + 1, 'x');
    assert(nstu::control::encode_uwf_fleet_status_report(long_detail).empty());
    UwfFleetStatusReport control_detail = report;
    control_detail.detail = "bad\ndetail";
    assert(
        nstu::control::encode_uwf_fleet_status_report(control_detail).empty());

    auto embedded_control = payload;
    embedded_control.back() = std::byte{0x07};
    assert(!nstu::control::decode_uwf_fleet_status_report(embedded_control)
                .has_value());

    auto zero_detail_length = payload;
    zero_detail_length[kDetailLengthOffset] = std::byte{0};
    zero_detail_length[kDetailLengthOffset + 1] = std::byte{0};
    assert(!nstu::control::decode_uwf_fleet_status_report(zero_detail_length)
                .has_value());

    auto trailing = payload;
    trailing.push_back(std::byte{'x'});
    assert(!nstu::control::decode_uwf_fleet_status_report(trailing).has_value());

    auto truncated = payload;
    truncated.pop_back();
    assert(
        !nstu::control::decode_uwf_fleet_status_report(truncated).has_value());

    const std::vector<std::byte> short_header(kPhaseOffset, std::byte{0});
    assert(!nstu::control::decode_uwf_fleet_status_report(short_header)
                .has_value());
    assert(!nstu::control::decode_uwf_fleet_status_report({}).has_value());
}

void protection_predicate() {
    UwfProtectionProbe probe;
    assert(!nstu::control::probe_proves_protection(probe));
    probe.probe_succeeded = true;
    assert(!nstu::control::probe_proves_protection(probe));
    probe.filter_current_enabled = true;
    assert(!nstu::control::probe_proves_protection(probe));
    probe.system_volume_current_protected = true;
    assert(nstu::control::probe_proves_protection(probe));

    // Next-session state is intent, not proof.
    UwfProtectionProbe intent_only;
    intent_only.probe_succeeded = true;
    intent_only.filter_next_enabled = true;
    assert(!nstu::control::probe_proves_protection(intent_only));

    // Exclusions are a configuration warning, not a protection failure: a
    // machine can be genuinely protected with one missing.
    UwfProtectionProbe without_exclusions;
    without_exclusions.probe_succeeded = true;
    without_exclusions.filter_current_enabled = true;
    without_exclusions.system_volume_current_protected = true;
    assert(nstu::control::probe_proves_protection(without_exclusions));
}

} // namespace

int main() {
    configure_request_round_trip();
    configure_request_rejections();
    status_report_round_trip();
    status_report_rejections();
    protection_predicate();
    return 0;
}
