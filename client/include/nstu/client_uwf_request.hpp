#pragma once

#include <string>

namespace nstu::client {

struct UwfRequestLocation {
    bool per_user = false;
    std::wstring subkey = L"Software\\NSTU";
    std::wstring value = L"UwfConfigureRequested";
};

// Installer writes one only after operator checks checkpoint acknowledgement.
// Missing, malformed, or unreadable state means no request.
[[nodiscard]] bool uwf_configuration_requested(
    const UwfRequestLocation& location = {});
[[nodiscard]] bool set_uwf_configuration_requested(
    bool requested, const UwfRequestLocation& location = {},
    std::string* error = nullptr);

} // namespace nstu::client
