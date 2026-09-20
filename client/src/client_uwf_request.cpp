#include "nstu/client_uwf_request.hpp"

#include <windows.h>

namespace nstu::client {
namespace {

HKEY hive_of(const UwfRequestLocation& location) noexcept {
    return location.per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

} // namespace

bool uwf_configuration_requested(const UwfRequestLocation& location) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive_of(location), location.subkey.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(
        key, location.value.c_str(), nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD &&
           bytes == sizeof(value) && value == 1;
}

bool set_uwf_configuration_requested(bool requested,
                                     const UwfRequestLocation& location,
                                     std::string* error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(hive_of(location), location.subkey.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        set_error(error, "UWF installer request could not be opened");
        return false;
    }
    const DWORD value = requested ? 1u : 0u;
    const LONG written = RegSetValueExW(
        key, location.value.c_str(), 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        set_error(error, "UWF installer request could not be written");
        return false;
    }
    return true;
}

} // namespace nstu::client
