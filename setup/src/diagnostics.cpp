#include "nstu/setup/diagnostics.hpp"

#include "nstu/deployment.hpp"
#include "nstu/protocol.hpp"
#include "nstu/setup/driver_scan.hpp"
#include "nstu/setup/hardware_scan.hpp"
#include "wmi_read.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ipifcons.h>
#include <netlistmgr.h>
#include <tlhelp32.h>
#include <versionhelpers.h>
#include <wbemidl.h>
#include <winsvc.h>
#include <wrl/client.h>
#include <winevt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nstu::setup {
namespace {

using Microsoft::WRL::ComPtr;

#if NSTU_INTERNAL_TEST_BUILD
constexpr bool kInternalTestBuild = true;
#else
constexpr bool kInternalTestBuild = false;
#endif

constexpr std::uint32_t kProductEnterprise = 0x00000004;
constexpr std::uint32_t kProductEducation = 0x00000079;
constexpr std::uint32_t kProductEducationN = 0x0000007a;
constexpr std::uint32_t kProductEnterpriseS = 0x0000007d;
constexpr std::uint32_t kProductEnterpriseSN = 0x0000007e;
constexpr std::uint32_t kProductEnterpriseN = 0x0000001b;
constexpr std::uint32_t kProductIotEnterprise = 0x000000bc;
constexpr std::uint32_t kProductIotEnterpriseS = 0x000000bf;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), result.data(), length);
    return result;
}

std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

DiagnosticResult result(std::string id, DiagnosticSeverity severity,
                        const wchar_t* title_en, const wchar_t* title_vi,
                        std::wstring detail_en, std::wstring detail_vi,
                        std::wstring remediation_en = {},
                        std::wstring remediation_vi = {},
                        std::uint32_t error_code = 0) {
    return {std::move(id), severity, title_en, title_vi,
            std::move(detail_en), std::move(detail_vi),
            std::move(remediation_en), std::move(remediation_vi), error_code};
}

bool read_reg_string(HKEY root, const wchar_t* path, const wchar_t* name,
                     std::wstring& value) {
    std::array<wchar_t, 1024> buffer{};
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(root, path, name, RRF_RT_REG_SZ,
                                        nullptr, buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    value.assign(buffer.data());
    return true;
}

std::wstring os_product_name() {
    std::wstring value;
    if (read_reg_string(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        L"ProductName", value)) {
        return value;
    }
    return L"Unknown Windows edition";
}

std::wstring os_display_version() {
    std::wstring value;
    if (read_reg_string(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        L"DisplayVersion", value)) {
        return value;
    }
    read_reg_string(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"ReleaseId", value);
    return value;
}

std::uint32_t os_product_type() {
    DWORD product = 0;
    if (GetProductInfo(10, 0, 0, 0, &product)) {
        return product;
    }
    return 0;
}

std::wstring os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn function = nullptr;
    if (module != nullptr) {
        const FARPROC raw = GetProcAddress(module, "RtlGetVersion");
        static_assert(sizeof(function) == sizeof(raw));
        std::memcpy(&function, &raw, sizeof(function));
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function == nullptr || function(&version) != 0) {
        return L"unknown build";
    }
    std::wostringstream text;
    text << version.dwMajorVersion << L'.' << version.dwMinorVersion << L'.'
         << version.dwBuildNumber;
    return text.str();
}

class Bstr {
public:
    explicit Bstr(const wchar_t* value) : value_(SysAllocString(value)) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }

private:
    BSTR value_ = nullptr;
};

using WmiFirstResult = detail::WmiReadResult;

class WmiSession {
public:
    bool connect(const wchar_t* namespace_name) {
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&locator)))) {
            return false;
        }
        Bstr ns(namespace_name);
        if (ns.get() == nullptr ||
            FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr,
                                           WBEM_FLAG_CONNECT_USE_MAX_WAIT,
                                           nullptr, nullptr, &services_))) {
            return false;
        }
        return SUCCEEDED(CoSetProxyBlanket(
            services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
            EOAC_NONE));
    }

    WmiFirstResult first_result(
        const wchar_t* query, ComPtr<IWbemClassObject>& object) const {
        object.Reset();
        if (!services_) {
            return WmiFirstResult::error;
        }
        Bstr language(L"WQL");
        Bstr text(query);
        ComPtr<IEnumWbemClassObject> enumerator;
        if (language.get() == nullptr || text.get() == nullptr ||
            FAILED(services_->ExecQuery(language.get(), text.get(),
                                         WBEM_FLAG_FORWARD_ONLY |
                                             WBEM_FLAG_RETURN_IMMEDIATELY,
                                         nullptr, &enumerator))) {
            return WmiFirstResult::error;
        }
        ULONG returned = 0;
        const HRESULT status =
            enumerator->Next(2000, 1, object.GetAddressOf(), &returned);
        const auto read = detail::classify_wmi_read(status, returned,
                                                    object != nullptr);
        if (read != WmiFirstResult::object) object.Reset();
        return read;
    }

    bool first(const wchar_t* query, ComPtr<IWbemClassObject>& object) const {
        return first_result(query, object) == WmiFirstResult::object;
    }

    bool all(const wchar_t* query,
             std::vector<ComPtr<IWbemClassObject>>& objects) const {
        objects.clear();
        if (!services_) {
            return false;
        }
        Bstr language(L"WQL");
        Bstr text(query);
        ComPtr<IEnumWbemClassObject> enumerator;
        if (language.get() == nullptr || text.get() == nullptr ||
            FAILED(services_->ExecQuery(language.get(), text.get(),
                                         WBEM_FLAG_FORWARD_ONLY |
                                             WBEM_FLAG_RETURN_IMMEDIATELY,
                                         nullptr, &enumerator))) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (objects.size() < 256 &&
               std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline -
                                           std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                objects.clear();
                return false;
            }
            ComPtr<IWbemClassObject> object;
            ULONG returned = 0;
            const HRESULT status = enumerator->Next(
                static_cast<ULONG>(std::min<std::int64_t>(remaining, 2000)),
                1, object.GetAddressOf(), &returned);
            const auto read = detail::classify_wmi_read(status, returned,
                                                       object != nullptr);
            if (read == WmiFirstResult::error) {
                objects.clear();
                return false;
            }
            if (read == WmiFirstResult::empty) return true;
            objects.push_back(std::move(object));
            if (status == WBEM_S_FALSE) return true;
        }
        objects.clear();
        return false;
    }

    [[nodiscard]] IWbemServices* services() const noexcept {
        return services_.Get();
    }

private:
    ComPtr<IWbemServices> services_;
};

bool variant_uint32(IWbemClassObject* object, const wchar_t* name,
                    std::uint32_t& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT result = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(result)) {
        if (variant.vt == VT_UI4) {
            value = variant.ulVal;
            ok = true;
        } else if (variant.vt == VT_I4 && variant.lVal >= 0) {
            value = static_cast<std::uint32_t>(variant.lVal);
            ok = true;
        }
    }
    VariantClear(&variant);
    return ok;
}

bool variant_bool(IWbemClassObject* object, const wchar_t* name, bool& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT result = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(result) && variant.vt == VT_BOOL) {
        value = variant.boolVal != VARIANT_FALSE;
        ok = true;
    }
    VariantClear(&variant);
    return ok;
}

bool variant_string(IWbemClassObject* object, const wchar_t* name,
                    std::wstring& value) {
    detail::WmiVariant owned;
    auto& variant = owned.value;
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(status) && variant.vt == VT_BSTR && variant.bstrVal != nullptr) {
        value.assign(variant.bstrVal, SysStringLen(variant.bstrVal));
        ok = true;
    }
    return ok;
}

bool variant_int64(IWbemClassObject* object, const wchar_t* name,
                   std::int64_t& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(status)) {
        switch (variant.vt) {
        case VT_I4:
            value = variant.lVal;
            ok = true;
            break;
        case VT_UI4:
            value = variant.ulVal;
            ok = true;
            break;
        case VT_I8:
            value = variant.llVal;
            ok = true;
            break;
        case VT_UI8:
            if (variant.ullVal <=
                static_cast<ULONGLONG>(std::numeric_limits<std::int64_t>::max())) {
                value = static_cast<std::int64_t>(variant.ullVal);
                ok = true;
            }
            break;
        default:
            break;
        }
    }
    VariantClear(&variant);
    return ok;
}

bool object_path(IWbemClassObject* object, std::wstring& path) {
    return variant_string(object, L"__PATH", path) && !path.empty();
}

bool query_optional_feature(bool& known, bool& enabled) {
    known = false;
    enabled = false;
    WmiSession session;
    if (!session.connect(L"ROOT\\cimv2")) {
        return false;
    }
    ComPtr<IWbemClassObject> object;
    const auto query = session.first_result(
        L"SELECT InstallState FROM Win32_OptionalFeature WHERE Name='Client-UnifiedWriteFilter'",
        object);
    if (query == WmiFirstResult::empty) {
        known = true;
        enabled = false;
        return true;
    }
    if (query != WmiFirstResult::object) {
        return false;
    }
    std::uint32_t state = 0;
    if (!variant_uint32(object.Get(), L"InstallState", state) ||
        state < 1 || state > 3) {
        return false;
    }
    known = true;
    enabled = state == 1;
    return true;
}

