# NSTU Reboot-to-Restore Design

Status: **architecture and safety plan only. NSTU does not currently provide
reboot-to-restore and cannot yet replace Deep Freeze.** No write-filter,
volume-protection, or UWF mutation code is enabled in the current installer or
runtime.

This document is the canonical repository source for a future GitHub Wiki page.
It defines a conservative implementation plan for centrally managed lab PCs.
The snapshot-monitoring path is independent of this work and remains the
production priority; continuous per-client H.264 monitoring is not required by
this design.

## Decision

NSTU should manage Microsoft's supported **Unified Write Filter (UWF)** rather
than implement a new file-system minifilter, storage filter, boot driver, or
copy-on-write disk format.

UWF intercepts non-excluded writes to a protected volume and redirects them to
a virtual overlay. Writes to exclusions and unprotected volumes remain
persistent, and exclusions do not reduce overlay consumption. With a
non-persistent overlay, restarting Windows clears the intercepted writes and
returns the protected volume to its previously committed state. NSTU's
responsibility would be policy, diagnostics, authenticated orchestration,
monitoring, and recovery guidance. Windows remains responsible for the
kernel-mode filtering.

This choice materially reduces kernel attack surface and avoids making NSTU
responsible for crash consistency across NTFS, BitLocker, paging, hibernation,
antivirus, boot repair, and Windows servicing. It does not remove the need for
destructive VM tests, physical-machine tests, recovery images, and independent
security review.

NSTU must not:

- install an NSTU-authored storage or file-system filter driver;
- use undocumented registry changes to expose UWF on an unsupported edition;
- treat VHD differencing, System Restore, mandatory profiles, or file cleanup
  scripts as equivalent to whole-volume reboot-to-restore;
- enable protection automatically during ordinary NSTU installation;
- claim that a successful command means protection is active before a reboot
  and post-boot verification.

## Windows edition gate

Microsoft's current UWF requirements list the following editions:

| Windows edition | Microsoft UWF support | NSTU plan |
| --- | --- | --- |
| Enterprise, including an applicable Enterprise LTSC build | Yes | Eligible only after build, feature, storage, driver, and recovery validation |
| Education | Yes | Eligible only after the same validation |
| IoT Enterprise, including an applicable IoT Enterprise LTSC build | Yes | Eligible only after the same validation |
| Pro | No | Audit-only; no built-in reboot-to-restore controls |
| Home | No | Unsupported |
| Windows Server | Not listed in the client UWF support matrix | Unsupported unless Microsoft publishes an applicable supported path |

The product name or the letters "LTSC" are not sufficient evidence. The client
must identify the actual SKU and build, query the `Client-UnifiedWriteFilter`
optional-feature state, and confirm that the UWF WMI provider is available.
NSTU support may be narrower than Microsoft's matrix until each exact image has
passed the project test gates.

### Initial qualification baseline: Windows 10 Education

The first supported lab qualification target is a clean, fully patched
Windows 10 Education x64 image on the project's weakest reference hardware:
Intel Core i5-6400, 8 GiB RAM, Intel HD 530, and a negotiated 100 Mbps link.
The exact Windows build, servicing stack, graphics driver, storage layout, UWF
optional-feature state, and firmware configuration must be recorded in the test
report. "Windows 10 Education" is a test target, not proof that every Windows
10 Education build is qualified. Windows 11 Education and Enterprise images
remain separate qualification targets.

The first trial is capability-only. It must prove that diagnostics can read the
SKU/build and UWF provider state without changing the filter. Only after the
baseline image passes persistent reboot, recovery-image, overlay, servicing,
and rollback tests may a lab-only UWF controller be evaluated.

Windows licensing remains the school's responsibility. NSTU's MIT license does
not grant an Enterprise, Education, or IoT Enterprise Windows license.

### Windows Pro fallback

There is no safe NSTU switch that makes UWF supported on Windows Pro. The
fallback order is:

1. Move the lab to a properly licensed supported Windows edition, then run the
   complete UWF qualification process.
2. Retain a separately licensed third-party reboot-to-restore product and let
   NSTU provide read-only health/inventory integration only through a documented
   vendor API, if one exists.
