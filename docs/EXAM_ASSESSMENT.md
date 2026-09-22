# Computer-Based Assessment

The first exam UI is a self-contained web surface under `exam/web/`. It is
intended to run from a local `.nstuexam` package in the native WebView2 host of
a WebView2-enabled client. A regular browser or other embedded browser can be
used for static UI preview, but authenticated answer transport and kiosk
behavior require the native host. It has no CDN or runtime download dependency.

## Package boundary

An `.nstuexam` package is a ZIP container with a UTF-8 `manifest.json`, the
required local entry page `exam/web/index.html` and its UI assets, local media,
and optional PDF references. Release packages also contain `manifest.p7s`:

```text
ielts-sample-01.nstuexam
|-- manifest.json
|-- manifest.p7s
|-- exam/
|   `-- web/
|       |-- index.html
|       |-- app.js
|       `-- styles.css
|-- documents/
|   `-- reading.pdf
`-- media/
    `-- listening-01.mp3
```

The manifest is validated against
`exam/schema/manifest.schema.json` before any file is published. The staging
validator accepts only the documented properties, required fields, bounded
integer/string values, supported question types, and unique document/question
IDs. Supported question types are `multiple_choice`, `short_answer`, `essay`,
`listening`, and `reading`. Every non-empty document `url` and question
`audio`/`pdf` reference must resolve to a regular file in the same archive;
references to directories, `manifest.json`, `manifest.p7s`, traversal paths,
queries/fragments, encoded separators, and network URLs are rejected. The
manifest must be strict UTF-8 JSON without a BOM, matching the native host's
parser contract.

## Deployment-owned staging

The client host accepts only an unpacked package. Deployment staging is
performed by the administrator-owned
`docs/deployment/stage-exam-package.ps1` helper shipped with the
installer. It uses the Windows `System.IO.Compression` API and
publishes into an ACL-protected, content-addressed directory below the
persistent client data root:

~~~powershell
$stager = "$env:ProgramFiles\NSTU\docs\deployment\stage-exam-package.ps1"
& $stager -ArchivePath "D:\SecureTransfer\ielts-sample-01.nstuexam" -PublishRoot "$env:ProgramData\NSTU\exams\packages" -ExpectedArchiveSha256 "<release-archive-sha256>" -ExpectedContentSha256 "<unpacked-content-sha256>" -TrustedPublisherThumbprint "<approved-publisher-thumbprint>"
~~~

`PublishRoot` is a required, caller-supplied absolute path. Deployment policy
must keep it under the configured persistent client data root; the helper does
not discover registry roots, choose a server path, or download/copy an archive
automatically. The root must be a non-root, non-reparse directory. The optional
`-RequireAuthenticode` switch adds a separate Authenticode check for every
`.exe`/`.dll` in the package; it does not replace the required detached
`manifest.p7s` publisher signature.

Both SHA-256 pins are required. The content digest is the same sorted
relative-path plus file-bytes digest that `ExamHost` verifies
immediately before mapping the WebView2 virtual host. The helper rejects
absolute, traversal, device-name, alternate-data-stream, duplicate, collision,
link, reparse-backed, encrypted, truncated, oversized, and zip-bomb-shaped
entries. It extracts into a private temporary directory, verifies the complete
tree, and uses a same-volume `MoveFileExW` rename with write-through and
fail-if-existing semantics. A per-package lock serializes publishers; a race
with an existing destination is accepted only after the complete tree and
metadata are revalidated. Existing content-addressed directories are never
overwritten. A strict sidecar records the archive/content pins, file count,
expanded bytes, signature state, and publication timestamp outside the package
tree. If sidecar publication fails after a new directory is published, that
directory is removed only after its digest is rechecked; temporary files are
always cleaned up.

Release deployments require a detached CMS/PKCS#7 `manifest.p7s`
entry and an explicit publisher certificate thumbprint. `-AllowUnsigned`
and `-AllowNonElevatedTest` are development-only switches used by the
disposable staging test; they must not appear in school deployment commands.
The server remains the persistent owner of the package and answer journal.

## Host integration

The native `ExamHost` is implemented in `client/src/exam_host.cpp` and runs in
`nstu-agent` on the agent UI/STA thread. In a build configured with
`NSTU_ENABLE_WEBVIEW2_HOST=ON`, it:

1. Receives an already-unpacked, deployment-staged package root from the
   authenticated agent command. ZIP extraction and publisher-signature
   verification are performed by the deployment helper outside the host
   boundary; the host does not unpack archives, and it still verifies the
   pinned content digest itself.
2. Validates the manifest, media/document paths, package quotas, reparse-point
   rules, and a pinned SHA-256 package digest before navigation.
3. Creates a topmost full-screen Win32 window, maps the package through the
   `https://nstu.exam/` virtual host, injects the host-supplied manifest and
   identity context, and navigates only to the validated local entry page.