bool query_uwf_filter(UwfProbeSnapshot& snapshot) {
    WmiSession session;
    if (!session.connect(L"ROOT\\standardcimv2\\embedded")) {
        return false;
    }
    snapshot.provider_available = true;
    ComPtr<IWbemClassObject> object;
    if (!session.first(L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter",
                      object)) {
        return false;
    }
    bool current = false;
    bool next = false;
    if (!variant_bool(object.Get(), L"CurrentEnabled", current) ||
        !variant_bool(object.Get(), L"NextEnabled", next)) {
        return false;
    }
    snapshot.filter_state_known = true;
    snapshot.current_enabled = current;
    snapshot.next_enabled = next;
    return true;
}

bool call_get_exclusions(WmiSession& session, IWbemClassObject* volume,
                         std::uint32_t& count, DWORD timeout_ms) {
    count = 0;
    std::wstring path;
    if (!object_path(volume, path) || session.services() == nullptr) {
        return false;
    }
    Bstr object_path_bstr(path.c_str());
    Bstr method(L"GetExclusions");
    ComPtr<IWbemClassObject> output;
    ComPtr<IWbemCallResult> pending;
    if (object_path_bstr.get() == nullptr || method.get() == nullptr ||
        FAILED(session.services()->ExecMethod(
            object_path_bstr.get(), method.get(), WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, nullptr, output.GetAddressOf(), &pending)) ||
        (!output && !pending)) {
        return false;
    }
    // ExecMethod may complete synchronously and return output parameters
    // without an IWbemCallResult.  If it is genuinely pending, wait once
    // within the same bounded timeout used by the other WMI calls.
    if (!output &&
        pending->GetResultObject(timeout_ms, output.GetAddressOf()) !=
            WBEM_S_NO_ERROR) {
        return false;
    }
    if (!output) return false;
    std::uint32_t returned_status = 0;
    if (!variant_uint32(output.Get(), L"ReturnValue", returned_status) ||
        returned_status != 0) return false;
    detail::WmiVariant exclusions;
    Bstr property(L"ExcludedFiles");
    const HRESULT status = output->Get(property.get(), 0, &exclusions.value,
                                       nullptr, nullptr);
    if (FAILED(status)) return false;
    const auto parsed = detail::exclusion_count(exclusions.value);
    if (!parsed) return false;
    count = *parsed;
    return true;
}

bool query_uwf_volumes(UwfVolumeSummary& summary) {
    summary = {};
    // Value-initialization clears the public default member initializer. Keep
    // the accumulator optimistic and make it sticky only when a record cannot
    // be decoded below.
    summary.records_complete = true;
    WmiSession session;
    if (!session.connect(L"ROOT\\standardcimv2\\embedded")) {
        return false;
    }
    std::vector<ComPtr<IWbemClassObject>> objects;
    if (!session.all(
            L"SELECT CurrentSession, DriveLetter, VolumeName, Protected FROM UWF_Volume",
            objects)) {
        return false;
    }
    summary.query_known = true;
    summary.entries.reserve(objects.size());
    for (const auto& object : objects) {
        if (!object) {
            summary.records_complete = false;
            continue;
        }
        UwfVolumeEntry entry;
        entry.session_known =
            variant_bool(object.Get(), L"CurrentSession", entry.current_session);
        entry.protected_known =
            variant_bool(object.Get(), L"Protected", entry.protected_state);
        summary.records_complete =
            summary.records_complete && entry.session_known &&
            entry.protected_known;
        // DriveLetter is NULL for a volume without a mounted letter.  A
        // missing string is therefore not itself a query failure.
        variant_string(object.Get(), L"DriveLetter", entry.drive_letter);
        variant_string(object.Get(), L"VolumeName", entry.volume_name);
        summary.entries.push_back(std::move(entry));
    }
    return true;
}

bool query_uwf_exclusions(UwfExclusionSummary& summary) {
    summary = {};
    WmiSession session;
    if (!session.connect(L"ROOT\\standardcimv2\\embedded")) {
        return false;
    }
    std::vector<ComPtr<IWbemClassObject>> objects;
    if (!session.all(
            L"SELECT * FROM UWF_Volume",
            objects)) {
        return false;
    }
    summary.query_known = true;
    summary.current_known = summary.next_known = true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    for (const auto& object : objects) {
        if (!object) {
            detail::accumulate_exclusions(summary, std::nullopt, std::nullopt);
            continue;
        }
        const auto remaining_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                       std::chrono::steady_clock::now())
                                        .count();
        if (remaining_ms <= 0) {
            summary.current_known = summary.next_known = false;
            break;
        }
        bool current = false;
        if (!variant_bool(object.Get(), L"CurrentSession", current)) {
            // Keep the successful query visible, but mark the affected
            // session as indeterminate rather than guessing its state.
            detail::accumulate_exclusions(summary, std::nullopt, std::nullopt);
            continue;
        }
        std::uint32_t count = 0;
        if (!call_get_exclusions(
                session, object.Get(), count,
                static_cast<DWORD>(std::min<std::int64_t>(remaining_ms, 2000)))) {
            detail::accumulate_exclusions(summary, current, std::nullopt);
            continue;
        }
        detail::accumulate_exclusions(summary, current, count);
    }
    summary.current_known = summary.current_known && summary.current_volume_count != 0;
    summary.next_known = summary.next_known && summary.next_volume_count != 0;
    return true;
}

bool query_uwf_overlay(UwfOverlaySummary& summary) {
    summary = {};
    WmiSession session;
    if (!session.connect(L"ROOT\\standardcimv2\\embedded")) {
        return false;
    }

    std::vector<ComPtr<IWbemClassObject>> config_objects;
    const bool config_query = session.all(
        L"SELECT CurrentSession, Type, MaximumSize FROM UWF_OverlayConfig",
        config_objects);
    summary.config_known = config_query;
    if (config_query) {
        for (const auto& object : config_objects) {
            if (!object) {
                summary.config_known = false;
                break;
            }
            bool current = false;
            std::uint32_t type = 0;
            std::int64_t maximum = -1;
            if (!variant_bool(object.Get(), L"CurrentSession", current) ||
                !variant_uint32(object.Get(), L"Type", type) ||
                !variant_int64(object.Get(), L"MaximumSize", maximum)) {
                summary.config_known = false;
                break;
            }
            if (current ? summary.current_config_known : summary.next_config_known) {
                summary.config_known = false;
                break;
            }
            if (current) {
                summary.current_config_known = true;
                summary.current_type = type;
                summary.current_maximum_size_mb = maximum;
            } else {
                summary.next_config_known = true;
                summary.next_type = type;
                summary.next_maximum_size_mb = maximum;
            }
        }
    }

    ComPtr<IWbemClassObject> overlay;
    const bool overlay_query =
        session.first(L"SELECT OverlayConsumption, WarningOverlayThreshold, "
                      L"CriticalOverlayThreshold FROM UWF_Overlay",
                      overlay);
    if (overlay_query && overlay) {
        std::uint32_t consumption = 0;
        std::uint32_t warning = 0;
        std::uint32_t critical = 0;
        if (variant_uint32(overlay.Get(), L"OverlayConsumption", consumption) &&
            variant_uint32(overlay.Get(), L"WarningOverlayThreshold", warning) &&
            variant_uint32(overlay.Get(), L"CriticalOverlayThreshold", critical)) {
            summary.consumption_known = true;
            summary.consumption_mb = consumption;
            summary.warning_threshold_mb = warning;
            summary.critical_threshold_mb = critical;
        }
    }
    summary.query_known = summary.config_known || summary.consumption_known;
    return summary.query_known;
}

class EvtHandle {
public:
    EvtHandle() = default;
    explicit EvtHandle(EVT_HANDLE handle) : handle_(handle) {}
    ~EvtHandle() {
        if (handle_ != nullptr) {
            EvtClose(handle_);
        }
    }
    EvtHandle(const EvtHandle&) = delete;
    EvtHandle& operator=(const EvtHandle&) = delete;
    EvtHandle(EvtHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    EvtHandle& operator=(EvtHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                EvtClose(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] EVT_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    EVT_HANDLE handle_ = nullptr;
};

bool query_event_count(const wchar_t* channel, const wchar_t* query,
                       std::uint32_t& count, bool& truncated) {
    constexpr std::uint32_t kMaximumEvents = 256;
    constexpr DWORD kQueryTimeoutMs = 2000;
    count = 0;
    EvtHandle result_set(EvtQuery(nullptr, channel, query,
                                  EvtQueryChannelPath |
                                      EvtQueryReverseDirection));
    if (!result_set) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (count < kMaximumEvents) {
        const auto remaining_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                       std::chrono::steady_clock::now())
                                       .count();
        if (remaining_ms <= 0) return false;
        std::array<EVT_HANDLE, 16> events{};
        DWORD returned = 0;
        if (!EvtNext(result_set.get(), static_cast<DWORD>(events.size()),
                     events.data(), static_cast<DWORD>(std::min<std::int64_t>(
                         remaining_ms, kQueryTimeoutMs)),
                     0, &returned)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                return true;
            }
            return false;
        }
        for (DWORD index = 0; index < returned; ++index) {
            if (events[index] != nullptr) {
                EvtClose(events[index]);
            }
        }
        const auto capacity_remaining = kMaximumEvents - count;
        count += std::min<std::uint32_t>(capacity_remaining, returned);
        if (returned == 0) {
            return true;
        }
    }
    truncated = true;
    return true;
}

