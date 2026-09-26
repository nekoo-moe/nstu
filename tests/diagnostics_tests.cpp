#include "nstu/setup/diagnostics.hpp"
#include "../setup/src/wmi_read.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    using nstu::setup::detail::classify_wmi_read;
    using nstu::setup::detail::WmiReadResult;
    assert(classify_wmi_read(WBEM_S_TIMEDOUT, 0, false) == WmiReadResult::error);
    assert(classify_wmi_read(E_ACCESSDENIED, 0, false) == WmiReadResult::error);
    assert(classify_wmi_read(S_OK, 0, false) == WmiReadResult::error);
    assert(classify_wmi_read(WBEM_S_FALSE, 0, false) == WmiReadResult::empty);
    assert(classify_wmi_read(S_OK, 1, true) == WmiReadResult::object);
    assert(classify_wmi_read(WBEM_S_FALSE, 1, true) == WmiReadResult::object);
    assert(classify_wmi_read(S_OK, 1, false) == WmiReadResult::error);

    {
        nstu::setup::detail::WmiVariant empty;
        assert(!nstu::setup::detail::exclusion_count(empty.value));
        empty.value.vt = VT_NULL;
        assert(nstu::setup::detail::exclusion_count(empty.value) == 0u);
    }
    {
        nstu::setup::detail::WmiVariant array;
        array.value.vt = VT_ARRAY | VT_BSTR;
        array.value.parray = SafeArrayCreateVector(VT_BSTR, 0, 3);
        assert(array.value.parray);
        assert(nstu::setup::detail::exclusion_count(array.value) == 3u);
    }
    {
        nstu::setup::detail::WmiVariant array;
        array.value.vt = VT_ARRAY | VT_UNKNOWN;
        array.value.parray = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
        assert(array.value.parray);
        assert(nstu::setup::detail::exclusion_count(array.value) == 0u);
    }
    {
        nstu::setup::detail::WmiVariant invalid;
        invalid.value.vt = VT_ARRAY | VT_BSTR;
        invalid.value.parray = nullptr;
        assert(!nstu::setup::detail::exclusion_count(invalid.value));
        invalid.value.vt = VT_EMPTY;
    }
    {
        nstu::setup::UwfExclusionSummary exclusions;
        exclusions.current_known = exclusions.next_known = true;
        nstu::setup::detail::accumulate_exclusions(exclusions, true, std::nullopt);
        nstu::setup::detail::accumulate_exclusions(exclusions, true, 0);
        nstu::setup::detail::accumulate_exclusions(exclusions, false, 0);
        assert(!exclusions.current_known && exclusions.next_known);
        assert(exclusions.current_volume_count == 2);
        nstu::setup::detail::accumulate_exclusions(exclusions, std::nullopt, 0);
        nstu::setup::detail::accumulate_exclusions(exclusions, false, 0);
        assert(!exclusions.current_known && !exclusions.next_known);
    }
    {
        nstu::setup::UwfVolumeSummary volumes;
        volumes.query_known = true;
        volumes.entries = {
            {L"C:", L"volume-a", true, true, true, true},
            {L"D:", L"volume-b", false, true, true, true}};
        assert(nstu::setup::detail::protected_volumes_match(volumes) == false);
        volumes.entries[1].volume_name = L"VOLUME-A";
        assert(nstu::setup::detail::protected_volumes_match(volumes) == true);
        volumes.entries.push_back({L"", L"", true, true, false, true});
        assert(nstu::setup::detail::protected_volumes_match(volumes) == true);
        volumes.entries[1].volume_name.clear();
        assert(!nstu::setup::detail::protected_volumes_match(volumes).has_value());
    }
    {
        nstu::setup::UwfOverlaySummary overlay;
        overlay.query_known = overlay.config_known = overlay.consumption_known = true;
        overlay.current_config_known = overlay.next_config_known = true;
        overlay.current_maximum_size_mb = overlay.next_maximum_size_mb = 1024;
        overlay.warning_threshold_mb = 768;
        overlay.critical_threshold_mb = 896;
        using nstu::setup::DiagnosticSeverity;
        using nstu::setup::detail::overlay_severity;
        assert(overlay_severity(overlay) == DiagnosticSeverity::pass);
        overlay.consumption_mb = 768;
        assert(overlay_severity(overlay) == DiagnosticSeverity::warning);
        overlay.consumption_mb = 896;
        assert(overlay_severity(overlay) == DiagnosticSeverity::failure);
        overlay.consumption_mb = 0;
        overlay.current_type = 2;
        assert(overlay_severity(overlay) == DiagnosticSeverity::failure);
        overlay.current_type = 0;
        overlay.warning_threshold_mb = overlay.critical_threshold_mb;
        assert(overlay_severity(overlay) == DiagnosticSeverity::failure);
        overlay.warning_threshold_mb = overlay.critical_threshold_mb = 0;
        assert(overlay_severity(overlay) == DiagnosticSeverity::pass);
        overlay.warning_threshold_mb = 2048;
        overlay.critical_threshold_mb = 3072;
        assert(overlay_severity(overlay) == DiagnosticSeverity::failure);
        overlay.warning_threshold_mb = overlay.critical_threshold_mb = 0;
        overlay.consumption_mb = 1024;
        assert(overlay_severity(overlay) == DiagnosticSeverity::failure);
        overlay.config_known = false;
        assert(overlay_severity(overlay) == DiagnosticSeverity::warning);
    }
    assert(nstu::setup::is_uwf_supported_product(0x79));
    assert(nstu::setup::is_uwf_supported_product(0xbc));
    assert(!nstu::setup::is_uwf_supported_product(0x30));

    nstu::setup::UwfProbeSnapshot unsupported{};
    unsupported.product_type = 0x30;
    assert(nstu::setup::classify_uwf(unsupported) ==
           nstu::setup::UwfState::unsupported_edition);

    // A supported SKU with an indeterminate optional-feature query is not the
    // same as a confirmed missing feature. The diagnostic must remain
    // read-only and ask the technician to retry the probe.
    nstu::setup::UwfProbeSnapshot probe_failed{};
    probe_failed.product_type = 0x79;
    assert(nstu::setup::classify_uwf(probe_failed) ==
           nstu::setup::UwfState::probe_unavailable);

    nstu::setup::UwfProbeSnapshot missing{};
    missing.product_type = 0x79;
    missing.feature_known = true;
    assert(nstu::setup::classify_uwf(missing) ==
           nstu::setup::UwfState::feature_missing);

    nstu::setup::UwfProbeSnapshot unavailable{};
    unavailable.product_type = 0x79;
    unavailable.feature_known = true;
    unavailable.feature_enabled = true;
    assert(nstu::setup::classify_uwf(unavailable) ==
           nstu::setup::UwfState::provider_unavailable);

    nstu::setup::UwfProbeSnapshot pending = unavailable;
    pending.provider_available = true;
    pending.filter_state_known = true;
    pending.current_enabled = false;
    pending.next_enabled = true;
    assert(nstu::setup::classify_uwf(pending) ==
           nstu::setup::UwfState::reboot_pending);

    nstu::setup::UwfProbeSnapshot enabled = pending;
    enabled.current_enabled = true;
    assert(nstu::setup::classify_uwf(enabled) ==
           nstu::setup::UwfState::enabled);

    assert(nstu::setup::classify_memory_gib(5) ==
           nstu::setup::Readiness::minimum_not_met);
    assert(nstu::setup::classify_memory_gib(6) ==
           nstu::setup::Readiness::good);
    assert(nstu::setup::classify_link_speed_mbps(99) ==
           nstu::setup::Readiness::minimum_not_met);
    assert(nstu::setup::classify_link_speed_mbps(100) ==
           nstu::setup::Readiness::recommended_not_met);
    assert(nstu::setup::classify_link_speed_mbps(1000) ==
           nstu::setup::Readiness::good);
    assert(nstu::setup::classify_processor(false, 8, 8) ==
           nstu::setup::Readiness::unrated);
    assert(nstu::setup::classify_processor(true, 4, 4) ==
           nstu::setup::Readiness::unrated);
    assert(nstu::setup::classify_processor(true, 0, 0) ==
           nstu::setup::Readiness::unrated);

    nstu::setup::ClientRuntimeSnapshot runtime;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::service_missing);
    runtime.service_present = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::service_not_running);
    runtime.service_running = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::service_not_automatic);
    runtime.service_automatic = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::service_wrong_account);
    runtime.service_local_system = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::service_wrong_session);
    runtime.service_session_zero = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::agent_binary_missing);
    runtime.agent_binary_present = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::interactive_session_unavailable);
    runtime.interactive_session = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::interactive_user_unavailable);
    runtime.interactive_user_known = true;
    runtime.interactive_user_administrator = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::interactive_user_administrator);
    runtime.interactive_user_administrator = false;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::agent_not_running);
    runtime.agent_running_in_session = true;
    assert(nstu::setup::classify_client_runtime(runtime) ==
           nstu::setup::ClientRuntimeState::ready);
    assert(nstu::setup::classify_processor(true, 2, 4) ==
           nstu::setup::Readiness::unrated);

    nstu::setup::UwfEventHealthSummary event_unavailable{};
    assert(nstu::setup::classify_uwf_event_health(event_unavailable) ==
           nstu::setup::UwfEventHealth::unavailable);

    nstu::setup::UwfEventHealthSummary event_healthy{};
    event_healthy.query_known = true;
    event_healthy.system_channel_known = true;
    event_healthy.admin_channel_known = true;
    event_healthy.operational_channel_known = true;
    assert(nstu::setup::classify_uwf_event_health(event_healthy) ==
           nstu::setup::UwfEventHealth::healthy);

    auto event_warning = event_healthy;
    event_warning.warning_events = 1;
    assert(nstu::setup::classify_uwf_event_health(event_warning) ==
           nstu::setup::UwfEventHealth::warning);

    auto event_critical = event_healthy;
    event_critical.critical_events = 1;
    assert(nstu::setup::classify_uwf_event_health(event_critical) ==
           nstu::setup::UwfEventHealth::critical);
    event_critical = event_healthy;
    event_critical.admin_error_events = 1;
    assert(nstu::setup::classify_uwf_event_health(event_critical) ==
           nstu::setup::UwfEventHealth::critical);

    auto event_truncated = event_healthy;
    event_truncated.truncated = true;
    assert(nstu::setup::classify_uwf_event_health(event_truncated) ==
           nstu::setup::UwfEventHealth::warning);

    auto event_partial = event_healthy;
    event_partial.operational_channel_known = false;
    assert(nstu::setup::classify_uwf_event_health(event_partial) ==
           nstu::setup::UwfEventHealth::warning);

    assert(nstu::setup::diagnostic_close_delay_ms(false, true, false, false) ==
           nstu::setup::kDiagnosticCleanCloseDelayMs);
    assert(nstu::setup::diagnostic_close_delay_ms(false, true, false, true) ==
           0);
    assert(nstu::setup::diagnostic_close_delay_ms(true, true, false, true) ==
           nstu::setup::kDiagnosticInstallerIssueCloseDelayMs);
    assert(nstu::setup::diagnostic_close_delay_ms(true, true, true, true) ==
           0);
    assert(nstu::setup::diagnostic_close_delay_ms(true, false, false, false) ==
           0);

    const nstu::setup::DiagnosticOptions options{};
    const auto checks = nstu::setup::diagnostic_checks(options);
    assert(!checks.empty());
    // The MSVC diagnostics library must decode UTF-8 source independently of
    // the developer machine's active ANSI code page.
    assert(checks.front().title_vi == L"H\u1ec7 \u0111i\u1ec1u h\u00e0nh");
    constexpr std::array<const char*, 17> expected_ids = {
        "os",          "uwf",          "uwf_volumes", "uwf_exclusions",
        "uwf_overlay", "uwf_events",   "safe_mode",   "installation",
        "registry",    "hardware",     "network",     "graphics",
        "encoder",     "time",         "internet",    "server",
        "service"};
    assert(checks.size() == expected_ids.size());
    for (std::size_t index = 0; index < expected_ids.size(); ++index) {
        assert(checks[index].id == expected_ids[index]);
    }
    return 0;
}
