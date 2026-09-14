# Security Protocol

## Scope

The current security layer provides live authenticated TCP control sessions,
authenticated UDP video packet primitives, replay-resistant bootstrap
enrollment, a persisted machine-scoped keyring, and protected client runtime
configuration. Periodic JPEG snapshots, teacher-screen snapshots, and overlay
strokes travel in authenticated TCP command frames. NSTU does not currently
encrypt screen content.

## Control handshake

Each installed client has a 128-bit client ID and references a provisioned key
using `key_id`. The pre-shared key itself must come from protected local machine
storage and must never be committed to the repository.
Protocol keys shorter than 256 bits are rejected.

```text
Client -> Server: AuthHello(client_id, client_nonce, unix_time, key_id)
Server -> Client: AuthChallenge(server_nonce, unix_time)
Client -> Server: HMAC(PSK, labelled transcript)
Server -> Client: HMAC(session_key, labelled transcript)
```

Both nonces are 256-bit values generated with Windows CNG. Client and server
proofs use different domain labels. The session key is also derived under a
separate label, preventing the same HMAC input from being reused across roles.

The server must verify the client MAC before inserting the hello into
`ReplayProtector`. Replay insertion is atomic and bounded. Capacity must be
sized for the maximum accepted handshake rate over the clock-skew interval, and
the TCP listener must separately rate-limit unauthenticated connections.

The blocking `client_handshake` and `server_handshake` implementations now use
the real `TcpSocket` framing path, including receive/send timeouts. A successful
handshake returns a move-only `AuthenticatedSession`; moving it into
`AuthenticatedControlChannel` transfers the session key and clears the source.
Handshake message types are rejected after channel establishment.

Before the first length-prefixed command frame, both peers exchange a fixed
32-byte connection preamble. It contains the protocol magic/version, role,
`client_id`, and `key_id`. The server can reject malformed, wrong-role, or
identity-mismatched connections before allocating a command payload or running
the HMAC proof. Preamble identity is an admission hint only: it is checked
against `AuthHello` and is never trusted without the subsequent HMAC proof.
Preamble parsing is fixed-size and allocation-free; deployments should apply a
short read timeout and per-source rate limit at this boundary.

`security::HandshakeRateLimiter` provides that bounded admission primitive. The
live IOCP listener calls it before admitting a source, records failed handshakes
and invalid framing, clears state after success, and emits structured audit
events. It tracks a source by stable address or enrollment identity, limits failures in a
fixed window, applies a temporary block after the threshold, evicts oldest
entries when the source table is full, and clears state after a successful
handshake. It must be called before `client_handshake`/`server_handshake`; it
does not replace replay protection or HMAC verification.

For local key material, `protect_machine_secret` and
`unprotect_machine_secret` use Windows DPAPI with `CRYPTPROTECT_LOCAL_MACHINE`.
`save_machine_secret` writes an ACL-restricted temporary file, flushes it, and
atomically replaces the destination with `MoveFileExW`. The file ACL grants
full access only to LocalSystem, built-in Administrators, and the file owner.
The PSK and DPAPI entropy remain deployment inputs and are never generated into
the repository.

`security::KeyStore` provides the in-process lifecycle boundary used by a
server key resolver: enrollment rejects weak or reused IDs, rotation allocates a
new monotonic ID before revoking older active keys, and revocation zeroizes key
material while retaining an ID tombstone. `resolve()` returns only active key
copies. `save_keyring` and `load_keyring` serialize active entries and revoked-ID
tombstones into a versioned binary format protected with machine-scope DPAPI,
restrictive ACLs, flush, and atomic replacement.

Initial enrollment uses a one-time 256-bit bootstrap secret. The client sends a
fresh nonce, timestamp, identity, requested key ID, and HMAC. Both peers derive
the installed PSK from the bootstrap secret and transcript, so the PSK itself is
not transmitted. The server applies clock and replay checks before enrollment,
persists the keyring before acknowledging, and rolls back the in-memory change
if persistence fails. `nstu-provision.exe` stores the resulting client runtime
configuration under machine-scope DPAPI. The bootstrap export must be distributed
out of band and deleted after enrollment.

## Authenticated control frames

After the handshake, every command carries:

- a strictly monotonic 64-bit sequence;
- a 128-bit truncated HMAC-SHA256 tag;
- the normal command envelope and payload.

The MAC covers a domain label, serialized command envelope, sequence, and
payload. Because TCP preserves order, `ControlSequenceGuard` requires the exact
next sequence. A reconnect creates a new session key and resets both directions
to independently negotiated initial sequences.

The shipped v1 transport uses the legacy neutral control-MAC domain for wire
compatibility. `ControlDirection` V2 helpers are covered by unit tests but are
not enabled by the live channel yet; enabling them requires an authenticated
capability/version rollout across server and clients. Do not describe the
current v1 traffic as direction-bound.

Verify the MAC before applying the sequence guard. Only advance the guard after
successful verification. Do not execute, log as trusted, or acknowledge an
unauthenticated command.

Snapshot JPEG payloads are capped at 60 KiB before transport, with authenticated
metadata bounded to 480x270. The server accepts only JPEG containers, verifies
the decoded dimensions against that metadata, and allocates a bounded pixel
buffer. The service keeps only the newest pending client snapshot, while the
server publishes immutable shared JPEG storage and attempts each generation's
decode/upload once. Authentication prevents undetected modification but does
not hide the JPEG from an observer on the LAN. The same confidentiality
limitation applies to teacher-screen snapshot broadcast.

## UDP video authentication

The datagram layout is:

```text
[44-byte video header][16-byte authentication tag][H.264 fragment]
```