bool query_uwf_event_health(UwfEventHealthSummary& summary) {
    summary = {};
    constexpr wchar_t kRecentSystemWarning[] =
        L"*[System[Provider[@Name='uwfvol'] and EventID=1 and "
        L"TimeCreated[timediff(@SystemTime) <= 604800000]]]";
    constexpr wchar_t kRecentSystemCritical[] =
        L"*[System[Provider[@Name='uwfvol'] and EventID=2 and "
        L"TimeCreated[timediff(@SystemTime) <= 604800000]]]";
    constexpr wchar_t kRecentSystemInformational[] =
        L"*[System[Provider[@Name='uwfvol'] and EventID=3 and "
        L"TimeCreated[timediff(@SystemTime) <= 604800000]]]";
    constexpr wchar_t kRecentAdminWarning[] =
        L"*[System[(Level=3) and "
        L"TimeCreated[timediff(@SystemTime) <= 604800000]]]";
    constexpr wchar_t kRecentAdminError[] =
        L"*[System[(Level=1 or Level=2) and "
        L"TimeCreated[timediff(@SystemTime) <= 604800000]]]";
    constexpr wchar_t kRecentOperational[] =
        L"*[System[TimeCreated[timediff(@SystemTime) <= 604800000]]]";

    bool system_ok =
        query_event_count(L"System", kRecentSystemWarning,
                          summary.warning_events, summary.truncated);
    bool query_ok =
        query_event_count(L"System", kRecentSystemCritical,
                          summary.critical_events, summary.truncated);
    system_ok = system_ok && query_ok;
    query_ok = query_event_count(L"System", kRecentSystemInformational,
                                 summary.informational_events,
                                 summary.truncated);
    system_ok = system_ok && query_ok;
    summary.system_channel_known = system_ok;

    bool admin_ok = query_event_count(
        L"Microsoft-Windows-UnifiedWriteFilter/Admin", kRecentAdminWarning,
        summary.admin_warning_events, summary.truncated);
    query_ok = query_event_count(
        L"Microsoft-Windows-UnifiedWriteFilter/Admin", kRecentAdminError,
        summary.admin_error_events, summary.truncated);
    admin_ok = admin_ok && query_ok;
    summary.admin_channel_known = admin_ok;

    summary.operational_channel_known = query_event_count(
        L"Microsoft-Windows-UnifiedWriteFilter/Operational", kRecentOperational,
        summary.operational_events,
        summary.truncated);
    summary.query_known = summary.system_channel_known ||
                          summary.admin_channel_known ||
                          summary.operational_channel_known;
    return summary.query_known;
}

DiagnosticResult check_os() {
    const bool supported = IsWindows10OrGreater() && sizeof(void*) == 8;
    const auto detail = os_product_name() + L" (build " + os_version() +
                         L", " + os_display_version() + L")";
    if (!supported) {
        return result("os", DiagnosticSeverity::failure, L"Operating system",
                      L"Hệ điều hành", detail, detail,
                      L"NSTU requires 64-bit Windows 10 or Windows 11.",
                      L"NSTU yêu cầu Windows 10 hoặc Windows 11 64-bit.", 1);
    }
    return result("os", DiagnosticSeverity::pass, L"Operating system",
                  L"Hệ điều hành", detail, detail);
}

DiagnosticResult check_uwf(DiagnosticRole role) {
    if (role != DiagnosticRole::client) {
        return result(
            "uwf", DiagnosticSeverity::not_applicable,
            L"Reboot-to-restore scope", L"Phạm vi khôi phục sau reboot",
            L"UWF is client-only. The server keeps persistent exam, lecture, enrollment, and diagnostic data.",
            L"UWF chỉ áp dụng cho client. Server giữ dữ liệu exam, bài giảng, enrollment và diagnostics một cách bền vững.",
            L"Run UWF qualification with --target=client on a separate client image.",
            L"Chạy qualification UWF bằng --target=client trên image client riêng.");
    }
    UwfProbeSnapshot snapshot;
    snapshot.product_type = os_product_type();
    query_optional_feature(snapshot.feature_known, snapshot.feature_enabled);
    query_uwf_filter(snapshot);
    const auto state = classify_uwf(snapshot);
    const auto edition = os_product_name();
    if (state == UwfState::unsupported_edition) {
        return result(
            "uwf", DiagnosticSeverity::warning, L"Reboot-to-restore support",
            L"Hỗ trợ khôi phục sau reboot",
            L"Current edition " + edition +
                L" does not support Microsoft UWF.",
            L"Edition hiện tại " + edition + L" không hỗ trợ Microsoft UWF.",
            L"Install a properly licensed Education, Enterprise, or IoT Enterprise edition, or use supported third-party freezing software.",
            L"Cài Windows Education, Enterprise hoặc IoT Enterprise có bản quyền, hoặc dùng phần mềm đóng băng bên thứ ba được hỗ trợ.");
    }
    if (state == UwfState::probe_unavailable) {
        return result(
            "uwf", DiagnosticSeverity::warning, L"Reboot-to-restore probe",
            L"Probe khôi phục sau reboot",
            L"The Windows optional-feature state could not be determined by the read-only UWF probe; no UWF change was attempted.",
            L"Không thể xác định trạng thái optional feature Windows bằng probe UWF chỉ đọc; không thực hiện thay đổi UWF nào.",
            L"Retry from an elevated diagnostic session and verify the Windows WMI/optional-feature providers. Do not enable UWF based on this result alone.",
            L"Thử lại từ phiên diagnostics có quyền nâng cao và kiểm tra provider WMI/optional feature của Windows. Không bật UWF chỉ dựa trên kết quả này.",
            17);
    }
    if (state == UwfState::feature_missing) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                      L"This edition may support UWF, but the UWF optional feature is not enabled.",
                      L"Edition này có thể hỗ trợ UWF nhưng optional feature UWF chưa được bật.",
                      L"A future maintenance workflow must enable the documented Windows feature and reboot; this diagnostic does not change it.",
                      L"Quy trình maintenance tương lai phải bật feature Windows được tài liệu hóa và reboot; chẩn đoán này không tự thay đổi.");
    }
    if (state == UwfState::provider_unavailable) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                      L"The UWF WMI provider is unavailable on this installation.",
                      L"UWF WMI provider không khả dụng trên installation này.",
                      L"Verify the exact Windows image and optional-feature installation.",
                      L"Kiểm tra image Windows và việc cài optional feature chính xác.");
    }
    if (state == UwfState::inconsistent) {
        return result("uwf", DiagnosticSeverity::failure,
                      L"UWF state", L"Trạng thái UWF",
                      L"Current and next UWF states are inconsistent; recovery is required.",
                      L"Trạng thái UWF hiện tại và kế tiếp không nhất quán; cần recovery.",
                      L"Do not mutate UWF. Use the documented recovery image and technician procedure.",
                      L"Không thay đổi UWF. Dùng recovery image và quy trình kỹ thuật viên đã tài liệu hóa.", 2);
    }
    if (state == UwfState::reboot_pending) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"UWF state", L"Trạng thái UWF",
                      L"UWF configuration is waiting for a Windows restart.",
                      L"Cấu hình UWF đang chờ Windows restart.");
    }
    if (state == UwfState::enabled) {
        return result("uwf", DiagnosticSeverity::pass,
                      L"UWF state", L"Trạng thái UWF",
                      L"UWF is enabled for the current and next session.",
                      L"UWF đang bật cho phiên hiện tại và kế tiếp.");
    }
    return result("uwf", DiagnosticSeverity::pass,
                  L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                  L"Supported edition and UWF provider detected; protection is not enabled.",
                  L"Đã phát hiện edition và provider UWF được hỗ trợ; protection chưa bật.");
}

std::wstring volume_display_name(const UwfVolumeEntry& entry) {
    if (!entry.drive_letter.empty()) {
        return entry.drive_letter;
    }
    if (!entry.volume_name.empty()) {
        return L"volume-id";
    }
    return L"unmounted-volume";
}

