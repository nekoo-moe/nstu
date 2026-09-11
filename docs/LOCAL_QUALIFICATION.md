# Local qualification fixture

This record describes the weakest-machine fixture supplied for NSTU readiness
testing. It is a test target, not a production approval or a supported school
deployment by itself.

| Field | Reported value | Qualification requirement |
|---|---|---|
| CPU | Intel Core i3-7400 | Below the i5-6400 reference; retain as a stress case |
| Memory | 8 GiB | Meets the minimum capacity boundary |
| Windows | Windows 10, reported as “Education/Pro” | Determine the exact SKU and build from the diagnostic report; Education and Pro are separate editions |
| Activation | Unactivated | Record only; activation state does not establish UWF eligibility |
| Network | Tailscale-reachable test host | Measure the physical adapter's negotiated link locally; do not use the Tailscale address as link-capacity evidence |
| Graphics | Not yet verified | Record adapter, driver version, D3D11 probe, and WARP fallback |

## Required run

This fixture is intentionally clean. It has no NSTU binaries, service, project
checkout, or diagnostics directory before bootstrap. Transfer the approved
unified installer to the VM using an operator-controlled channel (for example,
RDP drive redirection or the release page), verify its Authenticode signature,
and install exactly one role. Do not copy only `nstu-diagnostics.exe`: the
installer also stages the runtime files and role checks needed for a valid
qualification run.

The temporary remote-access port (if one is supplied by the lab operator) is
only for the interactive session. It is not NSTU's control port. During client
installation, enter the actual server address and NSTU control port (47001 by
default), or use the server values provided for the lab.

After the installer completes and the required restart has finished, the
diagnostics helper will be at:

```text
C:\Program Files\NSTU\diagnostics\nstu-diagnostics.exe
```

Run the staged `nstu-diagnostics.exe` interactively on the fixture as an
administrator, using a local report path:

```powershell
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" `
  --target=client `
  --report="$env:ProgramData\NSTU\qualification-i3-7400.json" `
  --diagnostics-stay-open
```

The report must capture the exact Windows product name, product SKU/build,
architecture, UWF optional-feature/provider state, Safe Mode state, service and
installation state, CPU/RAM, physical network link, graphics driver, system
time, and configured NSTU server reachability. Do not place passwords, tokens,
Tailscale addresses, screen captures, or enrollment material in the report or
repository.

## Acceptance boundaries

- 8 GiB RAM meets the minimum memory boundary.
- The i3-7400 is intentionally below the i5-6400 reference and must be
  reported as a stress-case CPU result, not silently promoted to “Good”.
- A negotiated link below 100 Mbps fails the minimum network boundary; 100 Mbps
  passes the minimum and 1 Gbps is recommended.
- Windows 10 Education is potentially eligible for UWF after exact-build,
  feature, provider, storage, driver, and recovery qualification.
- Windows 10 Pro is audit-only for UWF; no NSTU control may enable or configure
  reboot-to-restore on Pro.
- An unactivated image must not be “fixed” by NSTU. Activation and licensing
  remain an administrator/school responsibility.

## Current remote-access limitation

The supplied test account can reach the host over the private test network, but
remote WMI and Service Control Manager access return `Access is denied`, and no
SMB share is exposed. The fixture therefore requires an interactive local/RDP
run by an operator with the necessary Windows permissions. NSTU diagnostics do
not weaken remote-UAC, firewall, or account policy to work around this.

## Interactive operator handoff

When the lab operator provides a temporary externally forwarded RDP endpoint,
connect interactively with the host and port supplied out-of-band:

```powershell
mstsc /v:<host>:<port>
```

Do not place the endpoint, credentials, Tailscale/private addresses, or RDP
certificates in this repository. Verify the certificate and host identity with
the lab operator before signing in. After an administrator has signed in on the
fixture, run the diagnostic command above locally and return only the sanitized
JSON report. The report is the qualification evidence; TCP reachability alone
does not establish OS, UWF, graphics, service, or performance readiness.

The current externally forwarded test endpoint has been confirmed reachable on
its supplied TCP port, but no interactive sign-in or machine mutation was
performed by NSTU tooling.

## Report received from the VM

The first sanitized server-role report does not qualify the advertised
weakest-machine fixture:

- Windows reports **Windows 10 Pro Education 22H2**, so Microsoft UWF is
  correctly audit-only for this image.
- The VM exposes **3 GiB RAM** and **3 physical / 4 logical processors**. This
  is below the 8 GiB minimum and is not the previously described 8 GiB test
  configuration.
- The reported **100000 Mbps Tailscale** link is an overlay/tunnel result and
  is not physical link evidence. NSTU now excludes tunnel adapters from the
  physical-link gate; rerun diagnostics with an operational Ethernet or Wi-Fi
  adapter visible to the VM.
- D3D11 hardware and WARP were available. Missing H.264 hardware encoding is a
  warning only because snapshot mode remains the supported baseline.
- Windows Time was not running. Correct this through the lab image policy and
  rerun the check; NSTU does not change system time automatically.

This report is evidence for the lab baseline only; it is not a production
approval. Do not record the VM as an 8 GiB or UWF-capable target until the VM
memory allocation, physical network attachment, and Windows edition are
corrected and re-tested.
