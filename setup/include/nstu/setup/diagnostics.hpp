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

enum class UwfState : std::uint8_t {
    unsupported_edition,
    feature_missing,
    provider_unavailable,
    available_unconfigured,
    enabled,
    reboot_pending,
    inconsistent,
};

enum class Readiness : std::uint8_t {
    good,
    minimum_not_met,
    recommended_not_met,
    unrated,
};

[[nodiscard]] bool is_uwf_supported_product(std::uint32_t product_type) noexcept;
[[nodiscard]] UwfState classify_uwf(const UwfProbeSnapshot& snapshot) noexcept;
[[nodiscard]] Readiness classify_memory_gib(std::uint64_t gib) noexcept;
[[nodiscard]] Readiness classify_link_speed_mbps(
    std::uint64_t mbps) noexcept;
[[nodiscard]] Readiness classify_processor(bool x64, std::uint32_t physical,
                                           std::uint32_t logical) noexcept;

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
