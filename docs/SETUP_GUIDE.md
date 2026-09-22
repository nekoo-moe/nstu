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
`BuildChannel=InternalVmTest`, and permits RAM capacity warnings so an
intentionally undersized development VM can exercise installation, UWF
diagnostics, and reboot behavior. It does not change the production installer;
the production gate remains 6 GiB RAM and a 100 Mbps physical link. CPU model,
architecture, and core counts are recorded for information but do not block
installation. Never deploy the internal artifact to a school machine.

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
server address for subsequent logon diagnostics, and performs a second,
bounded TCP connection check against the installed client layout. That check
must pass before the installer reaches its mandatory reboot. The service
remains stopped until that restart activates the client. The address entered
here is not an enrollment credential and is not yet the service's authenticated
runtime configuration.

Client installation is a thawed-mode operation. NSTU does not enable, disable,
or replace Microsoft UWF and does not attempt to control third-party Deep
Freeze. Install while the machine is thawed, then allow the installer to
restart Windows. This preserves compatibility with existing freeze software;
the client boot check only observes the resulting state.

### Stable addressing and automatic recovery

Give the teacher server a DHCP reservation or static address outside the DHCP
pool. A practical layout keeps the router/gateway at `.1` and reserves an
address such as `.10` for NSTU; do not assign the server the gateway address.
This remains the simplest and most diagnosable production configuration.

After authenticated provisioning, `nstu-service` treats the stored server IP
as a cache. If TCP connection or mutual authentication fails, the client sends
an HMAC-authenticated UDP discovery request on the configured control port,
verifies the response with its enrollment PSK, completes the normal mutual TCP
handshake, and only then stores the new IPv4 address using machine-scope DPAPI.
It also refreshes the non-secret registry address used by login diagnostics.
No server MAC address is trusted or used as an authentication factor.

Allow inbound **TCP and UDP `47001`** to the NSTU server executable from the
managed classroom VLAN. TCP carries control and snapshots; UDP `47001` is only
the bounded endpoint-recovery exchange. UDP `47000` remains reserved for the
deferred continuous-video path and should stay closed when that feature is not
being tested.

IPv4 broadcast discovery does not cross a router. Two labs in one VLAN can find
the same enrolled server; separate VLANs must use a stable reserved address or
managed DNS/manual configuration until an authenticated relay is implemented.
If the router, switch, DHCP service, or server is unavailable, clients retain
their enrollment and retry with jitter, but classroom control remains offline.
Transient lock, exam, broadcast, annotation, and remote-control state is cleared
on disconnect rather than left active indefinitely.

The installer still needs a currently reachable address for its initial TCP
preflight, and discovery is unavailable until one-time authenticated enrollment
has installed the client PSK.

For a server, diagnostics check the display, network link, and hardware H.264
encoder before the server files and protected data root are installed. UWF is
reported as not applicable for the server role because server data is
persistent. Run UWF qualification with `--target=client` on a separate client
image.

The server installer also creates the machine startup value `NSTU Server`
under `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`. The desktop server
therefore starts in the interactive teacher session at each Windows sign-in;
it is not installed as a Session 0 service. Closing or minimizing the main
window leaves the tray process running. **Exit** stops it until a manual launch
or the next sign-in. The unified uninstaller removes this startup value.

## Integrated diagnostics

`diagnostics\nstu-diagnostics.exe` is used by the installer and can be run by a
technician. It displays checks sequentially. Without `--auto-close`, the window
stays open for review. With `--auto-close`, only a completely clean run closes
automatically for a technician run; warnings and failures remain visible, and
failures return a non-zero exit code. The installer passes `--installer
--auto-close`; an issue run remains visible for six seconds, then closes so the
synchronous NSIS preflight cannot hang. Its report is written before exit and
the failure exit code is preserved, allowing NSIS to abort safely. The clean
installer path closes after a short delay. Add `--diagnostics-stay-open` when a
technician needs to keep a clean run visible; do not combine it with a
synchronous installer preflight.
The installer retains the report at `%TEMP%\NSTU-installer-preflight.json` when
it aborts. Use `--report=<path>` to retain a structured JSON result list;
`--log=<path>` is retained as a compatibility alias.
UWF checks are read-only and never enable the filter, change registry/service
state, or reboot the machine. Unsupported editions and unavailable providers
are reported as warnings so a Pro/Home installation remains audit-only.
When a supported edition is detected but the optional-feature query itself
cannot be completed, the result is reported as `probe unavailable` rather than
`feature missing`; retry the diagnostic with the required local permissions and
do not enable UWF based on an indeterminate result.

On a supported client image, the read-only probe also reports current and next
protected-volume state, exclusion counts without storing path names, overlay
type/maximum size/consumption/thresholds, and UWF event health for the previous
seven days. WMI work is timeout-bounded: exclusion reads share a two-second
budget, and each event query has a two-second budget and a 256-event cap.
Partial reads or a reached cap are reported as warnings rather than treated as
approval. These diagnostics do not implement any UWF mutation.

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

## Installer payload and operator scripts

The unified installer ships only the role binaries (`nstu-service`, `nstu-agent`,
`nstu-provision` for the client; `nstu-server` for the server), their MinGW
runtimes, the standalone diagnostics helper, and the exam runtime assets
(`exam/web`, `exam/schema`, `exam/examples`). It does **not** package any
PowerShell scripts, documentation, or the repository-only `docs\assets\`
images. Standalone EXEs do not register services and are not a supported
installation source.

The operator helper scripts live in the repository's `packaging\` directory and
are run from a source checkout of the matching release, not from the installed
product. The client-role helper configures the service, data root, recovery
policy, and protected ACLs. The server-role helper validates the role and
protected data root.

`packaging\stage-exam-package.ps1` stages an exam package on a client. Run it
from an elevated PowerShell session in a source checkout after copying the
approved `.nstuexam` archive and release metadata. Supply both the archive
SHA-256 and the unpacked content SHA-256; production packages must include the
detached `manifest.p7s` publisher signature and its approved certificate
thumbprint. Pass an explicit absolute `-PublishRoot` under the configured
persistent client data root (the default is `%ProgramData%\NSTU\exams\packages`);
the helper does not discover a root or download/copy an archive from the server.
`-RequireAuthenticode` is an optional additional policy gate for `.exe` and
`.dll` files in a package; it is separate from the required detached manifest
signature. Use the helper only for client staging, outside the server data
root. It does not enable UWF or change Deep Freeze. Packages must contain
`exam/web/index.html`; see the [exam package format](EXAM_ASSESSMENT.md).
`-AllowUnsigned` and `-AllowNonElevatedTest` are limited to
disposable developer tests.

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

After installing the server, create a one-time enrollment secret by running
`packaging\new-enrollment-secret.ps1` from a source checkout. Provision each
client with the installed `client\nstu-provision.exe`. Provisioning writes the
authenticated DPAPI-protected runtime configuration used by `nstu-service`; the
address entered in the installer is retained for diagnostics only until this
exchange succeeds.

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