DiagnosticResult check_uwf_volumes(DiagnosticRole role) {
    if (role != DiagnosticRole::client ||
        !is_uwf_supported_product(os_product_type())) {
        return result(
            "uwf_volumes", DiagnosticSeverity::not_applicable,
            L"Protected UWF volumes", L"Volume UWF được bảo vệ",
            L"Protected-volume probing is skipped because UWF is not applicable to this role or Windows edition.",
            L"Đã bỏ qua probe volume được bảo vệ vì UWF không áp dụng cho role hoặc edition Windows này.");
    }
    UwfVolumeSummary summary;
    if (!query_uwf_volumes(summary) || !summary.query_known) {
        return result(
            "uwf_volumes", DiagnosticSeverity::warning,
            L"Protected UWF volumes", L"Volume UWF được bảo vệ",
            L"The UWF_Volume provider could not be queried; protected-volume state is unavailable.",
            L"Không thể truy vấn provider UWF_Volume; chưa xác định được trạng thái volume được bảo vệ.",
            L"Verify the UWF optional feature/provider locally. Diagnostics did not change volume protection.",
            L"Kiểm tra optional feature/provider UWF tại máy. Diagnostics không thay đổi bảo vệ volume.",
            18);
    }
    std::uint32_t current_total = 0;
    std::uint32_t next_total = 0;
    std::uint32_t current_protected = 0;
    std::uint32_t next_protected = 0;
    std::wstring names;
    for (const auto& entry : summary.entries) {
        if (!entry.session_known || !entry.protected_known) {
            continue;
        }
        if (entry.current_session) {
            ++current_total;
            if (entry.protected_state) {
                ++current_protected;
            }
        } else {
            ++next_total;
            if (entry.protected_state) {
                ++next_protected;
            }
        }
        if (entry.protected_state && names.size() < 160) {
            if (!names.empty()) {
                names.append(L", ");
            }
            names.append(volume_display_name(entry));
        }
    }
    std::wostringstream detail;
    detail << L"Current session: " << current_protected << L" protected / "
           << current_total << L" records; next session: " << next_protected
           << L" protected / " << next_total << L" records";
    if (!names.empty()) {
        detail << L"; protected: " << names;
    }
    std::wostringstream detail_vi;
    detail_vi << L"Phiên hiện tại: " << current_protected << L" được bảo vệ / "
              << current_total << L" bản ghi; phiên kế tiếp: " << next_protected
              << L" được bảo vệ / " << next_total << L" bản ghi";
    if (!names.empty()) {
        detail_vi << L"; volume được bảo vệ: " << names;
    }

    const auto same_volumes = detail::protected_volumes_match(summary);
    if (!same_volumes.has_value()) {
        return result(
            "uwf_volumes", DiagnosticSeverity::warning,
            L"Protected UWF volumes", L"Volume UWF được bảo vệ", detail.str(),
            detail_vi.str(),
            L"One or more UWF volume records had unreadable properties; do not approve a restore policy until rechecked.",
            L"Một hoặc nhiều bản ghi volume UWF không đọc được thuộc tính; không phê duyệt policy restore trước khi kiểm tra lại.",
            19);
    }
    if (summary.entries.empty() || current_protected == 0 || next_protected == 0) {
        return result(
            "uwf_volumes", DiagnosticSeverity::warning,
            L"Protected UWF volumes", L"Volume UWF được bảo vệ", detail.str(),
            detail_vi.str(),
            L"No protected volume is confirmed for both sessions; configure and verify the reviewed client policy before rollout.",
            L"Chưa xác nhận volume được bảo vệ ở cả hai phiên; cấu hình và kiểm tra policy client đã duyệt trước khi triển khai.",
            20);
    }
    if (!*same_volumes) {
        return result(
            "uwf_volumes", DiagnosticSeverity::warning,
            L"Protected UWF volumes", L"Volume UWF được bảo vệ", detail.str(),
            detail_vi.str(),
            L"Current and next protected-volume sets differ; a restart is pending.",
            L"Tập volume được bảo vệ hiện tại và kế tiếp khác nhau; đang chờ restart.",
            21);
    }
    return result("uwf_volumes", DiagnosticSeverity::pass,
                  L"Protected UWF volumes", L"Volume UWF được bảo vệ",
                  detail.str(), detail_vi.str());
}

DiagnosticResult check_uwf_exclusions(DiagnosticRole role) {
    if (role != DiagnosticRole::client ||
        !is_uwf_supported_product(os_product_type())) {
        return result(
            "uwf_exclusions", DiagnosticSeverity::not_applicable,
            L"UWF file exclusions", L"Exclusion file UWF",
            L"UWF exclusion probing is skipped because UWF is not applicable to this role or Windows edition.",
            L"Đã bỏ qua probe exclusion UWF vì UWF không áp dụng cho role hoặc edition Windows này.");
    }
    UwfExclusionSummary summary;
    if (!query_uwf_exclusions(summary) || !summary.query_known) {
        return result(
            "uwf_exclusions", DiagnosticSeverity::warning,
            L"UWF file exclusions", L"Exclusion file UWF",
            L"UWF exclusions could not be read from the provider.",
            L"Không thể đọc exclusion UWF từ provider.",
            L"Verify the UWF provider locally. Diagnostics never adds, removes, or changes exclusions.",
            L"Kiểm tra provider UWF tại máy. Diagnostics không thêm, xóa hoặc thay đổi exclusion.",
            22);
    }
    if (summary.current_volume_count == 0 && summary.next_volume_count == 0) {
        return result(
            "uwf_exclusions", DiagnosticSeverity::warning,
            L"UWF file exclusions", L"Exclusion file UWF",
            L"No UWF volume records were returned, so exclusions cannot be assessed.",
            L"Không có bản ghi volume UWF; chưa thể đánh giá exclusion.",
            L"Re-run the read-only probe after the UWF provider is healthy.",
            L"Chạy lại probe chỉ đọc sau khi provider UWF hoạt động bình thường.",
            23);
    }
    std::wostringstream detail;
    detail << L"Current session: " << summary.current_exclusion_count
           << L" exclusions across " << summary.current_volume_count
           << L" volume records; next session: " << summary.next_exclusion_count
           << L" exclusions across " << summary.next_volume_count
           << L" volume records";
    std::wostringstream detail_vi;
    detail_vi << L"Phiên hiện tại: " << summary.current_exclusion_count
              << L" exclusion trên " << summary.current_volume_count
              << L" bản ghi volume; phiên kế tiếp: "
              << summary.next_exclusion_count << L" exclusion trên "
              << summary.next_volume_count << L" bản ghi volume";
    if (!summary.current_known || !summary.next_known) {
        return result(
            "uwf_exclusions", DiagnosticSeverity::warning,
            L"UWF file exclusions", L"Exclusion file UWF", detail.str(),
            detail_vi.str(),
            L"One session's exclusion list could not be read completely; review it locally before enabling protection.",
            L"Không thể đọc đầy đủ exclusion của một phiên; hãy kiểm tra tại máy trước khi bật bảo vệ.",
            24);
    }
    if (summary.current_exclusion_count != 0 ||
        summary.next_exclusion_count != 0) {
        return result(
            "uwf_exclusions", DiagnosticSeverity::warning,
            L"UWF file exclusions", L"Exclusion file UWF", detail.str(),
            detail_vi.str(),
            L"Exclusions create persistent write paths; verify every entry against the reviewed policy.",
            L"Exclusion tạo đường ghi bền vững; kiểm tra từng mục theo policy đã duyệt.",
            25);
    }
    return result("uwf_exclusions", DiagnosticSeverity::pass,
                  L"UWF file exclusions", L"Exclusion file UWF", detail.str(),
                  detail_vi.str());
}

std::wstring overlay_type_name(std::uint32_t type) {
    switch (type) {
    case 0:
        return L"RAM";
    case 1:
        return L"disk";
    default:
        return L"unknown (" + std::to_wstring(type) + L")";
    }
}

DiagnosticResult check_uwf_overlay(DiagnosticRole role) {
    if (role != DiagnosticRole::client ||
        !is_uwf_supported_product(os_product_type())) {
        return result(
            "uwf_overlay", DiagnosticSeverity::not_applicable,
            L"UWF overlay configuration", L"Cấu hình overlay UWF",
            L"UWF overlay probing is skipped because UWF is not applicable to this role or Windows edition.",
            L"Đã bỏ qua probe overlay UWF vì UWF không áp dụng cho role hoặc edition Windows này.");
    }
    UwfOverlaySummary summary;
    if (!query_uwf_overlay(summary) || !summary.query_known) {
        return result(
            "uwf_overlay", DiagnosticSeverity::warning,
            L"UWF overlay configuration", L"Cấu hình overlay UWF",
            L"UWF overlay configuration and consumption could not be read.",
            L"Không thể đọc cấu hình và mức sử dụng overlay UWF.",
            L"Verify the UWF provider locally. Diagnostics never changes overlay type, size, or thresholds.",
            L"Kiểm tra provider UWF tại máy. Diagnostics không thay đổi loại, kích thước hoặc threshold overlay.",
            26);
    }
    std::wostringstream detail;
    detail << L"Current: ";
    if (summary.current_config_known) {
        detail << overlay_type_name(summary.current_type) << L", max "
               << summary.current_maximum_size_mb << L" MB";
    } else {
        detail << L"unavailable";
    }
    detail << L"; next: ";
    if (summary.next_config_known) {
        detail << overlay_type_name(summary.next_type) << L", max "
               << summary.next_maximum_size_mb << L" MB";
    } else {
        detail << L"unavailable";
    }
    if (summary.consumption_known) {
        detail << L"; consumption " << summary.consumption_mb << L" MB"
               << L" (warning " << summary.warning_threshold_mb << L" MB,"
               << L" critical " << summary.critical_threshold_mb << L" MB)";
    }
    std::wostringstream detail_vi;
    detail_vi << L"Hiện tại: ";
    if (summary.current_config_known) {
        detail_vi << overlay_type_name(summary.current_type) << L", tối đa "
                  << summary.current_maximum_size_mb << L" MB";
    } else {
        detail_vi << L"không khả dụng";
    }
    detail_vi << L"; kế tiếp: ";
    if (summary.next_config_known) {
        detail_vi << overlay_type_name(summary.next_type) << L", tối đa "
                  << summary.next_maximum_size_mb << L" MB";
    } else {
        detail_vi << L"không khả dụng";
    }
    if (summary.consumption_known) {
        detail_vi << L"; đã dùng " << summary.consumption_mb << L" MB"
                  << L" (warning " << summary.warning_threshold_mb << L" MB,"
                  << L" critical " << summary.critical_threshold_mb << L" MB)";
    }

    if (!summary.config_known || !summary.current_config_known || !summary.next_config_known ||
        !summary.consumption_known) {
        return result(
            "uwf_overlay", DiagnosticSeverity::warning,
            L"UWF overlay configuration", L"Cấu hình overlay UWF", detail.str(),
            detail_vi.str(),
            L"Overlay state is incomplete; do not approve a restore policy until all read-only fields are available.",
            L"Trạng thái overlay chưa đầy đủ; không phê duyệt policy restore trước khi đọc đủ các trường chỉ đọc.",
            27);
    }
    const auto severity = detail::overlay_severity(summary);
    if (severity == DiagnosticSeverity::failure) {
        return result(
            "uwf_overlay", DiagnosticSeverity::failure,
            L"UWF overlay configuration", L"Cấu hình overlay UWF", detail.str(),
            detail_vi.str(),
            L"Overlay limits are invalid or the critical threshold has been reached. Stop rollout and follow the local recovery runbook.",
            L"Giới hạn overlay không hợp lệ hoặc đã chạm critical threshold. Dừng triển khai và theo runbook recovery tại máy.",
            28);
    }
    if (severity == DiagnosticSeverity::warning) {
        return result(
            "uwf_overlay", DiagnosticSeverity::warning,
            L"UWF overlay configuration", L"Cấu hình overlay UWF", detail.str(),
            detail_vi.str(),
            L"The overlay policy is pending restart or has crossed its warning threshold; review before classroom use.",
            L"Policy overlay đang chờ restart hoặc đã vượt warning threshold; kiểm tra trước khi dùng trong lớp.",
            29);
    }
    return result("uwf_overlay", DiagnosticSeverity::pass,
                  L"UWF overlay configuration", L"Cấu hình overlay UWF",
                  detail.str(), detail_vi.str());
}

