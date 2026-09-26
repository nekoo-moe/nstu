#pragma once

#include <string>

namespace nstu::client {

// Registers the current executable as the automatic LocalSystem NSTU service,
// applies recovery actions and the service DACL, and leaves it stopped. The
// installer restart remains the activation boundary.
[[nodiscard]] bool install_service(std::string* error = nullptr);

// Stops an existing installation for a same-role payload replacement without
// removing its SCM registration or changing its durable Managed-mode state.
[[nodiscard]] bool prepare_service_update(std::string* error = nullptr);

// Refuses while managed mode is active, disables recovery, stops the service,
// waits for it to exit, then removes its SCM registration.
[[nodiscard]] bool uninstall_service(std::string* error = nullptr);

} // namespace nstu::client
