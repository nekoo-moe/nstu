# NSTU Exam Mode

Exam mode turns a protected student machine into a locked, full-screen
computer-based assessment station. An exam is a self-contained `.nstuexam`
package (questions + web UI + media) that runs in an embedded **WebView2**
kiosk on the client; the teacher server authorizes the exam and durably
collects answers. No internet, no CDN, no runtime download.

> **This README is the map and the plan.** For the implemented host/wire/journal
> contract in full detail see [`../docs/EXAM_ASSESSMENT.md`](../docs/EXAM_ASSESSMENT.md);
> for the shipping checklist see the "Computer-based assessment" section of
> [`../docs/ROADMAP.md`](../docs/ROADMAP.md).

## Pipeline

```
author (.nstuexam)  →  stage on client  →  teacher authorizes start
   manifest + web        content-addressed,       (UWF-gated)
                         signed, digest-pinned            │
                                                          ▼
   answers  ◀── hash-chained journal ◀── service ◀── WebView2 kiosk
 (server, durable)         (ACK round-trip)            (locked, timed)
```

## What's in this folder

- `web/` — the exam-taking UI (`index.html`, `app.js`, `styles.css`): timer,
  question map, PDF/audio panes, multiple-choice / short-answer / essay /
  reading / listening, autosave, EN/VI, accessibility, JSON export. Rendered
  through the `https://nstu.exam/` virtual host; no network use.
- `schema/manifest.schema.json` — the exam definition contract.
- `examples/ielts-sample.json` — a worked manifest.

## Status at a glance

Mapping the intended exam mode onto what exists in the tree today:

| Capability | State | Where |
|---|---|---|
| Full-screen WebView2 kiosk overlay | **Built** | `client/src/exam_host.cpp` |
| Input lockdown (Win / Alt+Tab / Ctrl+Esc …, foreground re-grab) | **Built** | `exam_host.cpp` low-level keyboard hook |
| Timer + live countdown (auto-submit at 0) | **Built** | `exam/web/app.js` |
| Question map / navigation rail | **Built** | `exam/web/index.html` |
| Split question / reference pane (PDF) | **Partial** | PDF iframe + question surface exist; a true side-by-side reading layout is a polish item |
| Answer capture + durable server journal | **Built** | `common/.../exam_sync.*` (append-only, hash-chained) |
| Reconnect recovery / state sync | **Built** | ACK + `exam_state_response` bridge |
| Signed, digest-pinned package + staging | **Built** | `packaging/stage-exam-package.ps1` |
| **Start / stop from the teacher UI** | **Not built** | API exists (`server/src/control_plane.cpp`) but is never called from `server/app/main.cpp` |
| **Authoring (create / edit exams)** | **Not built** | greenfield; there is no HTTP server anywhere in the repo |
| **Instructor authorization binding** | **Not built** | called a pre-production blocker in the docs |
| **OS-level app / internet blocking** | **Not built** | the kiosk is user-mode only; no process kill, no firewall |
| **Anticheat timing telemetry** | **Not built** | only a per-answer wall-clock timestamp is captured |
| **Teacher live monitor** | **Not built** | the journal holds the data; no UI reads it |
| **Scoring / grading** | **Not built** | the server stores opaque answer bytes by design |

**In one line:** the *take-an-exam-and-collect-answers* engine is built and
hardened; what is missing is everything that makes it **operable** (teacher
start / monitor), **authorable**, **hard-locked** (OS app + net), **proctored**
(timing telemetry), and **graded**.

## How an exam runs today

1. An admin stages a signed `.nstuexam` into the client's content-addressed
   store with `packaging/stage-exam-package.ps1` — currently per machine.
2. The server sends `exam_start` on the authenticated channel — **only if the
   client proves current-session UWF (reboot-to-restore) protection** (the gate
   fails closed; DEV builds bypass it with an audited warning).
3. The agent opens the WebView2 kiosk, maps the package to `https://nstu.exam/`,
   injects an identity-only context, and navigates to the local entry page.
4. The student answers; each event streams agent → service → server into an
   append-only, hash-chained journal with an ACK round-trip and crash recovery.
5. `exam_stop` (or a technician key combo) ends it; the server record survives a
   client reboot.

## How to try it today

- Configure with the WebView2 host: `-DNSTU_ENABLE_WEBVIEW2_HOST=ON
  -DNSTU_WEBVIEW2_SDK_DIR=<sdk>`; ship `WebView2Loader.dll` beside
  `nstu-agent.exe`; the client needs the Evergreen WebView2 Runtime.
- Stage `examples/ielts-sample.json` as a package (see `EXAM_ASSESSMENT.md`).
- **Caveat:** there is no teacher button yet. The start/stop path is exercised
  only by tests (`tests/control_plane_exam_auth_tests.cpp`) and dev harnesses.
  Making it operator-driven is Phase 1 below. *(This is the honest answer to
  "is it in the codebase or still in dev?": the engine is built and tested; the
  operator surface is not.)*

## Plan (toward the full vision)

