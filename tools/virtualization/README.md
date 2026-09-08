# NSTU Windows Sandbox lifecycle harness

These scripts exercise the client installer/service lifecycle inside a
disposable Windows Sandbox. They are deliberately destructive inside the
guest: the test creates a local user, registers `nstu-service`, terminates the
agent, and runs the client uninstaller. Do not point the harness at a
production installation or production secrets.

## Quick start

Run the wrapper from a host PowerShell prompt at the repository root:

```powershell
.\tools\virtualization\Start-NstuSandboxTest.ps1 `
  -BuildDirectory .\build-verify-werror
```

When a matching `nstu-*-setup.exe` is present below the build directory, the
wrapper can select that unified installer. The current destructive guest run is
unattended and does not answer the installer's role/server pages, so use
`-PackageRoot` for the supported client lifecycle assertions. Package mode
stages the build's `client\` directory and runs the packaged lifecycle script.
For an explicit package input:

```powershell
.\tools\virtualization\Start-NstuSandboxTest.ps1 `
  -PackageRoot .\build-verify-werror\client `
  -RuntimeRoot C:\msys64\ucrt64\bin
```

The host launcher writes a generated `.wsb`, `host-manifest.json`, and guest
results under `%TEMP%\nstu-sandbox-results\run-<UTC timestamp>`. Pass
`-OutputRoot` to choose another directory outside the repository. `-NoLaunch`
generates the configuration and manifest without starting Sandbox.

## Isolation and guardrails

- The checkout and artifact mappings are read-only in the guest. Only the
  dedicated result directory is writable.
- Networking and vGPU are disabled in the generated configuration.
- The guest script requires the exact launcher marker
  `NSTU-WINDOWS-SANDBOX-CLIENT-LIFECYCLE-v1`, the default
  `WDAGUtilityAccount`, virtual hardware identity, and an administrator token.
  It refuses to run on a physical host or in a normal interactive shell.
- MinGW builds need `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, and
  `libwinpthread-1.dll`. The host stages only those files from
  `C:\msys64\ucrt64\bin` (or `-RuntimeRoot`); it never maps the MSYS tree.
  Package mode fails before launch when a required DLL is unavailable.
- The service account is checked for `LocalSystem`, Session 0, automatic
  startup, recovery restart actions, and the expected service DACL. The
  running service process owner is also checked through `Win32_Process.GetOwner`
  when the guest permits that query; an unavailable owner query is reported as
  a skip, not mistaken for proof of a different account. The agent is launched
  in the active interactive session by the service, and both the initial and
  replacement agent are rejected if they run in Session 0 or a different
  active-console session.
- A temporary standard user attempts to stop/delete the service and terminate
  NSTU-owned processes. Successful privileged operations are recorded as
  failures; inability to create the account is recorded as a skip.
- An exclusive file handle is held while the uninstaller runs. The report
  captures `PendingFileRenameOperations`, service removal, and residual files.

## Reboot limitation

The harness never reboots the guest. Windows Sandbox is destroyed when it is
closed, so it cannot prove that a `MoveFileEx(...,
MOVEFILE_DELAY_UNTIL_REBOOT)` entry is processed on the next boot or that the
client service starts after the installer's required restart. Those assertions
must be run in a persistent, isolated Hyper-V/VMware/VirtualBox guest using the
sequence in `docs/VM_TESTING.md`.

The client GUI/agent must remain in the interactive user session. Running it
as `LocalSystem` would break Session 0 isolation and is not a valid way to
prevent a local administrator from ending a process. The harness therefore
tests the privileged service boundary and standard-user service ACL instead of
claiming that any user-mode process is unkillable.
