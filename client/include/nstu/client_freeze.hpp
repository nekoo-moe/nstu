#pragma once

#include <cstdint>
#include <string>

// Managed ("frozen") mode. A frozen machine keeps the NSTU service running:
// the service refuses a stop from the service control manager and the
// uninstaller declines to run, so the computer a lesson depends on cannot be
// quietly disarmed part-way through it.
//
// This is a management feature, not anti-tamper, and the difference matters.
// It is disclosed, it is visible in Services and in the agent, and it is
// undone either from the server or with `nstu-service.exe --thaw-local` by a
// local administrator. Nothing here hides a process, resists a debugger, or
// touches the kernel. A standard user is already blocked by the service DACL;
// what freezing adds is that stopping or removing NSTU becomes a deliberate
// administrative act rather than a click.
namespace nstu::client {

inline constexpr const wchar_t* kManagedServiceName = L"nstu-service";

// A custom service control that tells a running service to re-read the flag,
// so a local thaw takes effect without a restart. 128 and 129 are already the
// lock and unlock controls.
inline constexpr std::uint32_t kFreezeReloadControl = 130;

// Where the flag is written. It is a plain registry value rather than anything
// sealed, and that is deliberate: an administrator is allowed to thaw this
// machine, so hiding the flag would only keep it from the uninstaller and the
// diagnostics that legitimately read it, while doing nothing at all against
// the person it exists to stop - a standard user, who cannot write under HKLM
// in the first place.
struct FreezeLocation {
    // The per-user hive exists so a round trip can be proved by a test that
    // was not launched elevated. The service never uses it.
    bool per_user = false;
    std::wstring subkey = L"Software\\NSTU";
    std::wstring value = L"Frozen";
};

// Absent, unreadable and zero all mean the same thing: this machine is not
// managed. Failing open is the right way round - a service that refused to
// stop because it could not read a flag would be a machine nobody can service.
[[nodiscard]] bool machine_frozen(const FreezeLocation& location = {});

[[nodiscard]] bool set_machine_frozen(bool frozen,
                                      const FreezeLocation& location = {},
                                      std::string* error = nullptr);

// Clears the flag on this machine and, if the service is running, tells it to
// re-read it so the service control manager stops being told that stop is
// refused. Requires elevation, because writing under HKLM does.
[[nodiscard]] bool thaw_locally(std::string* error = nullptr);

} // namespace nstu::client