DiagnosticResult check_uwf_events(DiagnosticRole role) {
    if (role != DiagnosticRole::client ||
        !is_uwf_supported_product(os_product_type())) {
        return result(
            "uwf_events", DiagnosticSeverity::not_applicable,
            L"UWF event health", L"Sức khỏe event UWF",
            L"UWF event probing is skipped because UWF is not applicable to this role or Windows edition.",
            L"Đã bỏ qua probe event UWF vì UWF không áp dụng cho role hoặc edition Windows này.");
    }
    UwfEventHealthSummary summary;
    if (!query_uwf_event_health(summary) || !summary.query_known) {
        return result(
            "uwf_events", DiagnosticSeverity::warning,
            L"UWF event health", L"Sức khỏe event UWF",
            L"UWF event channels could not be queried; recent overlay/configuration health is unknown.",
            L"Không thể truy vấn các channel event UWF; chưa xác định sức khỏe overlay/cấu hình gần đây.",
            L"Verify read access to the System and UnifiedWriteFilter event channels. No event or UWF state was changed.",
            L"Kiểm tra quyền đọc channel System và UnifiedWriteFilter. Không thay đổi event hoặc trạng thái UWF.",
            30);
    }
    const auto health = classify_uwf_event_health(summary);
    std::wostringstream detail;
    detail << L"System uwfvol: warning " << summary.warning_events
           << L", critical " << summary.critical_events << L", info "
           << summary.informational_events << L"; Admin errors "
           << summary.admin_error_events << L", warnings "
           << summary.admin_warning_events << L"; channels: system "
           << (summary.system_channel_known ? L"ok" : L"unavailable")
           << L", admin "
           << (summary.admin_channel_known ? L"ok" : L"unavailable")
           << L", operational "
           << (summary.operational_channel_known ? L"ok" : L"unavailable");
    if (summary.truncated) {
        detail << L"; bounded query limit reached";
    }
    std::wostringstream detail_vi;
    detail_vi << L"System uwfvol: warning " << summary.warning_events
              << L", critical " << summary.critical_events << L", info "
              << summary.informational_events << L"; Admin errors "
              << summary.admin_error_events << L", warnings "
              << summary.admin_warning_events << L"; channel: system "
              << (summary.system_channel_known ? L"ok" : L"không khả dụng")
              << L", admin "
              << (summary.admin_channel_known ? L"ok" : L"không khả dụng")
              << L", operational "
              << (summary.operational_channel_known ? L"ok" : L"không khả dụng");
    if (summary.truncated) {
        detail_vi << L"; đã chạm giới hạn truy vấn";
    }
    if (health == UwfEventHealth::critical) {
        return result(
            "uwf_events", DiagnosticSeverity::failure,
            L"UWF event health", L"Sức khỏe event UWF", detail.str(),
            detail_vi.str(),
            L"Recent UWF critical/error events were detected. Stop rollout and inspect the local event details.",
            L"Đã phát hiện event critical/error UWF gần đây. Dừng triển khai và kiểm tra chi tiết event tại máy.",
            31);
    }
    if (health == UwfEventHealth::warning || summary.truncated) {
        return result(
            "uwf_events", DiagnosticSeverity::warning,
            L"UWF event health", L"Sức khỏe event UWF", detail.str(),
            detail_vi.str(),
            L"Recent UWF warnings or a bounded event query require local review before rollout.",
            L"Có warning UWF gần đây hoặc truy vấn event bị giới hạn; cần kiểm tra tại máy trước khi triển khai.",
            32);
    }
    return result("uwf_events", DiagnosticSeverity::pass,
                  L"UWF event health", L"Sức khỏe event UWF", detail.str(),
                  detail_vi.str());
}

DiagnosticResult check_safe_mode() {
    const int mode = GetSystemMetrics(SM_CLEANBOOT);
    if (mode != 0) {
        return result("safe_mode", DiagnosticSeverity::failure,
                      L"Safe Mode", L"Safe Mode",
                      L"Windows is running in Safe Mode; normal NSTU service, graphics, and networking are unavailable.",
                      L"Windows đang chạy Safe Mode; service, đồ họa và mạng NSTU bình thường không khả dụng.",
                      L"Restart Windows normally before installation or classroom operation.",
                      L"Khởi động lại Windows ở chế độ bình thường trước khi cài đặt hoặc vận hành lớp học.", 3);
    }
    return result("safe_mode", DiagnosticSeverity::pass, L"Safe Mode",
                  L"Safe Mode", L"Normal Windows boot detected.",
                  L"Đã phát hiện Windows khởi động bình thường.");
}

const NetworkScan* fastest_physical_network(const HardwareScan& hardware) {
    const auto is_usable = [](const NetworkScan& network) {
        // Tunnel adapters (including Tailscale) report overlay capacity, not
        // the negotiated Ethernet/Wi-Fi link used by classroom traffic.
        return network.operational && !network.tunnel &&
               network.interface_type != IF_TYPE_SOFTWARE_LOOPBACK;
    };
    const NetworkScan* fastest = nullptr;
    for (const auto& network : hardware.networks) {
        if (!is_usable(network)) {
            continue;
        }
        if (fastest == nullptr ||
            std::max(network.transmit_link_speed_mbps,
                     network.receive_link_speed_mbps) >
                std::max(fastest->transmit_link_speed_mbps,
                         fastest->receive_link_speed_mbps)) {
            fastest = &network;
        }
    }
    return fastest;
}

DiagnosticResult check_hardware(const HardwareScan& hardware) {
    const auto cpu_readiness = classify_processor(
        hardware.processor.x64, hardware.processor.physical_cores,
        hardware.processor.logical_processors);
    const auto memory = classify_memory_gib(
        hardware.memory.total_physical_bytes / (1024ull * 1024ull * 1024ull));
    const auto* fastest = fastest_physical_network(hardware);
    const std::uint64_t link = fastest == nullptr
        ? 0
        : std::max(fastest->transmit_link_speed_mbps,
                   fastest->receive_link_speed_mbps);
    const auto network = classify_link_speed_mbps(link);
    const bool memory_failure = memory == Readiness::minimum_not_met;
    const bool network_failure = network == Readiness::minimum_not_met;
    const bool capacity_failure = memory_failure;
    const bool failure = network_failure ||
                         (capacity_failure && !kInternalTestBuild);
    const auto severity = failure
        ? DiagnosticSeverity::failure
        : (capacity_failure ? DiagnosticSeverity::warning
                            : DiagnosticSeverity::pass);
    std::wstring detail = hardware.processor.model + L" (" +
                          hardware.processor.architecture + L") | " +
                          std::to_wstring(hardware.processor.physical_cores) +
                          L" physical / " +
                          std::to_wstring(hardware.processor.logical_processors) +
                          L" logical cores | " +
                          std::to_wstring(hardware.memory.total_physical_bytes /
                                          (1024ull * 1024ull * 1024ull)) +
                          L" GiB RAM | " + std::to_wstring(link) +
                          L" Mbps link | " +
                          (cpu_readiness == Readiness::unrated
                               ? L"CPU information only"
                               : L"CPU assessment available");
    std::wstring remediation =
        L"Install minimum: 6 GiB RAM and a 100 Mbps physical link. Processor model and core counts are informational and never block installation; validate CPU performance with an NSTU workload test. Windows x64 remains required by the installer.";
    std::wstring remediation_vi =
        L"Mức tối thiểu để cài: RAM 6 GiB và link vật lý 100 Mbps. Model CPU và số core chỉ mang tính thông tin, không bao giờ chặn cài đặt; hãy kiểm tra hiệu năng CPU bằng workload NSTU. Installer vẫn yêu cầu Windows x64.";
    if (capacity_failure && kInternalTestBuild && !network_failure) {
        remediation =
            L"INTERNAL VM TEST override: RAM capacity is below the release minimum, but installation may continue for development testing only.";
        remediation_vi =
            L"Ghi đè INTERNAL VM TEST: RAM thấp hơn mức tối thiểu của bản phát hành nhưng có thể tiếp tục cài chỉ để kiểm thử phát triển.";
    }
    return result("hardware", severity,
                  L"Hardware readiness", L"Mức đáp ứng phần cứng", detail, detail,
                  std::move(remediation), std::move(remediation_vi),
                  failure ? 4 : 0);
}

