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
| Multicast switches | Sender/receiver output for every supported switch/VLAN/IGMP configuration | Pending | |
| Unicast fallback | Forced multicast failure and verified recovery hysteresis | Pending | |
| Windows matrix | Supported Windows 10/11 builds, setup-check results, clean install, upgrade, reboot, uninstall | Pending | |
| Intel driver matrix | Supported GPU models and driver versions, capture/encode/device-loss recovery | Pending | |
| Deep Freeze | Every supported edition/version, Thawed install/enroll/upgrade/uninstall and Frozen operation | Pending | |
| Security review | Independent protocol/code review and fuzzing results | Pending | |
| Confidentiality decision | Documented LAN threat model; deploy encryption before use where screen confidentiality is required | Pending | |

Use `packaging/test-production-deployment.ps1 -RequireSignedArtifacts` on each
machine before and after the soak. Use `tools/production/test-multicast.ps1` on
separate sender and receiver machines for the switch matrix. Run the guarded
Windows Sandbox lifecycle harness for pre-reboot service/uninstall evidence,
then complete the persistent reboot and Deep Freeze sequence in
`docs/VM_TESTING.md`; Sandbox results alone cannot pass those gates.
