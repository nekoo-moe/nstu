#include "nstu/setup/uwf.hpp"

#include <windows.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace nstu::setup {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kProductEnterprise = 0x00000004;
constexpr std::uint32_t kProductEnterpriseN = 0x0000001b;
constexpr std::uint32_t kProductEducation = 0x00000079;
constexpr std::uint32_t kProductEducationN = 0x0000007a;
constexpr std::uint32_t kProductEnterpriseS = 0x0000007d;
constexpr std::uint32_t kProductEnterpriseSN = 0x0000007e;
constexpr std::uint32_t kProductIotEnterprise = 0x000000bc;
constexpr std::uint32_t kProductIotEnterpriseS = 0x000000bf;
constexpr wchar_t kUwfNamespace[] = L"ROOT\\standardcimv2\\embedded";
// Microsoft requires a full registry path, not the HKLM abbreviation.
// Source: https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-registryfilteraddexclusion
constexpr wchar_t kNstuRegistryExclusion[] =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\NSTU";

class Bstr {
public:
    explicit Bstr(const wchar_t* value) : value_(SysAllocString(value)) {}
    explicit Bstr(std::wstring_view value)
        : value_(SysAllocStringLen(value.data(),
                                   static_cast<UINT>(value.size()))) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }

private:
    BSTR value_ = nullptr;
};

class ComApartment {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ready() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

void set_failure(UwfConfigureResult& result, UwfConfigureOutcome outcome,
                 std::string detail) {
    result.outcome = outcome;
    result.detail = std::move(detail);
}

bool variant_bool(IWbemClassObject* object, const wchar_t* name, bool& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    const bool valid = SUCCEEDED(status) && variant.vt == VT_BOOL;
    if (valid) {
        value = variant.boolVal != VARIANT_FALSE;
    }
    VariantClear(&variant);
    return valid;
}

bool variant_uint32(IWbemClassObject* object, const wchar_t* name,
                    std::uint32_t& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool valid = false;
    if (SUCCEEDED(status) && variant.vt == VT_UI4) {
        value = variant.ulVal;
        valid = true;
    } else if (SUCCEEDED(status) && variant.vt == VT_I4 && variant.lVal >= 0) {
        value = static_cast<std::uint32_t>(variant.lVal);
        valid = true;
    }
    VariantClear(&variant);
    return valid;
}

bool variant_string(IWbemClassObject* object, const wchar_t* name,
                    std::wstring& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    const bool valid = SUCCEEDED(status) && variant.vt == VT_BSTR &&
                       variant.bstrVal != nullptr;
    if (valid) {
        value.assign(variant.bstrVal, SysStringLen(variant.bstrVal));
    }
    VariantClear(&variant);
    return valid;
}

class WmiWriter {
public:
    bool connect() {
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&locator)))) {
            return false;
        }
        Bstr ns(kUwfNamespace);
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

    bool query_one(const wchar_t* query, ComPtr<IWbemClassObject>& object) {
        object.Reset();
        Bstr language(L"WQL");
        Bstr text(query);
        ComPtr<IEnumWbemClassObject> enumerator;
        if (!services_ || language.get() == nullptr || text.get() == nullptr ||
            FAILED(services_->ExecQuery(
                language.get(), text.get(),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                nullptr, &enumerator))) {
            return false;
        }
        ULONG returned = 0;
        return enumerator->Next(3000, 1, object.GetAddressOf(), &returned) ==
                   WBEM_S_NO_ERROR &&
               returned == 1 && object != nullptr;
    }

    bool call(IWbemClassObject* instance, const wchar_t* method,
              std::wstring_view argument_name = {},
              std::wstring_view argument_value = {},
              std::uint32_t* return_value = nullptr,
              ComPtr<IWbemClassObject>* output_parameters = nullptr) {
        if (instance == nullptr || services_ == nullptr) {
            return false;
        }
        std::wstring object_path;
        std::wstring class_name;
        if (!variant_string(instance, L"__PATH", object_path) ||
            !variant_string(instance, L"__CLASS", class_name)) {
            return false;
        }
        Bstr class_path(class_name);
        ComPtr<IWbemClassObject> class_object;
        if (class_path.get() == nullptr ||
            FAILED(services_->GetObject(class_path.get(), 0, nullptr,
                                        &class_object, nullptr))) {
            return false;
        }
        Bstr method_name(method);
        ComPtr<IWbemClassObject> input_definition;
        if (method_name.get() == nullptr ||
            FAILED(class_object->GetMethod(method_name.get(), 0,
                                           &input_definition, nullptr))) {
            return false;
        }
        ComPtr<IWbemClassObject> input;
        if (input_definition &&
            FAILED(input_definition->SpawnInstance(0, &input))) {
            return false;
        }
        if (!argument_name.empty()) {
            if (!input) {
                return false;
            }
            Bstr property(argument_name);
            Bstr value(argument_value);
            VARIANT variant;
            VariantInit(&variant);
            variant.vt = VT_BSTR;
            variant.bstrVal = value.get();
            const HRESULT put = property.get() == nullptr || value.get() == nullptr
                ? E_OUTOFMEMORY
                : input->Put(property.get(), 0, &variant, CIM_STRING);
            // `value` owns the BSTR; do not VariantClear this borrowed value.
            variant.vt = VT_EMPTY;
            variant.bstrVal = nullptr;
            if (FAILED(put)) {
                return false;
            }
        }
        Bstr path(object_path);
        ComPtr<IWbemClassObject> output;
        if (path.get() == nullptr ||
            FAILED(services_->ExecMethod(path.get(), method_name.get(), 0,
                                         nullptr, input.Get(), &output,
                                         nullptr)) ||
            !output) {
            return false;
        }
        std::uint32_t returned = 0;
        if (!variant_uint32(output.Get(), L"ReturnValue", returned)) {
            return false;
        }
        if (return_value != nullptr) {
            *return_value = returned;
        }
        if (output_parameters != nullptr) {
            *output_parameters = output;
        }
        return returned == 0;
    }

    bool contains(IWbemClassObject* instance, const wchar_t* argument_name,
                  std::wstring_view argument_value, bool& found) {
        found = false;
        ComPtr<IWbemClassObject> output;
        if (!call(instance, L"FindExclusion", argument_name, argument_value,
                  nullptr, &output)) {
            return false;
        }
        return variant_bool(output.Get(), L"bFound", found);
    }

