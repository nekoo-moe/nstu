#include "nstu/client_freeze.hpp"

#include <windows.h>

#include <utility>

namespace nstu::client {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

HKEY hive_of(const FreezeLocation& location) noexcept {
    return location.per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

// The service and the installer are both 64-bit, but a 32-bit build reading
// the same value has to land on it rather than in the WOW6432Node mirror,
// where a freeze would silently stop applying.
constexpr REGSAM kRegistryView = KEY_WOW64_64KEY;

} // namespace

bool machine_frozen(const FreezeLocation& location) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive_of(location), location.subkey.c_str(), 0,
                      KEY_QUERY_VALUE | kRegistryView, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD frozen = 0;
    DWORD bytes = sizeof(frozen);
    DWORD type = 0;
    const LONG read =
        RegQueryValueExW(key, location.value.c_str(), nullptr, &type,
                         reinterpret_cast<BYTE*>(&frozen), &bytes);
    RegCloseKey(key);
    return read == ERROR_SUCCESS && type == REG_DWORD &&
           bytes == sizeof(frozen) && frozen != 0;
}

bool set_machine_frozen(bool frozen, const FreezeLocation& location,
                        std::string* error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(hive_of(location), location.subkey.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | kRegistryView,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        set_error(error,
                  "managed mode could not be opened for writing; run this "
                  "as an administrator");
        return false;
    }
    const DWORD value = frozen ? 1u : 0u;
    const LONG written =
        RegSetValueExW(key, location.value.c_str(), 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        set_error(error, "managed mode could not be written");
        return false;
    }
    return true;
}

bool thaw_locally(std::string* error) {
    if (!set_machine_frozen(false, {}, error)) {
        return false;
    }
    // From here on the machine is thawed whatever happens: the flag on disk is
    // the only source of truth, and a service that is stopped, or that this
    // process cannot reach, reads it again at its next start. Telling a
    // running service now only saves the operator a restart, so a failure to
    // do so is not a failure to thaw.
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return true;
    }
    SC_HANDLE service = OpenServiceW(manager, kManagedServiceName,
                                     SERVICE_USER_DEFINED_CONTROL);
    if (service != nullptr) {
        SERVICE_STATUS status{};
        (void)ControlService(service, static_cast<DWORD>(kFreezeReloadControl),
                             &status);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return true;
}

} // namespace nstu::client
