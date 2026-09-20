#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nstu::setup {

// Inputs kept separate from WMI so the safety gate is deterministic and can be
// tested without enabling a Windows feature on the test machine.
struct UwfEnableReadiness {
    std::uint32_t product_type = 0;
    bool feature_known = false;
    bool feature_enabled = false;
    bool provider_available = false;
    bool filter_state_known = false;
    bool current_enabled = false;
    bool next_enabled = false;
    bool data_root_valid = false;
    bool diagnostic_readiness_passed = false;
    bool checkpoint_acknowledged = false;
};

enum class UwfEnableGate : std::uint8_t {
    ready,
    already_enabled,
    unsupported_edition,
    feature_missing,
    probe_unavailable,
    provider_unavailable,
    reboot_pending,
    invalid_data_root,
    readiness_failed,
    checkpoint_required,
};

[[nodiscard]] bool is_uwf_supported_product(
    std::uint32_t product_type) noexcept;
[[nodiscard]] UwfEnableGate evaluate_uwf_enable(
    const UwfEnableReadiness& readiness) noexcept;

struct UwfConfigureRequest {
    std::filesystem::path data_root;
    // The caller must have shown the recovery-checkpoint reminder and received
    // an explicit acknowledgement. This library does not manufacture or
    // silently assume a restore point.
    bool diagnostic_readiness_passed = false;
    bool checkpoint_acknowledged = false;
};

enum class UwfConfigureOutcome : std::uint8_t {
    armed,
    already_enabled,
    unsupported_edition,
    feature_missing,
    probe_unavailable,
    provider_unavailable,
    reboot_pending,
    invalid_data_root,
    readiness_failed,
    checkpoint_required,
    access_denied,
    failed,
};

struct UwfConfigureResult {
    UwfConfigureOutcome outcome = UwfConfigureOutcome::failed;
    bool reboot_required = false;
    bool data_exclusion_added = false;
    bool registry_exclusion_added = false;
    std::string detail;
};

// Arms UWF for the next boot only after adding the whole NSTU data root and
// HKLM\Software\NSTU as exclusions. On any pre-enable failure it attempts to
// unprotect the volume again; it never schedules or forces a reboot itself.
[[nodiscard]] UwfConfigureResult configure_uwf(
    const UwfConfigureRequest& request);

} // namespace nstu::setup