private:
    ComPtr<IWbemServices> services_;
};

std::uint32_t product_type() {
    DWORD product = 0;
    return GetProductInfo(10, 0, 0, 0, &product) ? product : 0;
}

bool feature_state(bool& known, bool& enabled) {
    known = false;
    enabled = false;
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&locator)))) {
        return false;
    }
    Bstr ns(L"ROOT\\cimv2");
    ComPtr<IWbemServices> services;
    if (ns.get() == nullptr ||
        FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr,
                                      WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr,
                                      nullptr, &services)) ||
        FAILED(CoSetProxyBlanket(
            services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
            EOAC_NONE))) {
        return false;
    }
    Bstr language(L"WQL");
    Bstr query(L"SELECT InstallState FROM Win32_OptionalFeature WHERE "
               L"Name='Client-UnifiedWriteFilter'");
    ComPtr<IEnumWbemClassObject> enumerator;
    if (language.get() == nullptr || query.get() == nullptr ||
        FAILED(services->ExecQuery(
            language.get(), query.get(),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &enumerator))) {
        return false;
    }
    ComPtr<IWbemClassObject> object;
    ULONG returned = 0;
    const HRESULT status =
        enumerator->Next(3000, 1, object.GetAddressOf(), &returned);
    if (status == WBEM_S_FALSE && returned == 0) {
        known = true;
        return true;
    }
    std::uint32_t install_state = 0;
    if (status != WBEM_S_NO_ERROR || returned != 1 || !object ||
        !variant_uint32(object.Get(), L"InstallState", install_state)) {
        return false;
    }
    known = true;
    enabled = install_state == 1;
    return true;
}

std::optional<std::wstring> volume_exclusion(
    const std::filesystem::path& data_root) {
    if (!data_root.is_absolute() || !data_root.has_root_name() ||
        data_root == data_root.root_path()) {
        return std::nullopt;
    }
    // UWF_Volume is addressed by a local drive letter. Refuse UNC, removable,
    // network, and synthetic roots rather than configuring a different volume
    // than the path will actually use after restart.
    const auto root = data_root.root_path();
    const auto drive = root.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') ||
          (drive[0] >= L'a' && drive[0] <= L'z')) ||
        GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
        return std::nullopt;
    }
    const auto normalized = data_root.lexically_normal();
    const auto relative = normalized.lexically_relative(root);
    if (relative.empty()) {
        return std::nullopt;
    }
    for (const auto& component : relative) {
        if (component == L"..") {
            return std::nullopt;
        }
    }
    std::error_code error;
    if (!std::filesystem::is_directory(normalized, error) || error) {
        return std::nullopt;
    }
    return L"\\" + relative.wstring();
}

bool access_denied(std::uint32_t status) noexcept {
    return status == ERROR_ACCESS_DENIED ||
           status == static_cast<std::uint32_t>(
                         HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) ||
           status == static_cast<std::uint32_t>(WBEM_E_ACCESS_DENIED);
}