3. Use centrally managed reimaging between terms or incidents. This has a much
   longer recovery time and is not equivalent to discard-on-reboot protection.

NSTU must not ship a home-grown Pro-only clone based on scripts, snapshots,
shadow copies, or copying a baseline over the live system volume. Such a clone
would have different atomicity, boot-recovery, servicing, and security
properties and would reintroduce the riskiest part of the project.

## Scope and threat model

The intended goal is to discard routine student-session changes at reboot while
allowing authorized technicians to enter a controlled maintenance cycle.

The design can address:

- accidental operating-system and application configuration drift;
- files and applications written by standard student accounts to a protected
  volume;
- repeated reuse of a known-good classroom image;
- fleet visibility for current/next-session filter state and overlay health.

It does not, by itself, address:

- compromise of a local Administrator, `SYSTEM`, the kernel, firmware, or the
  NSTU server's deployment credentials;
- data theft, screen capture, or malicious activity performed before reboot;
- writes to excluded paths, unprotected volumes, external media, firmware, or
  network storage;
- physical boot from removable media, disk removal, or offline attacks;
- pre-existing corruption in the committed image;
- backup, disaster recovery, legal retention, or preservation of student work.

Standard student users are the primary adversary. A local administrator can
change UWF next-session configuration, and kernel-level software can defeat the
boundary entirely. Schools still need Secure Boot, firmware/boot-order controls,
BitLocker where appropriate, protected recovery keys, unique administrator
credentials, firewall policy, and a known-good recovery image.

## Privilege and authorization boundaries

UWF configuration is privileged. It belongs in the existing LocalSystem
`nstu-service` process, behind a small typed module; it must never run in
`nstu-agent.exe` or the interactive teacher UI.

Proposed service-side components:

| Component | Responsibility |
| --- | --- |
| `RestoreCapabilityProbe` | Read SKU/build, optional-feature state, current/next UWF state, protected volumes, exclusions, overlay configuration, and UWF event health |
| `RestorePolicyValidator` | Validate an immutable, versioned policy against local safety rules |
| `UwfController` | Call the documented UWF WMI provider through a fixed operation allowlist |
| `OverlayMonitor` | Read consumption and warning/critical events without changing configuration |
| `MaintenanceCoordinator` | Persist a bounded reboot/servicing transaction and run post-boot checks |
| `RestoreAudit` | Record who requested an operation, what state changed, and the verified result, without secrets or screen data |

The implementation should use the documented UWF WMI provider as the primary
API. `uwfmgr.exe get-config` may be useful to technicians for independent
diagnosis, but the service must not build a command line from network input or
expose a generic process-execution endpoint.

Existing teacher control authentication is not sufficient authorization for
storage policy. Before remote mutation is enabled, NSTU needs a separate
deployment-administrator role and credential. Each mutation request must be a
strictly typed operation carrying a unique intent ID, target device, expected
current state, desired policy revision, expiry time, maintenance window, and
replay protection. The client revalidates every precondition locally.

At minimum, these operations require deployment-administrator authorization:

- install or remove the optional UWF feature;
- protect or unprotect a volume;
- enable or disable the filter;
- change exclusions, overlay type/size, or persistence;
- enter or leave servicing mode;
- reboot, decommission, or transfer management of a protected client.

The first activation on each hardware/image combination and all recovery or
decommission operations require local technician confirmation. Normal teachers
may view health and request a classroom reboot under school policy, but they do
not receive UWF policy-editing rights.

## Persistent data boundary

Reboot-to-restore is useful only if the persistence boundary is explicit.
NSTU should prefer a dedicated unprotected **control-state** volume with an
ACL granting write access only to LocalSystem and administrators. This volume
is for NSTU identity, policy, transaction, audit, and health state; it is not
the location for student work. Student work needs a separate school-approved
user-data volume or network/cloud path with the intended student ACLs. If the
school has only one volume, use the smallest reviewed UWF file and registry
exclusions possible.

Permitted persistent NSTU data is limited to:

- DPAPI-protected enrollment identity and keys;
- signed restore policy and monotonic policy revision;
- bounded update/maintenance transaction state;
- bounded audit and health records;
- explicitly approved network configuration needed for stable enrollment.

