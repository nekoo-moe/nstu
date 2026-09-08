# NSTU Auto-Update and LTSC Maintenance Plan

Status: **planned architecture and trial harness; automatic package delivery is
not enabled in the current MVP.**

This file is the canonical repository source for the GitHub Wiki page
"Auto-Updates and LTSC Maintenance". Keep the wiki copy synchronized with this
file when the Wiki is initialized.

This plan is for schools that perform software maintenance during the long
summer break, commonly after roughly nine months of normal operation. It is
also intended for Windows Long-Term Servicing Channel (LTSC) deployments where
the exact edition and build must be recorded and validated instead of inferred
from the product name alone.

## Goals

- Update more than 50 clients without visiting every classroom machine.
- Keep client and server binaries compatible during a staged rollout.
- Make security patches verifiable, resumable, auditable, and reversible.
- Preserve enrolled identity, protected configuration, and service state.
- Cooperate with Deep Freeze rather than silently losing an update at reboot.
- Require no continuous video channel or inbound firewall exposure for updates.

## Non-goals

- The updater is not a privilege-escalation mechanism. MSI, NSIS, or a future
  updater helper only obtains the rights explicitly granted by UAC and the
  service account configuration.
- The updater must not bypass Deep Freeze, weaken Windows security policy, or
  install an unsigned executable from a user-writable directory.
- The current nightly build does not silently update itself. This document is
  the design and acceptance criteria for a future release.

## Proposed deployment shape

The runtime should remain unchanged where possible:

```text
Signed release manifest and packages
              |
              v
nstu-service (LocalSystem, outbound HTTPS polling)
              |
              +--> protected staging directory
              +--> signature/hash verification
              +--> maintenance-window approval
              +--> reboot-bound installation
              +--> health check and rollback
```

The service, not the interactive agent, owns update state. The server dashboard
shows the state of every enrolled client, but the client must still verify the
release locally before applying it.

Use role-specific artifacts even if a bootstrapper remains available for local
technicians:

- `NSTU-Client-<version>.msi` or an equivalent signed role package.
- `NSTU-Server-<version>.msi` or an equivalent signed role package.
- A signed bootstrapper for diagnostics, role selection, and offline setup.

Do not make a client silently switch to a server package, or vice versa. The
installed role, package role, and service identity must match.

## Release metadata and trust

The update endpoint should expose a small, canonical manifest containing at
least:

- schema version;
- package role (`client` or `server`);
- semantic version and minimum supported version;
- package URL and byte length;
- SHA-256 digest;
- release channel (`ltsc-stable`, `security`, or `pilot`);
- release timestamp and rollout percentage;
- minimum supported Windows build and required reboot flag.

The manifest must be signed by a release key kept outside the build worker.
Every executable/MSI must also carry a valid Authenticode signature with a
trusted timestamp. A client rejects an unsigned manifest, a bad digest, a
role mismatch, a downgrade, or a package outside its supported OS policy.

Use normal certificate validation for HTTPS. Enrollment credentials identify
the client to the update service; MAC address remains a local identity hint,
not the sole authorization factor.

## Client update state machine

1. **Check**: poll metadata over outbound HTTPS and record the result locally.
2. **Defer**: if no approved maintenance window is active, do not replace any
   binary. Critical security releases may be marked urgent by the server, but
   the school policy still decides whether an out-of-cycle thaw is allowed.
3. **Download**: resume an interrupted download into a protected staging path.
4. **Verify**: validate manifest signature, package signature, role, version,
   size, and SHA-256 digest.
5. **Stage**: copy the package and a versioned state record; do not modify the
   running service or agent yet.
6. **Apply at reboot**: stop only NSTU-owned processes, install the verified
   package, preserve the protected data root, and return a reboot-required
   result when Windows needs to restart.
7. **Validate**: after reboot, check the service account, automatic start,
   Session 0 placement, agent session, enrollment, protocol handshake, and
   package version.
8. **Commit or roll back**: retain the previous package until health checks pass;
   restore it automatically when startup or connectivity validation fails.
9. **Report**: send the result, error code, boot identity, and final version to
   the server. Never upload screen content or private keys as update telemetry.

## Deep Freeze and LTSC maintenance window

Deep Freeze is third-party disk-freezing software. A binary update written to
the protected system volume can disappear after a frozen reboot. Therefore a
school must provide a documented maintenance window:

