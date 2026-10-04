# AGENTS.md — NSTU

Rules for any AI agent working in this repository (Antigravity, Claude Code,
Gemini CLI). **Antigravity has no hooks**, so nothing here is enforced
mechanically — the only enforcement is `scripts/verify-gate.sh` plus human
review. Read this before your first edit.

## What NSTU is

Windows classroom management over a LAN: a teacher app sees and controls student
PCs — screen monitoring, lock/unlock, chat, live view, remote control, exam mode,
and reboot-to-restore (UWF).

| Executable | Role |
|---|---|
| `nstu-server.exe` | Teacher manager. Dear ImGui + D3D11. Listens TCP **47001**. |
| `nstu-service.exe` | Student LocalSystem service. Owns the authenticated link; relays commands; launches the agent. |
| `nstu-agent.exe` | In-session student UI. JPEG capture, chat, overlay, `SendInput`. |
| `nstu-diagnostics.exe` | Standalone readiness/diagnostics tool. |

Trust between server and client is anchored by a **six-digit pairing code (SAS)**.
Treat it as the security root: never auto-approve a pairing without the code
being confirmed on both machines.

## Hard rules

1. **Naming ban.** No C++ file, target, class or field may be named `moonlight`
   or `sunshine`. Vendor names belong only in UI strings, config, and docs.
2. **Commit identity** `khoasoma <songtu438@gmail.com>`. **Zero** attribution or
   `Co-Authored-By` lines.
3. **Never stage** `artifacts/`, `.claude/`, or `HANDOFF.md`.
4. **Never commit, push, or open/merge a PR** without an explicit human
   go-ahead. Leave finished work in the working tree.
5. **Do not claim exam-safety.** The exam network lockdown installs only
   `ALE_AUTH_CONNECT_V4` and blocks only ports 80/443 with per-IP permits — so
   IPv6 and any non-web port bypass it. Saying it is exam-safe is false.
6. **Do not weaken the bar to reach green.** No deleted assertions, no skipped
   or disabled tests, no lowered thresholds, no stubbed implementations to make a
   test pass.
7. **Do not self-certify.** Report the actual gate output, not a claim.

## The gate

Local — necessary but **not sufficient**:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH   # MUST precede /mingw64/bin, or tests
                                         # die with 0xc0000139 (DLL mismatch)
./scripts/verify-gate.sh                 # fast: zero-warning + full ctest
./scripts/verify-gate.sh --strict        # mirrors CI's warning posture
```

The **real** gate is GitHub Actions on `windows-2022`: MSVC `/W4 /permissive-`
with `-DNSTU_ENABLE_WERROR=ON` (i.e. `/WX`), **Release**, across three jobs
(`build-test`, `internal-vm-build`, `dev-unprotected-build`).

The default local build is **GCC, Debug, `WERROR=OFF`** — three independent
differences from CI. Commit `4e43e6a` was a signed/unsigned comparison that
MinGW did not flag and MSVC rejected. **Run `--strict` before you say done.**

### Gate caveats (read before trusting it)

- **Run it unpiped.** `./scripts/verify-gate.sh | tail` returns *tail's* exit
  status, so a FAIL becomes a silent 0. Redirect instead:
  `./scripts/verify-gate.sh > build/gate.log 2>&1; echo $?`.
- **Use Git Bash** (`C:\Program Files\Git\bin\bash.exe`). MSYS bash has no `git`
  on PATH and the gate refuses to run.
- **MSVC is not installed on this machine** — only the VS Installer stub. CI is
  the sole MSVC check, so push-and-watch is part of "done".
- **MSVC is authoritative, GCC is not a faithful mirror.** `--strict` uses GCC
  `-Werror`, which raises diagnostics MSVC does not, and vice versa. A green
  `--strict` does not prove CI will pass.
- **One suppression is deliberate:** `-Wno-free-nonheap-object`. GCC 15 raises it
  inside libstdc++'s `new_allocator.h` at `-O2`, inlined from an ordinary
  `std::vector` insert in `common/src/discovery.cpp` (pointer reported as
  `'<unknown>'`). That code performs no manual deallocation and MSVC `/WX` is
  clean on it — it is a false positive. **Do not restructure that code to
  silence it.**

## Division of labour

- **Antigravity implements.** Task specs are written to `artifacts/` (never
  staged) — read the spec fully before editing.
- **Claude Code plans, audits, and owns the gate.** It re-runs the gate
  independently; a reported "green" is not accepted as evidence.
- **Report back:** files changed, names of tests added, and verbatim build +
  ctest output.

## Repository map

```
server/   teacher app + control plane        client/   service, agent, platform lib
common/   protocol, crypto, audit, WFP       setup/    installer + diagnostics
exam/     schema, studio (authoring), docs   tests/    ctest suite
video/    DORMANT H.264 stack — zero production consumers; do not extend
```

## Environment traps

- Rebuilding the server requires stopping the running `nstu-server.exe` first —
  it locks the exe.
- Prefer a **graceful** server exit (tray → Exit) over force-kill; force-kill can
  lose the keyring and de-pair clients.
- A black Focus/remote surface on the test VM means a **disconnected RDP
  session**, not a bug — capture returns black when nothing composites.
- Test VM `192.168.1.237`; host `192.168.1.37`. Those credentials are test-only.
