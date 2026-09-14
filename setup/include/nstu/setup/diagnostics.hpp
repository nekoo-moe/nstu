#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace nstu::setup {

enum class DiagnosticRole : std::uint8_t { client, server };
enum class DiagnosticSeverity : std::uint8_t {
    pass,
    warning,
    failure,
    not_applicable,
};

struct DiagnosticOptions {
    DiagnosticRole role = DiagnosticRole::client;
    std::wstring server_address;
    std::uint16_t server_port = 47001;
    bool installer = false;
    bool boot_check = false;
};

struct DiagnosticResult {
    std::string id;
    DiagnosticSeverity severity = DiagnosticSeverity::pass;
    std::wstring title_en;
    std::wstring title_vi;
    std::wstring detail_en;
    std::wstring detail_vi;
    std::wstring remediation_en;
    std::wstring remediation_vi;
    std::uint32_t error_code = 0;
};

struct DiagnosticCheck {
    std::string id;
    std::wstring title_en;
    std::wstring title_vi;
};

struct UwfProbeSnapshot {
    std::uint32_t product_type = 0;
    bool feature_known = false;
    bool feature_enabled = false;
    bool provider_available = false;
    bool filter_state_known = false;
    bool current_enabled = false;
    bool next_enabled = false;
};

// Read-only UWF volume state.  UWF exposes one record for the current
// session and one for the next session; retaining the session bit lets the
// diagnostics distinguish an applied policy from a reboot-pending policy.
struct UwfVolumeEntry {
    std::wstring drive_letter;
    std::wstring volume_name;
    bool current_session = false;
    bool session_known = false;
    bool protected_state = false;
    bool protected_known = false;
};

struct UwfVolumeSummary {
    bool query_known = false;
    bool records_complete = true;
    std::vector<UwfVolumeEntry> entries;
};

// Exclusion paths are deliberately reduced to counts.  Diagnostics reports
// must not upload or persist the names of files that a school has chosen to
// make persistent.
struct UwfExclusionSummary {
    bool query_known = false;
    bool current_known = false;
    bool next_known = false;
    std::uint32_t current_volume_count = 0;
    std::uint32_t next_volume_count = 0;
    std::uint32_t current_exclusion_count = 0;
    std::uint32_t next_exclusion_count = 0;
};

struct UwfOverlaySummary {
    bool query_known = false;
    bool config_known = false;
    bool consumption_known = false;
    bool current_config_known = false;
    bool next_config_known = false;
    std::uint32_t current_type = 0;
    std::uint32_t next_type = 0;
    std::int64_t current_maximum_size_mb = -1;
    std::int64_t next_maximum_size_mb = -1;
    std::uint32_t consumption_mb = 0;
    std::uint32_t warning_threshold_mb = 0;
    std::uint32_t critical_threshold_mb = 0;
};

struct UwfEventHealthSummary {
    bool query_known = false;
    bool system_channel_known = false;
    bool admin_channel_known = false;
    bool operational_channel_known = false;
    bool truncated = false;
    std::uint32_t warning_events = 0;
    std::uint32_t critical_events = 0;
    std::uint32_t informational_events = 0;
    std::uint32_t operational_events = 0;
    std::uint32_t admin_warning_events = 0;
    std::uint32_t admin_error_events = 0;
};

enum class UwfEventHealth : std::uint8_t {
    unavailable,
    healthy,
    warning,
    critical,
};

enum class UwfState : std::uint8_t {
    unsupported_edition,
    feature_missing,
    provider_unavailable,
    available_unconfigured,
    enabled,
    reboot_pending,
    inconsistent,
    // The host edition is eligible, but the read-only optional-feature probe
    // could not establish whether UWF is installed. Do not treat this as a
    // request to install or mutate the Windows feature.
    probe_unavailable,
};

enum class Readiness : std::uint8_t {
    good,
    minimum_not_met,
    recommended_not_met,
    unrated,
};

struct ClientRuntimeSnapshot {
    bool service_present = false;
    bool service_running = false;
    bool service_automatic = false;
    bool service_local_system = false;
    bool service_session_zero = false;
    bool agent_binary_present = false;
    bool interactive_session = false;
    bool agent_running_in_session = false;
};

enum class ClientRuntimeState : std::uint8_t {
    ready,
    service_missing,
    service_not_running,
    service_not_automatic,
    service_wrong_account,
    service_wrong_session,
    agent_binary_missing,
    interactive_session_unavailable,
    agent_not_running,
};

// The diagnostics helper is also invoked synchronously by the NSIS
// preflight. Keep a clean automatic check short, but give an installer issue
// run a bounded amount of time to show its remediation before returning the
// result to the installer. A technician-run issue stays open for explicit
// review. A zero return value means that no close timer is wanted.
inline constexpr std::uint32_t kDiagnosticCleanCloseDelayMs = 1400;
inline constexpr std::uint32_t kDiagnosticInstallerIssueCloseDelayMs = 6000;

[[nodiscard]] constexpr std::uint32_t diagnostic_close_delay_ms(
    bool installer, bool auto_close, bool stay_open, bool has_issue) noexcept {
    if (!auto_close || stay_open) {
        return 0;
    }
    if (!has_issue) {
        return kDiagnosticCleanCloseDelayMs;
    }
    return installer ? kDiagnosticInstallerIssueCloseDelayMs : 0;
}

[[nodiscard]] bool is_uwf_supported_product(std::uint32_t product_type) noexcept;
[[nodiscard]] UwfState classify_uwf(const UwfProbeSnapshot& snapshot) noexcept;
[[nodiscard]] UwfEventHealth classify_uwf_event_health(
    const UwfEventHealthSummary& summary) noexcept;
[[nodiscard]] Readiness classify_memory_gib(std::uint64_t gib) noexcept;
[[nodiscard]] Readiness classify_link_speed_mbps(
    std::uint64_t mbps) noexcept;
[[nodiscard]] Readiness classify_processor(bool x64, std::uint32_t physical,
                                           std::uint32_t logical) noexcept;
[[nodiscard]] ClientRuntimeState classify_client_runtime(
    const ClientRuntimeSnapshot& snapshot) noexcept;

[[nodiscard]] std::vector<DiagnosticCheck> diagnostic_checks(
    const DiagnosticOptions& options);

using DiagnosticStartSink = std::function<void(const DiagnosticCheck&)>;
using DiagnosticResultSink = std::function<void(DiagnosticResult)>;

void run_startup_diagnostics(const DiagnosticOptions& options,
                             const DiagnosticStartSink& on_start,
                             const DiagnosticResultSink& on_result);

[[nodiscard]] bool write_diagnostic_report_json(
    const std::filesystem::path& path, const DiagnosticOptions& options,
    const std::vector<DiagnosticResult>& results, std::string* error = nullptr);

} // namespace nstu::setup