DiagnosticResult check_graphics() {
    const auto graphics = scan_graphics();
    const auto severity = graphics.hardware_d3d11_available
        ? DiagnosticSeverity::pass
        : DiagnosticSeverity::warning;
    std::wstring detail = std::to_wstring(graphics.adapters.size()) +
                          L" DXGI adapter(s); D3D11 hardware: " +
                          (graphics.hardware_d3d11_available ? L"available" :
                                                               L"unavailable") +
                          L"; WARP: " +
                          (graphics.warp_d3d11_available ? L"available" :
                                                           L"unavailable");
    return result("graphics", severity, L"Graphics driver", L"Driver đồ họa",
                  detail, detail,
                  L"Install a supported display driver. WARP is a diagnostic fallback; it is not suitable for production streaming.",
                  L"Cài driver màn hình được hỗ trợ. WARP chỉ là dự phòng chẩn đoán; không phù hợp cho streaming production.",
                  graphics.hardware_d3d11_available ? 0 : 5);
}

DiagnosticResult check_encoder(DiagnosticRole role) {
    if (role != DiagnosticRole::server) {
        return result("encoder", DiagnosticSeverity::not_applicable,
                      L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                      L"Informational for the client role.",
                      L"Chỉ mang tính thông tin với role client.");
    }
    std::string error;
    const auto encoders = scan_hardware_h264_encoders(&error);
    if (encoders.empty()) {
        return result("encoder", DiagnosticSeverity::warning,
                      L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                      L"No hardware H.264 encoder was found; snapshot mode remains supported.",
                      L"Không tìm thấy bộ mã hóa H.264 phần cứng; chế độ snapshot vẫn được hỗ trợ.",
                      L"Continuous H.264 is deferred and must not block snapshot-first classroom operation.",
                      L"H.264 liên tục đang tạm hoãn và không được chặn vận hành snapshot trong lớp học.");
    }
    return result("encoder", DiagnosticSeverity::pass,
                  L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                  std::to_wstring(encoders.size()) + L" hardware encoder(s) registered.",
                  std::to_wstring(encoders.size()) + L" bộ mã hóa phần cứng đã đăng ký.");
}

DiagnosticResult check_network(const HardwareScan& hardware) {
    const auto* fastest = fastest_physical_network(hardware);
    if (fastest == nullptr) {
        return result("network", DiagnosticSeverity::failure,
                      L"Network link", L"Kết nối mạng",
                      L"No operational physical Ethernet or Wi-Fi adapter was found; tunnel adapters are not accepted as link-capacity evidence.",
                      L"Không tìm thấy adapter Ethernet hoặc Wi-Fi vật lý đang hoạt động; adapter tunnel không được dùng làm bằng chứng dung lượng link.",
                      L"Connect an Ethernet or Wi-Fi adapter before installation.",
                      L"Kết nối adapter Ethernet hoặc Wi-Fi trước khi cài đặt.", 6);
    }
    const auto link = std::max(fastest->transmit_link_speed_mbps,
                               fastest->receive_link_speed_mbps);
    const auto severity = link < 100 ? DiagnosticSeverity::warning
                                     : DiagnosticSeverity::pass;
    const std::wstring detail = fastest->adapter_name + L": " +
                                std::to_wstring(link) + L" Mbps negotiated link";
    return result("network", severity, L"Network link", L"Kết nối mạng",
                  detail, detail,
                  L"100 Mbps is the minimum; 1 Gbps is recommended for larger rooms. This is link capacity, not an Internet speed test.",
                  L"100 Mbps là mức tối thiểu; khuyến nghị 1 Gbps cho phòng lớn. Đây là dung lượng link, không phải đo tốc độ Internet.",
                  link < 100 ? 7 : 0);
}

DiagnosticResult check_server(const DiagnosticOptions& options) {
    if (options.role != DiagnosticRole::client) {
        return result("server", DiagnosticSeverity::not_applicable,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"Not required for the server role.",
                      L"Không yêu cầu với role server.");
    }
    if (options.server_address.empty()) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"No NSTU server address is configured.",
                      L"Chưa cấu hình địa chỉ server NSTU.",
                      L"Set the school-LAN server address before client enrollment.",
                      L"Đặt địa chỉ server trong mạng trường trước khi enrollment client.", 8);
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"Winsock initialization failed.", L"Khởi tạo Winsock thất bại.", {}, {}, 9);
    }
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* addresses = nullptr;
    const auto service = std::to_wstring(options.server_port);
    const int resolve = GetAddrInfoW(options.server_address.c_str(), service.c_str(),
                                     &hints, &addresses);
    bool connected = false;
    if (resolve == 0) {
        for (auto* address = addresses; address != nullptr && !connected;
             address = address->ai_next) {
            SOCKET socket_handle = socket(address->ai_family,
                                           address->ai_socktype,
                                           address->ai_protocol);
            if (socket_handle == INVALID_SOCKET) {
                continue;
            }
            u_long non_blocking = 1;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            const int connect_result = connect(
                socket_handle, address->ai_addr,
                static_cast<int>(address->ai_addrlen));
            if (connect_result == 0) {
                connected = true;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(socket_handle, &write_set);
                timeval timeout{2, 0};
                if (select(0, nullptr, &write_set, nullptr, &timeout) > 0) {
                    int socket_error = 0;
                    int length = sizeof(socket_error);
                    connected = getsockopt(
                        socket_handle, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&socket_error), &length) == 0 &&
                        socket_error == 0;
                }
            }
            closesocket(socket_handle);
        }
    }
    if (addresses != nullptr) {
        FreeAddrInfoW(addresses);
    }
    WSACleanup();
    if (!connected) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"The configured NSTU server cannot be reached on TCP " +
                          std::to_wstring(options.server_port) + L".",
                      L"Không thể kết nối server NSTU qua TCP " +
                          std::to_wstring(options.server_port) + L".",
                      L"Verify the school-LAN address, firewall, and server listener.",
                      L"Kiểm tra địa chỉ trong mạng trường, firewall và listener server.", 10);
    }
    return result("server", DiagnosticSeverity::pass,
                  L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                  L"TCP endpoint is reachable.", L"TCP endpoint có thể kết nối.");
}

DiagnosticResult check_time() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    const bool sane = time.wYear >= 2020 && time.wYear <= 2100;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager == nullptr
        ? nullptr
        : OpenServiceW(manager, L"W32Time", SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    const bool running = service != nullptr &&
        QueryServiceStatus(service, &status) != FALSE &&
        status.dwCurrentState == SERVICE_RUNNING;
    if (service != nullptr) CloseServiceHandle(service);
    if (manager != nullptr) CloseServiceHandle(manager);
    const auto severity = sane && running ? DiagnosticSeverity::pass
                                          : DiagnosticSeverity::warning;
    return result("time", severity, L"System time", L"Thời gian hệ thống",
                  sane ? (running ? L"System time is sane and Windows Time is running."
                                  : L"System time is sane; Windows Time is not running.")
                       : L"System time is outside the expected deployment range.",
                  sane ? (running ? L"Thời gian hợp lệ và Windows Time đang chạy."
                                  : L"Thời gian hợp lệ; Windows Time chưa chạy.")
                       : L"Thời gian hệ thống nằm ngoài khoảng triển khai dự kiến.",
                  L"Do not adjust time automatically; synchronize it through the school's approved policy.",
                  L"Không tự động chỉnh giờ; đồng bộ theo policy được trường phê duyệt.", sane ? 0 : 11);
}

DiagnosticResult check_internet() {
    ComPtr<INetworkListManager> manager;
    const HRESULT created = CoCreateInstance(
        CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&manager));
    if (FAILED(created)) {
        return result("internet", DiagnosticSeverity::not_applicable,
                      L"Public Internet", L"Internet công cộng",
                      L"Public Internet state is unavailable; it is not required for classroom control.",
                      L"Không đọc được trạng thái Internet công cộng; không bắt buộc cho điều khiển lớp học.");
    }
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    const HRESULT queried = manager->GetConnectivity(&connectivity);
    const bool connected = SUCCEEDED(queried) &&
        (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET |
                         NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    const auto severity = connected
        ? DiagnosticSeverity::pass : DiagnosticSeverity::warning;
    return result("internet", severity, L"Public Internet", L"Internet công cộng",
                  connected ? L"Windows reports an Internet connection."
                            : L"No public Internet connection reported; school-LAN control can still operate.",
                  connected ? L"Windows báo có kết nối Internet."
                            : L"Không có Internet công cộng; điều khiển trong mạng trường vẫn có thể hoạt động.");
}