1. Export the current NSTU inventory, enrollment status, and update state.
2. Snapshot or otherwise record the known-good image and package versions.
3. Boot or schedule the machines in **Thawed** mode and disable protection.
4. Confirm the exact Windows LTSC edition/build, GPU driver, network policy,
   and Deep Freeze edition/version.
5. Apply the signed client package in waves, beginning with a pilot group.
6. Reboot each wave and run boot diagnostics and server connectivity checks.
7. Verify enrollment persistence, service recovery, snapshots, chat, and remote
   control before proceeding to the next wave.
8. Refreeze the image only after the acceptance checks and rollback window pass.

If the school cannot provide a thawed maintenance window or a supported Deep
Freeze management interface, NSTU must report the update as deferred. It must
not claim success merely because a package downloaded.

## Nine-month rollout calendar

### T-8 to T-6 weeks: prepare

- Freeze the release candidate and publish the signed manifest.
- Build a Windows LTSC matrix using the exact editions/builds deployed by each
  school.
- Test the package on clean client and server images, including rollback.
- Confirm Deep Freeze maintenance credentials, thaw spaces, and reboot policy.

### T-5 to T-3 weeks: pilot

- Update one server and a small representative client group.
- Include i5-6400/Intel HD 530 hardware and the highest-risk driver versions.
- Measure startup time, CPU/RAM/GPU usage, update duration, and bandwidth.
- Keep the previous package available for immediate rollback.

### T-2 to T-0 weeks: school wave

- Schedule batches that fit the school's switch and reboot capacity.
- Require a successful health report before releasing the next batch.
- Record exceptions rather than repeatedly retrying a machine with an unknown
  Deep Freeze or network state.

### T+1 week: closeout

- Verify every enrolled device reports the expected version and boot identity.
- Compare the final inventory with the pre-maintenance export.
- Refreeze only the validated image and retain logs for the next cycle.

## Server compatibility policy

The server should accept the current protocol and the immediately previous
compatible client protocol during a rolling update. Update the server before
the bulk client wave when the protocol requires it; otherwise keep the server
backward-compatible until all clients report the new version.

The server dashboard should expose update state without exposing signing keys:

| State | Meaning |
| --- | --- |
| `up-to-date` | Installed version matches the approved channel. |
| `available` | A verified package is available but no maintenance window is active. |
| `staged` | Package is verified and waiting for reboot. |
| `reboot-pending` | Installation requested a restart. |
| `healthy` | Post-reboot checks passed. |
| `rolled-back` | Health checks failed and the previous package was restored. |
| `blocked` | Deep Freeze, role mismatch, OS policy, or trust validation blocked the update. |

## Acceptance criteria

An update release is ready for a school only when all of the following are
true:

- packages and manifests have valid signatures and reproducible hashes;
- client and server role conflict checks pass;
- install, upgrade, reboot, repair, and uninstall are tested on the supported
  LTSC images;
- enrolled identity and protected data survive the upgrade;
- service startup is LocalSystem, automatic, and in Session 0;
- the agent starts in the interactive session;
- a failed health check restores the previous package;
- Deep Freeze behavior is validated for the exact school edition/version;
- logs contain no secrets, screen captures, or private keys;
- the pilot and at least one school wave complete without an unexplained
  rollback or persistent reboot loop.

## Trial harness

The repository includes a non-destructive state-machine trial:

```powershell
pwsh -NoProfile -File packaging/test-update-cycle.ps1
```

It creates temporary fake package payloads and verifies digest rejection,
successful staging, health-check rollback, and cleanup. It does **not** stop a
service, reboot Windows, contact an update server, modify Program Files, or
change Deep Freeze. Real LTSC and Deep Freeze validation remains a release gate.

### Trial record: 2026-09-08

The initial repository trial passed under PowerShell 7 and Windows PowerShell
5.1. It rejected a modified payload, accepted a matching SHA-256 payload,
activated a healthy staged version, rolled back an unhealthy version, and
preserved the last healthy version. This is state-machine evidence only; it is
not evidence for a real service replacement, reboot, LTSC image, or Deep Freeze
maintenance cycle.

## Current status

The current MVP has the signed-package and reboot-safe installer foundations,
but does not yet ship a background auto-update client or a server-side release
manifest service. The next implementation phase should add the update state
machine behind an administrator-controlled feature flag, then run the trial
harness and a disposable VM test before enabling it for any school.