UwfConfigureOutcome outcome_for(UwfEnableGate gate) noexcept {
    switch (gate) {
    case UwfEnableGate::ready: return UwfConfigureOutcome::failed;
    case UwfEnableGate::already_enabled: return UwfConfigureOutcome::already_enabled;
    case UwfEnableGate::unsupported_edition: return UwfConfigureOutcome::unsupported_edition;
    case UwfEnableGate::feature_missing: return UwfConfigureOutcome::feature_missing;
    case UwfEnableGate::probe_unavailable: return UwfConfigureOutcome::probe_unavailable;
    case UwfEnableGate::provider_unavailable: return UwfConfigureOutcome::provider_unavailable;
    case UwfEnableGate::reboot_pending: return UwfConfigureOutcome::reboot_pending;
    case UwfEnableGate::invalid_data_root: return UwfConfigureOutcome::invalid_data_root;
    case UwfEnableGate::readiness_failed: return UwfConfigureOutcome::readiness_failed;
    case UwfEnableGate::checkpoint_required: return UwfConfigureOutcome::checkpoint_required;
    }
    return UwfConfigureOutcome::failed;
}

const char* detail_for(UwfEnableGate gate) noexcept {
    switch (gate) {
    case UwfEnableGate::ready: return "UWF is ready to configure";
    case UwfEnableGate::already_enabled: return "UWF is already enabled";
    case UwfEnableGate::unsupported_edition: return "this Windows edition does not support UWF";
    case UwfEnableGate::feature_missing: return "the Unified Write Filter optional feature is not enabled";
    case UwfEnableGate::probe_unavailable: return "Windows edition or feature state could not be verified";
    case UwfEnableGate::provider_unavailable: return "the UWF WMI provider is unavailable";
    case UwfEnableGate::reboot_pending: return "a UWF state change is already waiting for a reboot";
    case UwfEnableGate::invalid_data_root: return "the NSTU data root is not a safe local path";
    case UwfEnableGate::readiness_failed: return "client diagnostics must pass before UWF is enabled";
    case UwfEnableGate::checkpoint_required: return "acknowledge the recovery-checkpoint reminder first";
    }
    return "UWF readiness failed";
}

} // namespace

bool is_uwf_supported_product(std::uint32_t product) noexcept {
    switch (product) {
    case kProductEnterprise:
    case kProductEnterpriseN:
    case kProductEnterpriseS:
    case kProductEnterpriseSN:
    case kProductEducation:
    case kProductEducationN:
    case kProductIotEnterprise:
    case kProductIotEnterpriseS:
        return true;
    default:
        return false;
    }
}

UwfEnableGate evaluate_uwf_enable(
    const UwfEnableReadiness& readiness) noexcept {
    if (readiness.product_type == 0 || !readiness.feature_known) {
        return UwfEnableGate::probe_unavailable;
    }
    if (!is_uwf_supported_product(readiness.product_type)) {
        return UwfEnableGate::unsupported_edition;
    }
    if (!readiness.feature_enabled) {
        return UwfEnableGate::feature_missing;
    }
    if (!readiness.provider_available || !readiness.filter_state_known) {
        return UwfEnableGate::provider_unavailable;
    }
    if (readiness.current_enabled != readiness.next_enabled) {
        return UwfEnableGate::reboot_pending;
    }
    // Even an already-enabled filter may be missing NSTU's mandatory
    // persistence exclusions. Require every safety acknowledgement before the
    // writer is allowed to inspect and repair next-session configuration.
    if (!readiness.data_root_valid) {
        return UwfEnableGate::invalid_data_root;
    }
    if (!readiness.diagnostic_readiness_passed) {
        return UwfEnableGate::readiness_failed;
    }
    if (!readiness.checkpoint_acknowledged) {
        return UwfEnableGate::checkpoint_required;
    }
    return readiness.current_enabled ? UwfEnableGate::already_enabled
                                     : UwfEnableGate::ready;
}