4. Handles WebView2 answer-recovery messages and forwards validated events through
   the agent/service boundary to the server's authenticated control channel.
5. Drains authenticated acknowledgements and state chunks on the agent UI thread
   and restores the last accepted answer state after a client reconnects. The
   browser uses a bounded, context-scoped `localStorage` recovery buffer only;
   its durable key is derived from the host-supplied package digest, client ID,
   session ID, and candidate ID. Before a complete trusted context is injected,
   the page uses only an unbound per-page namespace and does not load or send
   prior recovery data. Once a trusted context is available, only the storage
   belonging to that exact context is loaded and preserved. Switching contexts
    reinitializes the active view and chain cursor for the new context; a later
    return can use that context's own record, but records are never merged across
    contexts. The browser cache is never the authoritative record.

The WebView2 user-data folder is also context-scoped. The client derives a
filesystem-safe `v2-<package-digest>-<client-id>-<session-id>` profile leaf below
the interactive user's approved `%LOCALAPPDATA%\\NSTU\\exam-webview` root. This
prevents a service worker, cache entry, cookie, or other browser artifact from a
different package or client identity being reused at the fixed
`https://nstu.exam/` origin. A non-empty `user_data_root` in the wire request is
accepted only when it canonicalizes to that exact derived leaf; it cannot select
an arbitrary persistent profile. Reconnects for the same authenticated tuple
intentionally reuse the leaf so browser-local recovery remains available.

The host blocks navigation outside the local virtual host, popup windows,
developer tools, default context menus, and zoom controls. It also keeps the
window foreground and suppresses common user-mode shortcuts while the exam is
running. This is a user-mode kiosk boundary, not a guarantee against the secure
desktop, a local administrator, or kernel-level software. Clipboard/download
policy, production publisher certificate-chain/revocation policy, instructor
authorization, and a complete proctoring workflow remain production gates.

The WebView2-enabled build requires the WebView2 SDK at build time,
`WebView2Loader.dll` beside `nstu-agent.exe`, and a compatible WebView2 Runtime
on the client. The agent loads only that packaged loader (it does not fall back
to an arbitrary system DLL search path), rejects a missing or reparse-backed
copy, and probes the Runtime version before creating the environment. A clean
machine therefore needs the loader delivered with the selected client package
and the Evergreen WebView2 Runtime installed or deployed by the school image.
Builds with `NSTU_ENABLE_WEBVIEW2_HOST=OFF` retain the protocol and bounded
`ExamBridge` coverage for offline/CI builds, but intentionally fail closed when
an exam is started; they do not provide an exam browser.

## Answer recovery bridge

Before navigation, the host injects an identity-only context. The exam manifest
must never be allowed to select the client or session identity:

```js
window.NSTU_EXAM_CONTEXT = {
  packageId: "ielts-sample-01",
  packageDigestHex: "64 hexadecimal characters",
  clientIdHex: "32 hexadecimal characters",
  sessionIdHex: "32 hexadecimal characters",
  candidateId: "candidate-id",
  previousEventHashHex: "optional 64-character hash",
  nextSequence: 1
};
```

The page emits `exam_ready`, then sends one `exam_answer_event` at a time. Text
answers are UTF-8 strings; structured controls are canonical JSON strings.
For every `upsert`, `clear`, and `finalize` event, the page serializes the
canonical `NEV1` event encoding and computes its SHA-256 `eventHashHex` locally.
The hash is not accepted as an arbitrary package field: stored events must
recompute to the same digest, and the native host and server still validate the
chain. The event remains in a bounded browser queue until the host sends an
`exam_answer_ack` with status `accepted` or `duplicate` and the matching
`eventHashHex`. A rejected or unavailable event is retained for retry and is
shown as a pending recovery state.

Submission is fail-closed with respect to local durability. Before marking the
assessment submitted, the page makes sure every visible answer has a durable
draft or pending event, writes the answer map, and persists the finalization
marker. If serialization, quota, or browser storage access fails, submission
is not marked complete and `exam_submit` is not dispatched; the UI reports that
recovery storage is unavailable or full. Once that durable checkpoint succeeds,
`exam_submit` is sent for the host's response-package workflow and a durable
`finalize` event is queued after all answer events.

