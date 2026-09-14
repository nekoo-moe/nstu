# Production Validation Record

Repository tests and packaging are necessary but do not replace this lab
record. Attach raw CSV, switch configuration, driver versions, screenshots,
and issue links for each run. Do not approve a production tag with blank or
failed required rows.

Set `Result` to exactly `Passed` only after attaching a non-empty evidence path
or link. The production workflow rejects missing, duplicated, pending, or blank
required rows.

| Gate | Required evidence | Result | Evidence path/link |
|---|---|---|---|
| Authenticode | Valid signature and trusted timestamp on the unified installer and installed executables | Pending | |
| 50-client soak | At least 8 hours, reconnect/lock/chat/snapshot/annotation/broadcast cycles, no crash or unbounded memory growth | Pending | |
| CPU/RAM/network | `collect-benchmarks.ps1` CSV at 5, 7, and 10 second snapshot intervals plus server/client hardware specification | Pending | |
| Snapshot network | 50-client authenticated TCP control/snapshot run with measured switch uplink capacity, reconnects, and no sustained queue growth | Pending | |
| Windows matrix | Supported Windows 10/11 builds, setup-check results, clean install, upgrade, reboot, uninstall | Pending | |
| Intel driver matrix | Supported GPU models and driver versions, capture/encode/device-loss recovery | Pending | |
| Deep Freeze | Every supported edition/version, Thawed install/enroll/upgrade/uninstall and Frozen operation | Pending | |
| Security review | Independent protocol/code review and fuzzing results | Pending | |
| Confidentiality decision | Documented LAN threat model; deploy encryption before use where screen confidentiality is required | Pending | |
| Exam journal durability | Crash-tail recovery, append rollback, restart/offline replay, and authoritative server journal evidence | Pending | |
| Exam outbox protection | DPAPI `EOB1` migration, quota/rollback, ACK hash validation, and client reboot persistence evidence | Pending | |
| Exam state export | Multi-chunk `SEX1` export/import with duplicate, truncation, metadata-conflict, and trailing-byte rejection | Pending | |
| Exam compatibility | Coordinated v3 client/server recovery plus explicit rejection/evidence for unsupported mixed-version rebasing | Pending | |
| Exam authorization | Instructor package/session/candidate binding, replay protection, and audit evidence | Blocked | Required before official assessments; runtime now enforces the server-issued client/digest/session/candidate context, but the instructor/deployment authorization record is still missing |
| Optional H.264 network | Future-only sender/receiver evidence for each supported switch/VLAN/IGMP configuration and forced fallback | Deferred | Not required while continuous H.264 remains deferred |

Use `packaging/test-production-deployment.ps1 -RequireSignedArtifacts` on each
machine before and after the soak. Measure the snapshot network gate with the
same switch/uplink used for the classroom rollout. The
`tools/production/test-multicast.ps1` harness is reserved for the optional
continuous-video trial and does not block the snapshot release while that
feature is deferred. Run the guarded Windows Sandbox lifecycle harness for
pre-reboot service/uninstall evidence, then complete the persistent reboot and
Deep Freeze sequence in `docs/VM_TESTING.md`; Sandbox results alone cannot pass
those gates.

The exam gates are server-first: the server must retain the package and
authoritative journal across client UWF/Deep Freeze resets. A client outbox
replay test demonstrates retry behavior only; it cannot substitute for a
server journal backup, authorization record, or grading export.