UwfConfigureResult configure_uwf(const UwfConfigureRequest& request) {
    UwfConfigureResult result;
    const auto exclusion = volume_exclusion(request.data_root);
    UwfEnableReadiness readiness;
    readiness.product_type = product_type();
    readiness.diagnostic_readiness_passed =
        request.diagnostic_readiness_passed;
    readiness.checkpoint_acknowledged = request.checkpoint_acknowledged;
    readiness.data_root_valid = exclusion.has_value();

    ComApartment apartment;
    if (!apartment.ready()) {
        set_failure(result, UwfConfigureOutcome::probe_unavailable,
                    "COM could not be initialized for the UWF readiness probe");
        return result;
    }
    (void)feature_state(readiness.feature_known, readiness.feature_enabled);

    WmiWriter writer;
    ComPtr<IWbemClassObject> filter;
    readiness.provider_available = writer.connect();
    if (readiness.provider_available &&
        writer.query_one(L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter",
                         filter)) {
        readiness.filter_state_known =
            variant_bool(filter.Get(), L"CurrentEnabled",
                         readiness.current_enabled) &&
            variant_bool(filter.Get(), L"NextEnabled", readiness.next_enabled);
    }
    const auto gate = evaluate_uwf_enable(readiness);
    const bool filter_already_enabled =
        gate == UwfEnableGate::already_enabled;
    if (gate != UwfEnableGate::ready && !filter_already_enabled) {
        result.outcome = outcome_for(gate);
        result.reboot_required = gate == UwfEnableGate::reboot_pending;
        result.detail = detail_for(gate);
        return result;
    }

    const std::wstring drive = request.data_root.root_name().wstring();
    const std::wstring volume_query =
        L"SELECT * FROM UWF_Volume WHERE CurrentSession=FALSE AND "
        L"DriveLetter='" + drive + L"'";
    ComPtr<IWbemClassObject> volume;
    ComPtr<IWbemClassObject> registry_filter;
    if (!writer.query_one(volume_query.c_str(), volume) ||
        !writer.query_one(L"SELECT * FROM UWF_RegistryFilter WHERE "
                          L"CurrentSession=FALSE",
                          registry_filter)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session UWF volume or registry filter was unavailable");
        return result;
    }

    bool volume_protected = false;
    if (!variant_bool(volume.Get(), L"Protected", volume_protected)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session volume protection state was unreadable");
        return result;
    }
    bool file_exclusion_present = false;
    bool registry_exclusion_present = false;
    if (!writer.contains(volume.Get(), L"FileName", *exclusion,
                         file_exclusion_present) ||
        !writer.contains(registry_filter.Get(), L"RegistryKey",
                         kNstuRegistryExclusion,
                         registry_exclusion_present)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session UWF exclusions could not be verified");
        return result;
    }

    std::uint32_t returned = 0;
    bool volume_protection_added = false;
    bool file_exclusion_added = false;
    bool registry_exclusion_added = false;
    const auto rollback = [&] {
        // Roll back only state this call created. Existing protection and
        // exclusions belong to the operator and are never removed here.
        if (registry_exclusion_added) {
            (void)writer.call(registry_filter.Get(), L"RemoveExclusion",
                              L"RegistryKey", kNstuRegistryExclusion);
        }
        if (file_exclusion_added) {
            (void)writer.call(volume.Get(), L"RemoveExclusion", L"FileName",
                              *exclusion);
        }
        if (volume_protection_added) {
            (void)writer.call(volume.Get(), L"Unprotect");
        }
    };

    // Protect() and all exclusion changes target the next session and require
    // a restart according to Microsoft's UWF provider reference.
    // Sources:
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-volumeprotect
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-volumeaddexclusion
    if (!volume_protected) {
        if (!writer.call(volume.Get(), L"Protect", {}, {}, &returned)) {
            set_failure(result,
                        access_denied(returned)
                            ? UwfConfigureOutcome::access_denied
                            : UwfConfigureOutcome::failed,
                        "the NSTU system volume could not be protected");
            return result;
        }
        volume_protection_added = true;
    }
    if (!file_exclusion_present) {
        if (!writer.call(volume.Get(), L"AddExclusion", L"FileName",
                         *exclusion, &returned)) {
            rollback();
            set_failure(result, UwfConfigureOutcome::failed,
                        "the NSTU data exclusion could not be added");
            return result;
        }
        file_exclusion_added = true;
    }
    if (!registry_exclusion_present) {
        if (!writer.call(registry_filter.Get(), L"AddExclusion",
                         L"RegistryKey", kNstuRegistryExclusion, &returned)) {
            rollback();
            set_failure(result, UwfConfigureOutcome::failed,
                        "the NSTU registry exclusion could not be added");
            return result;
        }
        registry_exclusion_added = true;
    }
    // Enable() arms UWF for next restart and requires elevation.
    // Source: https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-filterenable
    if (!filter_already_enabled &&
        !writer.call(filter.Get(), L"Enable", {}, {}, &returned)) {
        rollback();
        set_failure(result,
                    access_denied(returned)
                        ? UwfConfigureOutcome::access_denied
                        : UwfConfigureOutcome::failed,
                    "UWF could not be armed for the next boot");
        return result;
    }

    const bool changed = volume_protection_added || file_exclusion_added ||
                         registry_exclusion_added || !filter_already_enabled;
    result.outcome = filter_already_enabled
        ? UwfConfigureOutcome::already_enabled
        : UwfConfigureOutcome::armed;
    result.reboot_required = changed;
    result.data_exclusion_added =
        file_exclusion_present || file_exclusion_added;
    result.registry_exclusion_added =
        registry_exclusion_present || registry_exclusion_added;
    result.detail = changed
        ? "UWF configuration is ready; save work and reboot to apply it"
        : "UWF and the mandatory NSTU exclusions are already active";
    return result;
}

} // namespace nstu::setup