The page retransmits an unacknowledged event after five seconds with bounded
backoff. It accepts an `accepted` or `duplicate` ACK only when the session,
sequence, non-zero event hash, and contiguous watermark are valid. A reconnect
state containing `lastEventHashHex` rebases the first pending event and clears
stale predecessor hashes on later events; events already covered by the server
watermark are retired for that session.

Example page-to-host event:

```json
{
  "type": "exam_answer_event",
  "event": {
    "packageId": "ielts-sample-01",
    "packageDigestHex": "...",
    "clientIdHex": "...",
    "sessionIdHex": "...",
    "candidateId": "candidate-01",
    "questionId": "writing-01",
    "questionRevision": 1,
    "sequence": 7,
    "clientTimeUnixMilliseconds": 1770000000000,
    "kind": "upsert",
    "answer": "UTF-8 answer text",
    "previousEventHashHex": "..."
  }
}
```

The host replies with `{ "type": "exam_answer_ack", "ack": { ... } }`,
where `status` is `accepted`, `duplicate`, `gap`, `conflict`, `rejected`, or
`unavailable`. For a reconnect, the host sends `exam_state_response` with the
accepted answers, `highestContiguousSequence`, and (when available)
`lastEventHashHex`. For the matching trusted context, the accepted server state
is authoritative for every question without a local pending draft or event:
the page rebuilds that portion of the answer map, removes answers omitted by
the state (including server-side `clear`/reset results), and then overlays
locally pending work. Thus stale browser answers cannot survive an authoritative
recovery, while newer unsent answers are preserved. The native host must
validate every identity, revision, size, and hash-chain field before converting
the message to the packed C++ protocol.

The client install places `exam/web`, `exam/schema`, and `exam/examples` under
`%ProgramFiles%\\NSTU\\client\\exam`. The service response branches publish
only `exam_answer_ack` and `exam_state_response` messages, cap the bridge queue
at 64 entries, and notify the agent window with a private `WM_APP` message. In
the WebView2-enabled build, `ExamHost` drains that queue on the agent UI thread;
it never reads the service pipe directly and a dropped UI notification is not
treated as data loss. A build without WebView2 still exposes the bounded bridge
for protocol and lifecycle tests, but cannot launch the exam surface.

State recovery may be split across several authenticated
`exam_state_response` messages. The page buffers a set only when all chunks
share package, session, candidate, sequence, and state-hash metadata; it then
requires every index from `0` through `chunkCount - 1` exactly once before
merging answers. Reassembly is capped at 64 chunks and 512 answers and expires
after 15 seconds. The native splitter and `ExamBridge` enforce the same
64-chunk ceiling; a snapshot that would need more chunks fails closed instead
of being partially delivered. Pagination is reserved for a future protocol
revision, so the host should keep recovery snapshots within this bound and
resend the complete set when a chunk is missing.

Durable browser entries are validated before they are loaded. Invalid entries
are retained only as bounded diagnostic summaries. Storage keys are scoped to
the complete trusted context, so the manifest's display candidate cannot select
which answer history is opened. Once the host injects a context, pending events
and drafts whose package digest, client ID, session ID, or candidate ID do not
match it are moved to a bounded quarantine and are never transmitted. A context
switch reinitializes the active chain cursor and visible recovery view for the
new context; returning to a matching context uses only that context's own
storage. No prior-context record is merged into another session. This prevents
a stale exam session from blocking or contaminating a new session; the UI
reports that recovery data is being held for another context. A reconnect can
still rebase matching pending events from the server's `lastEventHashHex`
watermark.

Every state response, including a single-chunk response, must carry consistent
package, digest, client, session, candidate, chunk, and finalized metadata. If
`highestContiguousSequence` is greater than zero, both `stateHashHex` and a
non-zero `lastEventHashHex` are required. A zero watermark may omit those
fields, but a supplied last-event hash must be all zero. Responses that fail
these checks are ignored without modifying local answers.

If a reconnect watermark covers a still-pending browser event, the page retires
that event only when its individual event hash can be matched. If the hash is
missing or differs, recovery enters a manual-review state and leaves the local
pending record intact; it will not guess or overwrite the answer. The
technician should export the local response and reconcile it with the server
journal before resuming the session.

The v3 response format is a coordinated client/server rollout boundary. The
native decoder still accepts v1 and v2 responses for staged migration and
archival inspection, but those versions do not carry `lastEventHashHex`; a
browser cannot safely rebase a pending hash chain from such a response. Run a
v3-capable server with v3-capable clients together before enabling answer
recovery, and do not claim mixed-version recovery support. Capability
negotiation is intentionally deferred until a future protocol revision.

