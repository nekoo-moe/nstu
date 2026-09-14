#pragma once

#include "nstu/setup/diagnostics.hpp"

#include <windows.h>
#include <wbemidl.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace nstu::setup::detail {

enum class WmiReadResult { error, empty, object };

// Next requests one object. A timeout is not end-of-enumeration, even though
// WBEM_S_TIMEDOUT is a success HRESULT. Never approve a partial inventory.
[[nodiscard]] inline WmiReadResult classify_wmi_read(
    HRESULT status, ULONG returned, bool has_object) noexcept {
    if (status != WBEM_S_NO_ERROR && status != WBEM_S_FALSE) {
        return WmiReadResult::error;
    }
    if (returned == 1 && has_object) return WmiReadResult::object;
    if (status == WBEM_S_FALSE && returned == 0 && !has_object) {
        return WmiReadResult::empty;
    }
    return WmiReadResult::error;
}

class WmiVariant {
public:
    WmiVariant() noexcept { VariantInit(&value); }
    ~WmiVariant() { VariantClear(&value); }
    WmiVariant(const WmiVariant&) = delete;
    WmiVariant& operator=(const WmiVariant&) = delete;
    VARIANT value;
};

[[nodiscard]] inline std::optional<std::uint32_t> exclusion_count(
    const VARIANT& value) noexcept {
    // UWF_Volume.GetExclusions explicitly returns NULL for an empty list.
    if (value.vt == VT_NULL) return 0;
    if (value.vt != (VT_ARRAY | VT_UNKNOWN) &&
        value.vt != (VT_ARRAY | VT_BSTR)) return std::nullopt;
    if (value.parray == nullptr || SafeArrayGetDim(value.parray) != 1) {
        return std::nullopt;
    }
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(value.parray, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(value.parray, 1, &upper))) {
        return std::nullopt;
    }
    const auto length = static_cast<std::int64_t>(upper) - lower + 1;
    if (length < 0 || static_cast<std::uint64_t>(length) >
                          std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(length);
}

// Start known flags at true after successful enumeration, then accumulate.
// Later successful records must never hide an earlier unreadable volume.
inline void accumulate_exclusions(UwfExclusionSummary& summary,
                                 std::optional<bool> current,
                                 std::optional<std::uint32_t> count) noexcept {
    if (!current.has_value()) {
        summary.current_known = summary.next_known = false;
        return;
    }
    auto& known = *current ? summary.current_known : summary.next_known;
    auto& volumes = *current ? summary.current_volume_count
                             : summary.next_volume_count;
    auto& total = *current ? summary.current_exclusion_count
                           : summary.next_exclusion_count;
    ++volumes;
    if (!count || total > std::numeric_limits<std::uint32_t>::max() - *count) {
        known = false;
        return;
    }
    total += *count;
}

[[nodiscard]] inline std::optional<bool> protected_volumes_match(
    const UwfVolumeSummary& summary) {
    if (!summary.query_known || !summary.records_complete) return std::nullopt;
    std::vector<std::wstring_view> current;
    std::vector<std::wstring_view> next;
    const auto equal = [](std::wstring_view a, std::wstring_view b) {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                    b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
    };
    for (const auto& entry : summary.entries) {
        if (!entry.session_known || !entry.protected_known) {
            return std::nullopt;
        }
        if (!entry.protected_state) continue;
        // Only protected records participate in the identity comparison. An
        // unprotected or unmounted record may legitimately omit VolumeName.
        if (entry.volume_name.empty() || entry.volume_name.size() > 32767) {
            return std::nullopt;
        }
        auto& entries = entry.current_session ? current : next;
        for (const auto existing : entries) {
            if (equal(existing, entry.volume_name)) return std::nullopt;
        }
        entries.push_back(entry.volume_name);
    }
    if (current.size() != next.size()) return false;
    for (const auto name : current) {
        bool found = false;
        for (const auto other : next) found = found || equal(name, other);
        if (!found) return false;
    }
    return true;
}

[[nodiscard]] inline DiagnosticSeverity overlay_severity(
    const UwfOverlaySummary& summary) noexcept {
    if (!summary.query_known || !summary.config_known ||
        !summary.current_config_known || !summary.next_config_known ||
        !summary.consumption_known) return DiagnosticSeverity::warning;
    if (summary.current_type > 1 || summary.next_type > 1 ||
        summary.current_maximum_size_mb <= 0 ||
        summary.next_maximum_size_mb <= 0 ||
        (summary.warning_threshold_mb != 0 &&
         summary.critical_threshold_mb != 0 &&
         summary.warning_threshold_mb >= summary.critical_threshold_mb) ||
        (summary.warning_threshold_mb != 0 &&
         (summary.warning_threshold_mb >
              static_cast<std::uint64_t>(summary.current_maximum_size_mb) ||
          summary.warning_threshold_mb >
              static_cast<std::uint64_t>(summary.next_maximum_size_mb))) ||
        (summary.critical_threshold_mb != 0 &&
         (summary.critical_threshold_mb >
              static_cast<std::uint64_t>(summary.current_maximum_size_mb) ||
          summary.critical_threshold_mb >
              static_cast<std::uint64_t>(summary.next_maximum_size_mb))) ||
        summary.consumption_mb >= summary.current_maximum_size_mb ||
        (summary.critical_threshold_mb != 0 &&
         summary.consumption_mb >= summary.critical_threshold_mb)) {
        return DiagnosticSeverity::failure;
    }
    if (summary.current_type != summary.next_type ||
        summary.current_maximum_size_mb != summary.next_maximum_size_mb ||
        (summary.warning_threshold_mb != 0 &&
         summary.consumption_mb >= summary.warning_threshold_mb)) {
        return DiagnosticSeverity::warning;
    }
    return DiagnosticSeverity::pass;
}

} // namespace nstu::setup::detail
