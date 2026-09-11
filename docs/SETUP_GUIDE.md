# NSTU Setup Guide

[English README](../README.md) | [Tiếng Việt](../README.vi.md)

## Unified installer

Release builds provide one role-selecting package: `nstu-<version>-setup.exe`.
The first page offers **Install for Client** or **Install for Server** and never
installs both roles into the same directory. The installer checks for an
existing opposite role before copying files.

### Internal VM test build

Developer CI also publishes a separate `nstu-<version>-internal-vm-setup.exe`
artifact. It is visibly labeled **Internal VM Test**, records
`BuildChannel=InternalVmTest`, and permits CPU/RAM capacity warnings so an
intentionally undersized development VM can exercise installation, UWF
diagnostics, and reboot behavior. It does not change the production installer;
the production gate remains 6 GiB RAM, four physical/logical processors, and a
100 Mbps physical link. Never deploy the internal artifact to a school machine.

### Clean-machine bootstrap

On a clean qualification machine, `nstu-diagnostics.exe` does not exist until
the unified installer has been transferred and run. Transfer the approved
`nstu-<version>-setup.exe` through an operator-controlled channel, verify its
signature, and select exactly one role. The installer copies the diagnostics
helper to `C:\Program Files\NSTU\diagnostics\` and, for a client install,
requires the documented restart before the service is active.

Any temporary RDP or port-forwarding endpoint used to reach the machine is
separate from NSTU's client/server control port. Enter the actual NSTU server
address and control port (47001 by default) in the client role page; do not use
the temporary remote-access port as the NSTU control port.

For a client, enter the server IP address and control port (`47001` by default).
The installer runs the diagnostics helper before service registration. It then
registers `nstu-service` as an automatic `LocalSystem` service, stores the
server address for subsequent logon diagnostics, and sets a mandatory reboot
flag. The service remains stopped until that restart activates the client. The
address entered here is not an enrollment credential and is not yet the
service's authenticated runtime configuration.

For a server, diagnostics check the display, network link, and hardware H.264
encoder before the server files and protected data root are installed.

## Integrated diagnostics

`diagnostics\nstu-diagnostics.exe` is used by the installer and can be run by a
technician. It displays checks sequentially. Without `--auto-close`, the window
stays open for review. With `--auto-close`, only a completely clean run closes
automatically for a technician run; warnings and failures remain visible, and
failures return a non-zero exit code. The installer passes `--installer
--auto-close`, which closes the diagnostic window after the checks even when a
warning or failure is present so the synchronous NSIS preflight cannot hang.
The installer retains the report at `%TEMP%\NSTU-installer-preflight.json` when
it aborts. Use `--report=<path>` to retain a structured JSON result list;
`--log=<path>` is retained as a compatibility alias. Add
`--diagnostics-stay-open` when a technician needs to keep a clean run visible.
UWF checks are read-only and never enable the filter, change registry/service
state, or reboot the machine. Unsupported editions and unavailable providers
are reported as warnings so a Pro/Home installation remains audit-only.

```powershell
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" --target=client --server-ip=192.168.10.10 --server-port=47001 --installer
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" --target=client --boot-check --auto-close --log="$env:ProgramData\NSTU\boot-check.log"
```

The installer registers this command in the machine `Run` key, so the client
health window starts when a user signs in after boot. It verifies that the
service is running as `LocalSystem` in Session 0, that the installed agent is
running in the signed-in session, that an operational network adapter exists,
and that the configured server's TCP port is reachable. It is not a pre-logon
Session 0 check. The agent intentionally does not run as SYSTEM: Windows
Session 0 isolation means screen, tray, overlay, and input operations belong in
the interactive user session.

The diagnostics helper does not enroll a client or derive a protocol key from an
IP address. Enrollment remains the authenticated one-time operation documented
below.

## Installer and scripts

The complete installer includes lifecycle scripts under `client\` and
`docs\deployment\`. Standalone EXEs do not register services and are not a
supported installation source.

Both roles are checked before installation to prevent conflicts. The client
helper configures the service, data root, recovery policy, and protected ACLs.
The server helper validates the role and protected data root.

## Uninstall requires a restart

The unified uninstaller deliberately performs no removal on the first invocation.
It checks administrator access and Deep Freeze state, records a removal plan,
creates a one-shot SYSTEM startup task, and requires a Windows restart. If the
user declines or skips the restart, no service, process, or package file is
changed. After reboot, the startup task verifies that uptime changed, then the
role-specific uninstaller stops owned processes, removes the service, and
schedules any locked files for next-boot deletion.

Deep Freeze is third-party disk-freezing software. Uninstallation is blocked
while its recognized protection services are active; boot Thawed and disable
protection before staging removal.

## Enrollment

After installing the server, create a one-time enrollment secret with
`docs\deployment\new-enrollment-secret.ps1`. Provision each client with the
packaged `client\nstu-provision.exe`. Provisioning writes the authenticated
DPAPI-protected runtime configuration used by `nstu-service`; the address
entered in the installer is retained for diagnostics only until this exchange
succeeds.

## Build

The diagnostics target is the standalone check helper used by the unified
installer:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DNSTU_ENABLE_PACKAGING=ON
cmake --build build --target nstu-diagnostics
cmake --build build --target nstu-package
```

`nstu-package` requires NSIS `makensis.exe`. The generated package is the release
installer. Use [VM lifecycle testing](VM_TESTING.md) for the destructive service,
End Task, uninstall, and persistent reboot validation.

The planned nine-month update process is documented in
[Auto-update and LTSC maintenance](AUTO_UPDATE_LTSC.md). The current MVP does
not silently replace installed binaries; run the non-destructive trial before
implementing or enabling a future updater:

```powershell
pwsh -NoProfile -File packaging/test-update-cycle.ps1
```