DiagnosticResult check_installation(const DiagnosticOptions& options) {
    std::wstring role;
    const bool has_role = read_reg_string(HKEY_LOCAL_MACHINE, L"Software\\NSTU",
                                          L"InstallRole", role);
    std::wstring location;
    const bool has_location = read_reg_string(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NSTU",
        L"InstallLocation", location);
    const std::filesystem::path root = has_location
        ? std::filesystem::path(location)
        : std::filesystem::path();
    const bool client_binary = !root.empty() &&
        std::filesystem::exists(root / L"client" / L"nstu-service.exe");
    const bool server_binary = !root.empty() &&
        std::filesystem::exists(root / L"server" / L"nstu-server.exe");
    const bool conflict = client_binary && server_binary;
    const bool expected = !options.boot_check ||
        (options.role == DiagnosticRole::client ? client_binary : server_binary);
    if (conflict) {
        return result("installation", DiagnosticSeverity::failure,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"Both Client and Server payloads are installed in one root.",
                      L"Cả payload Client và Server đang được cài trong cùng một root.",
                      L"Uninstall the conflicting role and restart before installing one role.",
                      L"Gỡ role xung đột và restart trước khi cài một role.", 12);
    }
    if (!has_role && !options.boot_check) {
        return result("installation", DiagnosticSeverity::pass,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"No existing NSTU installation was found; installer preflight may continue.",
                      L"Chưa tìm thấy NSTU đã cài; installer preflight có thể tiếp tục.");
    }
    if (options.boot_check && !expected) {
        return result("installation", DiagnosticSeverity::failure,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"The expected NSTU role binary is missing.",
                      L"Thiếu binary NSTU của role được yêu cầu.",
                      L"Repair or reinstall the selected role using the signed installer.",
                      L"Sửa chữa hoặc cài lại role bằng installer đã ký.", 13);
    }
    return result("installation", DiagnosticSeverity::pass,
                  L"Installation integrity", L"Toàn vẹn cài đặt",
                  L"NSTU role and installation metadata are consistent.",
                  L"Role NSTU và metadata cài đặt nhất quán.");
}

std::wstring normalized_image_path(const std::filesystem::path& path) {
    auto value = path.lexically_normal().wstring();
    if (value.starts_with(L"\\\\?\\")) {
        value.erase(0, 4);
    }
    while (value.size() > 3 &&
           (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool installed_agent_running_in_session(
    const std::filesystem::path& expected_path, DWORD session_id) {
    const HANDLE process_snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (process_snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    const auto expected = normalized_image_path(expected_path);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(process_snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(entry.szExeFile, L"nstu-agent.exe") != 0) {
                continue;
            }
            DWORD process_session = 0;
            if (ProcessIdToSessionId(entry.th32ProcessID, &process_session) ==
                    FALSE ||
                process_session != session_id) {
                continue;
            }
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            std::array<wchar_t, 32'768> image{};
            DWORD image_length = static_cast<DWORD>(image.size());
            const bool queried = QueryFullProcessImageNameW(
                process, 0, image.data(), &image_length) != FALSE;
            CloseHandle(process);
            if (!queried || image_length == 0 ||
                image_length >= image.size()) {
                continue;
            }
            const auto actual = normalized_image_path(
                std::filesystem::path(
                    std::wstring_view(image.data(), image_length)));
            if (_wcsicmp(actual.c_str(), expected.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(process_snapshot, &entry) != FALSE);
    }
    CloseHandle(process_snapshot);
    return found;
}

DiagnosticResult check_service(const DiagnosticOptions& options) {
    if (options.role != DiagnosticRole::client || !options.boot_check) {
        return result("service", DiagnosticSeverity::not_applicable,
                      L"Client runtime", L"Tiến trình client",
                      L"Client runtime verification runs on client boot checks.",
                      L"Kiểm tra tiến trình client chạy trong boot check của client.");
    }
    ClientRuntimeSnapshot runtime;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager == nullptr
        ? nullptr
        : OpenServiceW(manager, L"nstu-service",
                       SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        if (manager != nullptr) CloseServiceHandle(manager);
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Tiến trình client",
                      L"nstu-service is missing or cannot be queried.",
                      L"Thiếu nstu-service hoặc không thể truy vấn.", {}, {}, 14);
    }
    runtime.service_present = true;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    runtime.service_running = QueryServiceStatusEx(
        service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
        sizeof(status), &bytes) != FALSE &&
        status.dwCurrentState == SERVICE_RUNNING;
    DWORD service_session = std::numeric_limits<DWORD>::max();
    runtime.service_session_zero = runtime.service_running &&
        status.dwProcessId != 0 &&
        ProcessIdToSessionId(status.dwProcessId, &service_session) != FALSE &&
        service_session == 0;
    DWORD required = 0;
    QueryServiceConfigW(service, nullptr, 0, &required);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && required > 0) {
        std::vector<std::byte> buffer(required);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, required, &required)) {
            runtime.service_automatic =
                config->dwStartType == SERVICE_AUTO_START;
            runtime.service_local_system =
                config->lpServiceStartName != nullptr &&
                (_wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0 ||
                 _wcsicmp(config->lpServiceStartName,
                          L"NT AUTHORITY\\SYSTEM") == 0);
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    std::wstring installation_root;
    const bool has_root = read_reg_string(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NSTU",
        L"InstallLocation", installation_root);
    const std::filesystem::path agent_path = has_root
        ? std::filesystem::path(installation_root) / L"client" /
              L"nstu-agent.exe"
        : std::filesystem::path();
    std::error_code agent_status_error;
    runtime.agent_binary_present = !agent_path.empty() &&
        std::filesystem::is_regular_file(agent_path, agent_status_error) &&
        !agent_status_error;

    DWORD interactive_session = 0;
    runtime.interactive_session = ProcessIdToSessionId(
        GetCurrentProcessId(), &interactive_session) != FALSE &&
        interactive_session != 0 &&
        interactive_session != std::numeric_limits<DWORD>::max();
    if (runtime.service_running && runtime.service_automatic &&
        runtime.service_local_system && runtime.service_session_zero &&
        runtime.agent_binary_present && runtime.interactive_session) {
        constexpr int kAgentProbeAttempts = 20;
        constexpr DWORD kAgentProbeDelayMs = 250;
        for (int attempt = 0; attempt < kAgentProbeAttempts; ++attempt) {
            if (installed_agent_running_in_session(agent_path,
                                                   interactive_session)) {
                runtime.agent_running_in_session = true;
                break;
            }
            if (attempt + 1 < kAgentProbeAttempts) {
                Sleep(kAgentProbeDelayMs);
            }
        }
    }

    const auto state = classify_client_runtime(runtime);
    switch (state) {
    case ClientRuntimeState::ready:
        return result(
            "service", DiagnosticSeverity::pass,
            L"Client runtime", L"Client runtime",
            L"The automatic LocalSystem service is running in Session 0 and the installed agent is running in this interactive session.",
            L"Service LocalSystem tự động đang chạy trong Session 0 và agent đã cài đang chạy trong session tương tác này.");
    case ClientRuntimeState::service_missing:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"nstu-service is missing or cannot be queried.",
                      L"Thiếu nstu-service hoặc không thể truy vấn.", {}, {},
                      14);
    case ClientRuntimeState::service_not_running:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"nstu-service is installed but is not running.",
                      L"nstu-service đã cài nhưng không chạy.", {}, {}, 15);
    case ClientRuntimeState::service_not_automatic:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"nstu-service is not configured for automatic startup.",
                      L"nstu-service chưa được cấu hình tự khởi động.", {}, {},
                      16);
    case ClientRuntimeState::service_wrong_account:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"nstu-service is not configured for LocalSystem.",
                      L"nstu-service chưa được cấu hình chạy bằng LocalSystem.",
                      {}, {}, 17);
    case ClientRuntimeState::service_wrong_session:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"nstu-service is not running in Session 0.",
                      L"nstu-service không chạy trong Session 0.", {}, {}, 18);
    case ClientRuntimeState::agent_binary_missing:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"The installed nstu-agent.exe binary is missing.",
                      L"Thiếu binary nstu-agent.exe đã cài.", {}, {}, 19);
    case ClientRuntimeState::interactive_session_unavailable:
        return result("service", DiagnosticSeverity::failure,
                      L"Client runtime", L"Client runtime",
                      L"Diagnostics is not running in an interactive user session.",
                      L"Diagnostics không chạy trong session người dùng tương tác.",
                      {}, {}, 20);
    case ClientRuntimeState::agent_not_running:
    default:
        return result(
            "service", DiagnosticSeverity::failure,
            L"Client runtime", L"Client runtime",
            L"The installed nstu-agent.exe did not start in this interactive session.",
            L"nstu-agent.exe đã cài không khởi động trong session tương tác này.",
            L"Restart the client once. If the issue remains, repair the client role with the signed installer.",
            L"Restart client một lần. Nếu lỗi còn, sửa role client bằng installer đã ký.",
            21);
    }
}