Each authenticated state-response payload is limited to
`kMaximumExamPayloadBytes` (65,512 bytes with the current 64 KiB command limit,
8-byte command sequence, and 16-byte authentication tag). A complete response
may contain at most 64 chunks and 512 answer entries. The browser holds an
incomplete set for 15 seconds and requires every index exactly once; the host
must resend the complete set when a chunk is lost.

### Durable client outbox (`OBX1` inside `EOB1`)

The client write-ahead outbox is separate from the authoritative server journal.
Its logical binary payload is `OBX1` version 4. The format history is:

| Version | Persisted addition |
| --- | --- |
| 1 | Legacy event records only. |
| 2 | Per-session watermarks and the last event hash. |
| 3 | The acknowledged sequence/hash boundary. |
| 4 | The finalization marker, preventing events after `finalize`. |

New writes are wrapped in `EOB1` version 1 and protected with Windows
machine-scoped DPAPI (`CRYPTPROTECT_LOCAL_MACHINE`). On open, the client can
validate legacy plaintext and older `OBX1` versions, then atomically migrates
them to the encrypted v4 representation before exposing events to the runtime.
An authentication or integrity failure is an open error; the client must not
fall back to transmitting an unvalidated file. The outbox is bounded to 512
events and 8 MiB of logical plaintext, with a separate 64 KiB allowance for the
DPAPI wrapper. It is a retry buffer only: an event is removed after a matching
durable ACK, while the server journal remains authoritative.

### Framed state export (`SEX1`)

`AnswerJournal::export_state` writes a point-in-time state export in the framed
`SEX1` version 1 container. The header is a little-endian `u32` magic (`SEX1`),
a `u16` format version, and a `u16` chunk count. It is followed by repeated
`u32` little-endian length + complete `NSR1` payload records. Each record is an
independently decodable state-response chunk; consumers must honor the length
boundaries, validate every chunk, and reject truncation, duplicate metadata, or
trailing bytes. Concatenating the payloads and parsing them as one response is
incorrect. The complete export is capped at 256 MiB.

The export is a diagnostic/backup view, not a replacement for the append-only
journal and not an encrypted container. On Windows the atomic writer applies a
restricted SYSTEM/Administrators/owner DACL, but operators must still store and
transfer exports as sensitive answer data.

The page supports a PDF reference pane, listening audio, multiple-choice,
short-answer, essay, reading, timer, autosave, language selection, text-size
and contrast settings, keyboard navigation, and JSON response export. It does
not grade essays or accept arbitrary JavaScript from an exam package.

## Security and recovery requirements

- Keep exam packages and answer records on the persistent server; do not place
  them in a client path managed by UWF or third-party freezing software.
- The current host blocks external navigation, popups, developer tools, default
  context menus, and zoom controls in the exam WebView2 profile. Add explicit
  download and clipboard policy before production use.
- Sign packages in the deployment/release pipeline and pin the expected package
  digest before an exam starts. The staging helper verifies the detached
  publisher signature; the native host intentionally rechecks the content
  digest and path boundary rather than repeating CMS certificate validation.
- Treat the server's answer log as append-only and include package version,
  question revision, candidate identity, and event time.
- Close the exam through an authenticated server command or a local technician
  action. A client reboot must not erase the server-side response record.

### Authorization boundary

The current control plane authenticates the client during the handshake and
installs a server-side active-exam context when `start_exam` succeeds. The
version-2 `ExamStartRequest` carries the manifest `package_id` as a required
field; every answer event and state request must match that context's client
identity, package ID, package digest, session ID, and candidate ID. The context
survives a normal reconnect and is removed by `stop_exam` or server shutdown.
The native client host also compares the server-issued package ID with the
manifest before opening WebView2. This blocks browser messages from changing
the journal tuple, but it does **not** yet verify that
the package, session, and candidate were authorized by an instructor or
deployment administrator. That signed, auditable authorization binding remains
a pre-production security blocker.

The sample manifest is in `exam/examples/ielts-sample.json`. This UI is the
interaction layer; scoring rules, teacher authoring, proctor controls, and
server-side finalization remain separate milestones in the roadmap.

## Reboot-to-restore protection gate

Exam mode is authorized by the server only when the target client has proven
current-session UWF (reboot-to-restore) protection. The gate is server-side and
fails closed: the client never asserts a protection flag, and nothing about
protection is carried in the exam start request. See
[REBOOT_TO_RESTORE.md](REBOOT_TO_RESTORE.md) for how the boot-bound proof is
established and why a reconnect retires it.

The publicly released `NSTU DEV (UNPROTECTED)` build bypasses this one gate for
testing and nothing else; every bypassed start is audited at `severity=warning`
and the build is conspicuously labeled in the UI and installer. Release builds
cannot start an exam on an unprotected client.