The NSTU executable directory, DLLs, scripts, plugin/search paths, startup
entries, and any directory from which code can execute must not be writable by
students and must not be broadly excluded. Never exclude all of `C:\Users`,
`C:\ProgramData`, the Windows directory, or a browser profile merely for
convenience. Every exclusion becomes a persistence path for both legitimate
state and an attacker.

The qualification checklist must explicitly reject exclusions covering the
Windows system/configuration files and boot-critical paths documented by
Microsoft, including `\Windows`, `\Windows\System32`,
`\Windows\System32\config\{DEFAULT,SAM,SECURITY,SOFTWARE,SYSTEM}`,
`BOOTSTAT.DAT`, the volume root, `Drivers`, and the page file. These paths must
remain protected unless a documented Windows servicing procedure specifically
requires a temporary, reviewed change.

Student work must be redirected to a school-approved network location, cloud
location, or separate user-data volume. The UI must clearly warn that work left
on a protected volume is discarded after restart.

## Overlay policy for low-spec PCs

The target i5-6400/8 GB machines make a large RAM overlay unattractive. The
initial pilot should therefore evaluate a **disk overlay with non-persistent
state** and, where supported and validated, free-space passthrough. This is a
test hypothesis, not a production default.

Do not hard-code one overlay size for every school. Size it from measured peak
write volume for a full teaching day plus a documented safety margin. Validate
Windows, browser, office suite, antivirus, exam mode, printing, and unexpected
large-download workloads. Configure warning and critical thresholds below the
maximum and report all three values to the server.

Persistent overlay mode defeats the normal "discard every reboot" expectation
and Microsoft describes it as experimental. NSTU must keep it off unless a
future, separately reviewed policy explicitly requires it.

Fast Startup must be disabled because a Fast Startup shutdown does not clear
the overlay. UWF side effects depend on the activation path: Microsoft
documents changes to paging, System Restore, SysMain, indexing, fast boot,
drive optimization, boot status, Windows Update, Store updates, and maintenance
when UWF is enabled through `uwfmgr.exe`/WMI on a running installation, while
SMI or unattend provisioning does not necessarily apply the same changes. The
installer must record the baseline, identify the activation method, and report
which settings actually changed before activation. Disabling UWF later does not
automatically restore every performance setting; the baseline and desired
post-removal values must be recorded.

## State model

The server and client must show both **current session** and **next session**
state. A queued setting is not the active setting.

```text
unsupported
  -> audit-only
  -> eligible/unconfigured
  -> feature-reboot-pending
  -> ready/unprotected
  -> protection-reboot-pending
  -> protected/healthy
  -> servicing-reboot-pending
  -> servicing
  -> validation-reboot-pending
  -> protected/healthy
```

Any state may move to `blocked` or `recovery-required`. A transaction records a
boot identifier and a bounded attempt count. NSTU must never enter an automatic
reboot loop. After two failed boot/health attempts, the device stops automatic
mutation, remains visible as quarantined when networking works, and requires a
technician to follow the recovery runbook.

## Qualification and first activation

Protection must be opt-in and separate from ordinary NSTU installation.

1. Inventory the exact SKU/build, firmware mode, Secure Boot, disk layout,
   BitLocker state, recovery-key custody, free space, file system, Storage
   Spaces use, page-file location, security products, network profile, and all
   local accounts.
2. Reject unsupported editions, Storage Spaces, an unavailable UWF feature/WMI
   provider, missing recovery material, an unhealthy file system, or an image
   that has not passed backup and restore testing.
3. Build and verify a known-good image before protection. UWF is not a backup
   and cannot repair a bad baseline.
4. Enable only the Windows optional feature, then restart and verify that the
   feature and WMI provider are healthy.
5. While the filter is disabled, apply the reviewed protected-volume,
   exclusion, overlay, threshold, and persistence policy. Display all UWF side
   effects and require local administrator confirmation.
6. Enable protection for the next session and restart through an approved
   Windows/UWF restart path.
7. After boot, verify current and next filter state, protected volume identity,
   exclusions, overlay mode, thresholds, Fast Startup state, service account,
   agent session, enrollment, server handshake, snapshots, chat, and audit
   persistence.
