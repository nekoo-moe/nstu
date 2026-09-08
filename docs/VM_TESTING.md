# Virtual-machine lifecycle testing

Use a disposable Windows guest for service, tamper-resistance, and uninstall
tests. Never run the destructive lifecycle harness on a teacher or student
workstation. The harness deletes an NSTU test installation, creates a temporary
local user, and changes service configuration.

## Privilege boundary

NSTU deliberately uses two Windows security contexts:

| Process | Required context | Reason |
|---|---|---|
| `nstu-service.exe` | `LocalSystem`, Session 0 | Owns privileged lifecycle, networking, protected configuration, and agent supervision. |
| `nstu-agent.exe` | Logged-in classroom user, interactive session | Owns tray, chat, capture, overlay, and input UI on that user's desktop. |
| `nstu-server.exe` | Teacher's interactive account | Displays the administration UI; elevation is not a normal runtime requirement. |
| Installer/uninstaller | Elevated administrator | Creates/deletes the service and protected files. |

Running the interactive agent as `LocalSystem` does not make the design safer.
Windows isolates services in Session 0, while screen and input operations must
run in the target interactive session. It would also expose a much larger UI and
media attack surface with SYSTEM authority.

The client installer registers only `nstu-service.exe` as `LocalSystem`. Its
service DACL gives SYSTEM and Administrators service-control rights and gives
authenticated users query access only. After an established agent pipe
disconnects, the SYSTEM service attempts a bounded replacement launch in the
active session. This is recovery, not a promise that a user-mode process is
unkillable. A local
administrator or kernel-level product always retains control of the machine.
Use standard classroom accounts, protected Program Files ACLs, service policy,
and, where appropriate, WDAC or AppLocker for the real tamper boundary.

## Disposable Windows Sandbox test

Windows Sandbox is suitable for installation, service-account, agent-recovery,
service-DACL, and pre-reboot uninstall checks. It is not suitable for validating
next-boot deletion because closing or restarting Sandbox destroys the guest.

Prerequisites:

- Windows 10/11 Pro, Enterprise, or Education with virtualization enabled.
- The Windows Sandbox optional feature enabled by an administrator.
- A completed Windows build containing `client/nstu-service.exe` and
  `client/nstu-agent.exe`.
- No secrets or production enrollment material in the mapped checkout.

From an ordinary host PowerShell prompt at the repository root, launch:

```powershell
.\tools\virtualization\Start-NstuSandboxTest.ps1 `
  -BuildDirectory .\build-verify-werror
```

The launcher maps the repository read-only, maps a dedicated result directory
read/write, and starts the test automatically inside Sandbox. The guest copies
all executable/script inputs to its local disk before installation. By default,
results are written under
`%TEMP%\nstu-sandbox-results\run-<UTC timestamp>` on the host. Pass
`-OutputRoot` to select another directory outside the repository.

The test is intentionally guarded by both Windows Sandbox detection and a
launcher-only marker. The guest script refuses to execute when invoked directly
on a physical host.

Expected assertions:

1. The service is registered with `StartName=LocalSystem`, automatic startup,
   configured recovery actions, and the restrictive service DACL. The report
   also records the running service process owner when WMI permits the query.
2. The service runs in Session 0 and launches the agent in the active desktop
   session; an agent in Session 0 or a mismatched active-console session fails
   the lifecycle test.
3. Terminating the connected agent causes a different agent PID to appear
   within the bounded watchdog timeout.
4. A temporary standard user cannot stop or delete `nstu-service`.
5. A direct pre-reboot uninstaller call is rejected and leaves the service,
   processes, package files, and `PendingFileRenameOperations` unchanged.
6. The report records OS build, account names, process paths, service account,
   session IDs, exit codes, and pending-delete state.

The production installer deliberately waits for a Windows restart before first
normal client use. The Sandbox harness starts the service manually after
installation solely to exercise the pre-reboot lifecycle. It cannot execute
the SYSTEM startup task that finalizes removal after a persistent reboot.

## Persistent reboot test

Use a persistent Generation 2 Hyper-V guest for the release-blocking reboot
test. A practical baseline is 4 virtual CPUs, 6-8 GiB RAM, a dynamically
expanding 64 GiB VHDX, Secure Boot, and an isolated NAT/internal virtual switch.
Create a clean checkpoint before installing NSTU. Use two guest accounts:

- a dedicated administrator for setup and recovery;
- a standard classroom user for normal sign-in and tamper attempts.

Run the following sequence and retain the transcript/screenshots:

1. Install while the guest is Thawed or does not yet contain Deep Freeze.
2. Confirm that the installer requests restart and that the service is not
   started inside the installer transaction.
3. Restart and confirm automatic `LocalSystem` service startup plus agent launch
   in the standard user's session.
4. Exercise Task Manager `End task` against the agent and verify recovery.
5. Confirm that the standard user cannot stop/delete the service or modify the
   installed binaries and protected data root.
6. Hold an installed package file open, run the elevated uninstaller, and
   capture `PendingFileRenameOperations`.
7. Restart, then confirm that the service returns error 1060 from `sc.exe query`,
   no NSTU processes remain, and all scheduled package files are gone.
8. Revert to the clean checkpoint before the next run.

Windows Sandbox cannot replace steps 3 and 7. On the current development host,
Hyper-V management cmdlets and a Windows ISO/VHD are not available to this
session, so the persistent guest must be created after those host prerequisites
are supplied by an administrator.

## Deep Freeze validation

Deep Freeze is third-party disk-freezing software. Test each supported edition
and version in a persistent guest or sacrificial lab machine supplied by its
vendor-supported virtualization path:

1. Install and enroll NSTU while Thawed.
2. Freeze and restart; verify service/agent startup and configuration survival.
3. Attempt uninstall while Frozen; NSTU must fail closed when a recognized
   active Deep Freeze service is present.
4. Disable Deep Freeze, restart Thawed, uninstall NSTU, and complete the second
   restart required for locked-file deletion.

Do not treat service-name detection alone as final Deep Freeze compatibility
evidence. Record the product edition, version, policy state, and raw lifecycle
results in `docs/PRODUCTION_VALIDATION.md`.