DiagnosticResult check_registry() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"Software\\NSTU", L"ServerPort",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return result("registry", DiagnosticSeverity::warning,
                      L"NSTU registry", L"Registry NSTU",
                      L"NSTU registry values could not be read completely.",
                      L"Không thể đọc đầy đủ giá trị registry NSTU.", {}, {}, status);
    }
    if (status == ERROR_SUCCESS && (value == 0 || value > 65535)) {
        return result("registry", DiagnosticSeverity::failure,
                      L"NSTU registry", L"Registry NSTU",
                      L"Configured ServerPort is outside 1-65535.",
                      L"ServerPort cấu hình nằm ngoài 1-65535.",
                      L"Correct the configuration through the signed installer; diagnostics does not modify it.",
                      L"Sửa cấu hình bằng installer đã ký; diagnostics không tự thay đổi.", 16);
    }
    return result("registry", DiagnosticSeverity::pass,
                  L"NSTU registry", L"Registry NSTU",
                  L"Documented NSTU registry values are readable.",
                  L"Các giá trị registry NSTU được tài liệu hóa có thể đọc.");
}

} // namespace

UwfState classify_uwf(const UwfProbeSnapshot& snapshot) noexcept {
    if (!is_uwf_supported_product(snapshot.product_type)) {
        return UwfState::unsupported_edition;
    }
    if (!snapshot.feature_known) {
        return UwfState::probe_unavailable;
    }
    if (!snapshot.feature_enabled) {
        return UwfState::feature_missing;
    }
    if (!snapshot.provider_available || !snapshot.filter_state_known) {
        return UwfState::provider_unavailable;
    }
    if (snapshot.current_enabled != snapshot.next_enabled) {
        return UwfState::reboot_pending;
    }
    return snapshot.current_enabled ? UwfState::enabled
                                    : UwfState::available_unconfigured;
}

UwfEventHealth classify_uwf_event_health(
    const UwfEventHealthSummary& summary) noexcept {
    // A query that did not establish any channel state is not evidence of a
    // healthy UWF installation. Keep this distinction explicit so callers can
    // present a retry/read-access remediation instead of a false pass.
    if (!summary.query_known ||
        (!summary.system_channel_known && !summary.admin_channel_known &&
         !summary.operational_channel_known)) {
        return UwfEventHealth::unavailable;
    }
    if (summary.critical_events != 0 || summary.admin_error_events != 0) {
        return UwfEventHealth::critical;
    }
    if (summary.warning_events != 0 || summary.admin_warning_events != 0 ||
        summary.truncated || !summary.system_channel_known ||
        !summary.admin_channel_known || !summary.operational_channel_known) {
        return UwfEventHealth::warning;
    }
    return UwfEventHealth::healthy;
}

Readiness classify_memory_gib(std::uint64_t gib) noexcept {
    return gib >= 6 ? Readiness::good : Readiness::minimum_not_met;
}

Readiness classify_link_speed_mbps(std::uint64_t mbps) noexcept {
    if (mbps < 100) {
        return Readiness::minimum_not_met;
    }
    return mbps >= 1000 ? Readiness::good : Readiness::recommended_not_met;
}

Readiness classify_processor(bool, std::uint32_t,
                             std::uint32_t) noexcept {
    // VM topology and firmware reporting are unreliable performance gates.
    // Keep processor data in the report and qualify it with real workloads.
    return Readiness::unrated;
}

ClientRuntimeState classify_client_runtime(
    const ClientRuntimeSnapshot& snapshot) noexcept {
    if (!snapshot.service_present) {
        return ClientRuntimeState::service_missing;
    }
    if (!snapshot.service_running) {
        return ClientRuntimeState::service_not_running;
    }
    if (!snapshot.service_automatic) {
        return ClientRuntimeState::service_not_automatic;
    }
    if (!snapshot.service_local_system) {
        return ClientRuntimeState::service_wrong_account;
    }
    if (!snapshot.service_session_zero) {
        return ClientRuntimeState::service_wrong_session;
    }
    if (!snapshot.agent_binary_present) {
        return ClientRuntimeState::agent_binary_missing;
    }
    if (!snapshot.interactive_session) {
        return ClientRuntimeState::interactive_session_unavailable;
    }
    if (!snapshot.agent_running_in_session) {
        return ClientRuntimeState::agent_not_running;
    }
    return ClientRuntimeState::ready;
}

std::vector<DiagnosticCheck> diagnostic_checks(const DiagnosticOptions&) {
    std::vector<DiagnosticCheck> checks = {
        {"os", L"Operating system", L"Hệ điều hành"},
        {"uwf", L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot"},
        {"uwf_volumes", L"Protected UWF volumes", L"Volume UWF được bảo vệ"},
        {"uwf_exclusions", L"UWF file exclusions", L"Exclusion file UWF"},
        {"uwf_overlay", L"UWF overlay configuration", L"Cấu hình overlay UWF"},
        {"uwf_events", L"UWF event health", L"Sức khỏe event UWF"},
        {"safe_mode", L"Safe Mode", L"Safe Mode"},
        {"installation", L"Installation integrity", L"Toàn vẹn cài đặt"},
        {"registry", L"NSTU registry", L"Registry NSTU"},
        {"hardware", L"Hardware readiness", L"Mức đáp ứng phần cứng"},
        {"network", L"Network link", L"Kết nối mạng"},
        {"graphics", L"Graphics driver", L"Driver đồ họa"},
        {"encoder", L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng"},
        {"time", L"System time", L"Thời gian hệ thống"},
        {"internet", L"Public Internet", L"Internet công cộng"},
        {"server", L"NSTU server reachability", L"Khả năng kết nối server NSTU"},
    };
    checks.push_back({"service", L"Client runtime", L"Tiến trình client"});
    return checks;
}

void run_startup_diagnostics(const DiagnosticOptions& options,
                             const DiagnosticStartSink& on_start,
                             const DiagnosticResultSink& on_result) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        bool initialized;
        ~ComScope() { if (initialized) CoUninitialize(); }
    } com_scope{com == S_OK || com == S_FALSE};
    const auto hardware = scan_hardware();
    const auto checks = diagnostic_checks(options);
    auto run = [&](const DiagnosticCheck& check, auto&& function) {
        if (on_start) on_start(check);
        try {
            on_result(function());
        } catch (...) {
            on_result(result(check.id, DiagnosticSeverity::failure,
                             check.title_en.c_str(), check.title_vi.c_str(),
                             L"The diagnostic probe failed unexpectedly.",
                             L"Probe chẩn đoán thất bại ngoài dự kiến.", {}, {}, 0xffff));
        }
    };
    // Resolve checks by stable identifier rather than relying on vector
    // positions. This keeps the progress/report stream correct when a probe is
    // inserted or reordered, and avoids out-of-bounds access if the catalogue
    // changes independently of this dispatcher.
    auto run_named = [&](std::string_view id, auto&& function) {
        const auto found = std::find_if(
            checks.begin(), checks.end(),
            [id](const DiagnosticCheck& check) { return check.id == id; });
        if (found != checks.end()) {
            run(*found, std::forward<decltype(function)>(function));
        }
    };
    run_named("os", [] { return check_os(); });
    run_named("uwf", [&] { return check_uwf(options.role); });
    run_named("uwf_volumes", [&] { return check_uwf_volumes(options.role); });
    run_named("uwf_exclusions",
              [&] { return check_uwf_exclusions(options.role); });
    run_named("uwf_overlay", [&] { return check_uwf_overlay(options.role); });
    run_named("uwf_events", [&] { return check_uwf_events(options.role); });
    run_named("safe_mode", [] { return check_safe_mode(); });
    run_named("installation", [&] { return check_installation(options); });
    run_named("registry", [] { return check_registry(); });
    run_named("hardware", [&] { return check_hardware(hardware); });
    run_named("network", [&] { return check_network(hardware); });
    run_named("graphics", [] { return check_graphics(); });
    run_named("encoder", [&] { return check_encoder(options.role); });
    run_named("time", [] { return check_time(); });
    run_named("internet", [] { return check_internet(); });
    run_named("server", [&] { return check_server(options); });
    run_named("service", [&] { return check_service(options); });
}

std::string json_escape(std::wstring_view value) {
    const auto utf8 = wide_to_utf8(value);
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : utf8) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<unsigned int>(character)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

const char* severity_name(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::pass: return "pass";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::failure: return "failure";
    case DiagnosticSeverity::not_applicable: return "not_applicable";
    }
    return "unknown";
}

bool write_diagnostic_report_json(
    const std::filesystem::path& path, const DiagnosticOptions& options,
    const std::vector<DiagnosticResult>& results, std::string* error) {
    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            set_error(error, "diagnostic report directory could not be created");
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        set_error(error, "diagnostic report could not be opened");
        return false;
    }
    output << "{\n  \"role\": "
            << (options.role == DiagnosticRole::client ? "\"client\"" :
                                                          "\"server\"")
            << ",\n  \"build_channel\": "
            << (kInternalTestBuild ? "\"internal_vm_test\"" : "\"release\"")
            << ",\n  \"server_port\": " << options.server_port
            << ",\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& item = results[index];
        output << "    {\"id\": " << json_escape(utf8_to_wide(item.id))
                << ", \"severity\": \"" << severity_name(item.severity)
                << "\", \"detail_en\": " << json_escape(item.detail_en)
                << ", \"detail_vi\": " << json_escape(item.detail_vi)
                << ", \"error_code\": " << item.error_code << "}";
        if (index + 1 != results.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
    output.flush();
    if (!output) {
        set_error(error, "diagnostic report write failed");
        return false;
    }
    return true;
}

} // namespace nstu::setup