8. Perform a sentinel test: create a harmless test file on the protected
   volume, restart, prove it disappeared, and prove approved persistent state
   survived. Do not enable fleet controls until this passes.

Configuration should bind protected volumes by stable volume identity where
possible rather than assuming that `C:` always denotes the intended volume.

## Daily operation and overlay exhaustion

The client reports filter state, overlay type, maximum size, consumption,
warning/critical state, last successful reset, boot identity, and pending
maintenance. The server does not collect file names from the overlay during
normal operation.

At warning threshold, notify the teacher and stop nonessential writes such as
diagnostic capture. A critical-threshold event is telemetry; it does not by
itself mean that NSTU may restart the computer. At the maximum overlay size,
Windows may trigger its own automatic restart on supported builds, which is a
separate exhaustion risk that the qualification matrix must measure. NSTU may
force a restart only under an explicit deployment policy and consent path,
after checking exam state and warning users to save work externally. When the
overlay is full, normal shutdown can take an extremely long time; use the
documented UWF restart operation in the tested recovery path, and audit every
exception where continued operation is judged less safe than data loss.

## Servicing and NSTU updates

UWF-protected devices need an explicit servicing workflow. UWF normally changes
Windows Update and maintenance behavior because ordinary updates would be
discarded. Microsoft's documented servicing path can clear the overlay,
restart, temporarily disable filtering, apply a locally staged update, and
re-enable protection; the exact mechanism and failure behavior must be verified
on each supported image. NSTU must not imply that servicing mode installs an
arbitrary network package or guarantees re-enablement after a failure.

NSTU should integrate this workflow with the signed-package design in
`AUTO_UPDATE_LTSC.md`:

1. A deployment administrator opens a maintenance window; the client rejects
   the request during an exam or while user-data synchronization is incomplete.
2. Verify AC power, recovery artifacts, free disk/overlay space, server
   compatibility, signed manifest/package, role, version, and anti-downgrade
   policy before changing UWF state.
3. Persist a signed, bounded transaction outside the discardable overlay.
4. Enter the appropriate documented UWF servicing or filter-disabled path for
   the next session and restart. The mechanism must be explicit, tested, and
   able to recover if NSTU is unavailable. Microsoft servicing mode requires
   every local account to have a password; preflight must enforce that
   condition.
5. Apply only a package already verified and staged locally (or on an
   explicitly trusted recovery medium). Do not download or execute an
   unverified payload in the unprotected session.
6. Use the documented mechanism to re-enable protection and restart, then
   prove from the post-boot current state that the filter is active before
   reporting success.
7. Commit the new NSTU version only after health checks and protection proof
   pass. Otherwise leave a recoverable, explicitly quarantined state and run
   the signed package rollback/recovery-image procedure; never silently leave
   the machine unprotected, and stop after the bounded attempt count.
8. Report the final current/next UWF state and package version to the server.

The prototype must validate the exact custom-application servicing behavior on
every supported LTSC build; a repository state-machine test is not proof that
Windows servicing mode completed correctly.

## Decommission and uninstall

NSTU must not remove the management service and leave a machine in an unknown,
unmanaged protected state.

1. Require deployment-administrator authorization plus local technician
   confirmation and recovery-key availability.
2. Record the intended final policy: transfer UWF management to another tool,
   or disable protection and optionally remove the Windows feature.
3. Disable/unprotect only for the next session, restart, and verify the current
   session is unprotected.
4. Export the final audit record and then run the existing reboot-bound NSTU
   uninstaller.
5. Removing the UWF Windows feature is a separate explicit action; do not remove
   a Windows component merely because NSTU is removed.

If the service is damaged while protection is active, recovery uses documented
Windows/UWF administration from a trusted local recovery procedure, a
known-good system image or recovery media, and a tested way to disable or
unconfigure UWF when NSTU cannot start. NSTU must publish that procedure and a
signed offline diagnostics package before the feature can leave pilot status.

## Failure behavior