Legend: ⟲ wire up what already exists · ✚ new build · ⚠ high-risk.

### Phase 1 — Operate it from the teacher UI ⟲
Add an **Exam** panel to the server dashboard (`server/app/main.cpp`): pick a
staged package, select target clients from the registry, start/stop through the
existing `start_exam`/`stop_exam`. Surface the UWF gate (show *why* a client is
blocked). Show live state (who is in-exam, answered counts).
- *Gap it exposes:* packages are staged **per machine** by hand today. A
  server-side package registry (id + digest) and a fleet distribution path are
  needed for real classroom use.
- *Timer note:* duration lives in the manifest (`durationSeconds`); a "timer
  choice at start" means picking a package, unless we extend the `EXS` start
  request to carry a per-start override.

### Phase 2 — Authoring ✚  (the "local site to create/edit exams")
Goal: produce a valid manifest + assets → a signed `.nstuexam`. Three options:
- **A — Embedded local site (your stated preference).** A **loopback-only** HTTP
  server inside the teacher app serves an authoring SPA: CRUD exams, upload
  PDF/audio, live-preview with the real `web/` renderer, then export + sign the
  package. Cost: adds an HTTP stack (none in the repo today) and a **mandatory
  loopback bind + authentication** — never expose an unauthenticated authoring
  server on the LAN.
- **B — ImGui panel.** Author inside the native teacher UI; no new dependency;
  weaker for rich passages and live preview.
- **C — Standalone static authoring page.** Reuse the `web/` stack as a
  file-based editor in the teacher's own browser (no server); emit the manifest
  + assets for `packaging/stage-exam-package.ps1`.
- *Recommendation:* fix the manifest contract first, ship **C** quickly, then
  grow into **A** if you want authoring + signing + fleet-push in one place.

### Phase 3 — Hard isolation: block apps and internet ✚ ⚠ (authorized)
At start, the LocalSystem **service** installs egress firewall rules (WFP or
`netsh advfirewall`) and suppresses non-allowlisted applications; both revert at
stop. The network block is an **allowlist, not a blackout**: the teacher server
and the exam's **declared online-document origins stay reachable** (exams may
read online documents) while everything else is blocked — so the WebView2
resource filter and the manifest schema also grow a way to declare those
origins. **Hard requirements:** a watchdog that restores networking and apps on
exam end, service crash, or timeout (never strand a machine offline or
app-less); a conservative process allowlist (OS, critical, and NSTU processes
excluded); and it must be **impossible to engage outside a live exam on a
student machine** (never on the server, in dev, or in CI). Implemented in
**native service code**, not a shelled-out script. **Testing is deferred:**
activating the block here severs the active connection (RDP / data stream), so
it is verified on an isolated VM later — unit-test the rule-building, allowlist,
and watchdog state machine without flipping the real firewall. Recommend
**network-block first**; app suppression second. UWF reboot-to-restore is a
safety net, not a substitute for a clean, reboot-free restore.

### Phase 4 — Anticheat timing telemetry ✚  (the "answer-timing logs")
In `web/app.js`, capture per-question first-view, dwell, time-to-first-answer,
answer-change count, and selection timestamps; log focus/blur/visibility changes
and blocked-shortcut attempts. Carry them on a **new bounded `exam_telemetry`
channel** (keep the answer ledger pure) and persist them server-side like
answers. The foundation already exists: a per-answer `client_time` plus the
hash-chained journal.

### Phase 5 — Teacher live monitor and grading ✚ ⟲
A live per-student view: **time-remaining tick**, answered/total, connection
state, pending-recovery, and anticheat flags (reads the journal plus a periodic
status channel). Optional **grading**: keep the answer key **server-side only**
(never shipped in the client package), auto-grade objective items, manual-review
essay/short-answer, sealed exports — this matches the roadmap's "server-side
grading, review, sealed exports" item. Pair it with the **instructor
authorization binding** the docs flag as a pre-production blocker.

### Suggested order
**1 → 2 → 4 → 3 → 5.** Phase 1 unlocks everything; Phase 2 makes exams easy to
create; Phase 4 is cheap and high-value; Phase 3 is powerful but risky (do it
deliberately); Phase 5 closes the teacher loop. Adjustable to your priorities.

## Open decisions
- **Authoring surface:** embedded local site (A) vs ImGui panel (B) vs static page (C).
- **Isolation depth:** network-block only, or also app suppression — and what allowlist?
- **Grading:** which types auto-grade, and where does the answer key live? (Recommend: server-only.)
- **Timer at start:** manifest-fixed, or teacher-overridable per start?

## See also
- [`../docs/EXAM_ASSESSMENT.md`](../docs/EXAM_ASSESSMENT.md) — the implemented host / wire / journal contract.
- [`../docs/ROADMAP.md`](../docs/ROADMAP.md) — the "Computer-based assessment" checklist.
- [`../docs/REBOOT_TO_RESTORE.md`](../docs/REBOOT_TO_RESTORE.md) — the UWF gate that exam start depends on.