The tag is a truncated HMAC-SHA256 over a video-specific domain label, the
serialized header, and payload. Verify it before packet-loss accounting or
frame reassembly; otherwise spoofed packets can corrupt both metrics and video.

One client-derived session key cannot authenticate a shared multicast stream.
The server therefore needs a random video group key, distributed individually
over each authenticated control session. Rotate the group key when membership
changes or according to policy, and allocate a new `stream_id` and sequence
baseline for the rotated stream.

## Reassembly ordering

The required receive order is:

```text
validate datagram length
  -> decode fixed header
  -> select authenticated stream/key
  -> verify video HMAC
  -> packet-loss tracker
  -> frame reassembler
  -> decoder/jitter buffer
```

Neither `PacketLossTracker` nor `FrameReassembler` auto-selects a stream from an
untrusted first datagram. Both must be reset from authenticated control-plane
metadata before UDP reception begins.

## Exam answer recovery

Exam answer events use a separate per-session sequence and SHA-256 hash chain.
The server's append-only `AnswerJournal` is authoritative: it validates the
chain, flushes each record before acknowledging it, and can recover only an
incomplete tail after a crash. The client keeps a bounded
`exam-answer-outbox.bin` retry buffer protected by machine-scope DPAPI. The
outbox is not a source of truth and an entry is removed only after the matching
event hash has been durably acknowledged by the server.

The browser and native bridge validate package ID, package digest, client
identity, session, candidate, question revision, sequence, size, and state
hashes before forwarding events. State responses are bounded and chunked; a missing or conflicting hash
causes manual review rather than an automatic overwrite. The outbox must live
on the explicitly approved client persistence boundary when UWF or third-party
 freezing is used. Exam packages, the server journal, exports, and grading data
 must remain on persistent server storage.

Deployment staging is performed by
`packaging/stage-exam-package.ps1` (installed under
`docs/deployment`). It holds the archive open with write/delete sharing
denied, requires archive and unpacked-tree SHA-256 pins, validates the supported
manifest schema and every declared local asset, rejects traversal,
case-insensitive duplicates, file/directory collisions, links, reparse points,
oversized or high-ratio entries, and publishes only through a same-volume
`MoveFileExW` rename with `MOVEFILE_WRITE_THROUGH` and without
`MOVEFILE_REPLACE_EXISTING`, preserving any existing destination. A detached CMS/PKCS#7
`manifest.p7s` and a pinned publisher certificate thumbprint are required by
default. The helper uses a per-package publication lock and revalidates an
existing content directory and sidecar after any move collision; it never
blindly replaces a concurrent publisher's bytes. The development-only
`-AllowUnsigned` and `-AllowNonElevatedTest` switches are never valid for a
school deployment. The strict content metadata sidecar is outside the package
tree, so it cannot silently change the digest checked by `ExamHost`; if its
commit fails after a new directory is published, the helper removes only that
newly verified directory and cleans its temporary files.

The current control plane now keeps a server-side active-exam context for each
authenticated client. Answer events and state requests are accepted only when
their client ID, package ID, package digest, session ID, and candidate ID
exactly match the context issued by `start_exam`; the context survives an
ordinary reconnect and is removed by an authenticated `stop_exam` or server
shutdown. The native client also checks the server-issued package ID against the
manifest before opening WebView2. This prevents a
client from switching journals by changing fields in a browser message, but it
does not prove that the teacher or deployment administrator authorized the
original start command. Until a signed, auditable instructor/deployment
authorization workflow is implemented, answer recovery must not be used as the
sole integrity control for an official assessment.

The reboot-to-restore work now has a standalone maintenance-intent contract.
It uses a deployment-administrator credential view and a dedicated
HMAC-SHA-256 domain rather than the teacher/control-channel MAC domain. Its
canonical bounded payload authenticates the target client, unique intent ID,
nonce, exact expected restore state, policy revision, validity window, and a
fixed operation-specific parameter variant. Local authorization rejects stale,
future, mistargeted, state-mismatched, replayed, and over-capacity requests.
Only that successful path returns an opaque capability containing a copy of the
verified intent. This code is not registered as a network command and cannot
execute WMI, processes, reboots, or UWF changes. Deployment-key provisioning,
durable replay state across reboot, technician confirmation, isolated helper
IPC, audit, and independent review remain mandatory before any mutation path
is enabled.

## Remaining blockers

- The unified installer and diagnostics helper do not apply Task Manager,
  Command Prompt, Control Panel, or drive-visibility policy. Production
  student-account hardening therefore still requires centrally managed Group
  Policy or a separately reviewed target-SID-aware deployment mechanism.
- Local service-agent IPC rejects remote clients and verifies that the pipe peer
  is the installed `nstu-agent.exe` in an active interactive session. It still
  lacks a per-launch authenticated bootstrap and heartbeat. Add both before
  treating deliberate same-user launch racing or suspension of the genuine
  agent as fully mitigated.
- Membership-driven video group-key generation, rotation, and distribution wired
  into live stream startup and UDP reception.
- Confidentiality: HMAC authenticates but does not encrypt screen content.
  AES-GCM group encryption or an equivalent design is required where LAN users
  must not be able to view captured traffic.
- Exam authorization: the active context binds the authenticated client to the
  exact package ID, package digest, session, and candidate for the lifetime of
  a started exam. It is still not an instructor/deployment authorization record; add
  signed, replay-resistant bindings and audit them before production assessment
  use.
- Exam package staging: the host revalidates the content digest before mapping,
  checks parent reparse points, and loads only the packaged WebView2 loader.
  The offline deployment stager now supplies bounded ZIP extraction and
  content-addressed publication. Production certificate provisioning,
  chain/revocation policy, signed fixtures, and handle-pinned host I/O still
  require validation before official assessments.
- Independent protocol review and fuzzing before deployment.