| Failure | Required result |
| --- | --- |
| Server unavailable during normal protected use | Remain protected; local reboot-to-restore continues; no policy mutation |
| Invalid/expired/replayed admin request | Reject and audit without changing next-session state |
| Network lost before maintenance reboot | Cancel or remain in the current protected state |
| Network lost in an unprotected servicing session | Finish only the verified local transaction or stop for technician recovery; never fetch a new payload |
| Current and next UWF state disagree with the transaction | Stop automatic actions, quarantine, and require recovery |
| Overlay warning/critical event | Notify, shed nonessential writes, and follow the tested restart policy |
| Repeated boot or health failure | Stop after the bounded attempt count; no reboot loop |
| Persistent-state ACL or signature invalid | Do not mutate UWF; report recovery-required |

## Test and release gates

This feature is destructive by nature. Windows Sandbox is insufficient for
qualification because it cannot provide the required persistent, checkpointed
multi-reboot lifecycle. Use checkpointed, persistent VMs first, followed by
disposable physical test PCs. Never run early mutation tests on a school
production image.

### Gate 0: documentation and review

- Microsoft UWF behavior and edition matrix cited and reviewed.
- Threat model, persistent-data boundary, exclusions, recovery ownership, and
  licensing approved by project and school IT stakeholders.
- Independent review agrees that no custom kernel component is required.

### Gate 1: read-only capability probe

- Unit tests use a fake WMI adapter and cannot change the host.
- Probe correctly distinguishes supported, unsupported, feature-missing, and
  inconsistent current/next states.
- Server UI labels the function experimental and read-only.

### Gate 2: persistent VM mutation trial

- Clean install, feature enable, protect, reset, service, disable, uninstall,
  and recovery complete across real reboots on every claimed Windows image.
- Tests cover sudden power loss, full overlay, failed update, corrupt state,
  expired command, server loss, no-password account, BitLocker, and rollback.
- VM checkpoints and a clean image recover every intentionally failed case.

### Gate 3: physical low-spec validation

- Test on the i5-6400/8 GB/Intel HD 530 baseline with both HDD and SSD storage
  represented where schools use them.
- Measure boot time, daily overlay growth, RAM, CPU, disk latency, network
  persistence, and at least 50 consecutive protect/reset cycles.
- Validate antivirus, Office/browser workloads, printing, exam mode, power
  interruption, and classroom shutdown behavior.

### Gate 4: controlled pilot

- One non-production lab and a small client cohort run for a complete teaching
  cycle with daily audit review and an on-site recovery path.
- No unexplained persistence, overlay exhaustion, reboot loop, loss of
  enrollment, or failed restore is accepted.
- School IT signs off on user-data handling and emergency recovery.

### Gate 5: production eligibility

- Feature remains disabled by default and is enabled only by signed policy for
  exact validated SKU/build/image combinations.
- Code signing, protocol authorization review, fuzzing, updater/rollback, UWF
  servicing, offline recovery, and incident runbooks are complete.
- A failed capability or health check always removes the device from rollout
  rather than weakening protection.

## Delivery phases

1. **Research complete:** this design and official-source matrix.
2. **Read-only telemetry:** capability, current/next state, overlay health, and
   event-log reporting; no mutation methods compiled into production builds.
3. **Local lab controller:** typed WMI operations behind a lab-only build flag,
   local confirmation, and persistent-VM tests.
4. **Servicing coordinator:** signed transaction state, update integration,
   bounded recovery, and decommission flow.
5. **Server administration:** separate deployment role, scheduled fleet waves,
   current/next state visualization, and audit export.
6. **Physical pilot and independent review:** only then consider an opt-in
   production policy.

The project should not estimate a production date until Gates 0-3 pass. The
highest-risk work is not the dashboard; it is preserving bootability and a
recoverable, auditable maintenance path through every failure.

## Official references

- [Unified Write Filter overview and edition requirements](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/)
- [Turn on and configure UWF](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-turnonuwf)
- [UWF overlay sizing, thresholds, and exhaustion](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfoverlay)
- [Service UWF-protected devices](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/service-uwf-protected-devices)
- [UWF exclusions](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfexclusions)
- [`uwfmgr.exe` reference](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfmgrexe)
- [UWF WMI provider reference](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-wmi-provider-reference)
- [UWF troubleshooting and event logs](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwftroubleshooting)

References were reviewed on 2026-09-09. Microsoft documentation is the
authority for platform support and may change; revalidate it before each
production release.
