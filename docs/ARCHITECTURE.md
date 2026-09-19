# Architecture

## Process model

```text
nstu-server.exe       ImGui/D3D11 administration UI and client state
nstu-service.exe      LocalSystem Session 0 service, policy and lifecycle
nstu-agent.exe        Logged-in-user tray and fullscreen overlay
```

The unified installer registers `nstu-server.exe` in the machine `Run` key so
the teacher UI starts at interactive sign-in. It is deliberately not a Windows
service: D3D11, ImGui, and notification-area ownership remain in the signed-in
user session. The uninstaller removes the startup registration.

The service and agent are separate because Windows services cannot interact
directly with the logged-in user's desktop. IPC uses a local named pipe whose
DACL permits SYSTEM, Administrators, and the interactive user and rejects
remote clients. After a connection, the service obtains the peer PID from the
pipe and accepts it only when the process image is the installed
`nstu-agent.exe` in an active interactive session.

The installer registers the service explicitly under `LocalSystem`. The
service launches the agent with the active user's token through
`WTSQueryUserToken` and `CreateProcessAsUserW`; the agent is not elevated to
SYSTEM. The service DACL prevents a standard classroom account from stopping or
deleting the service, and the service attempts a bounded agent relaunch after
an established pipe disconnect. This protects the privileged core while
preserving Windows' Session 0 boundary. It does not override a local
administrator or kernel-level security product. The disposable and reboot test
procedures are documented in `docs/VM_TESTING.md`.

The service loads its client identity and PSK from a machine-scoped DPAPI
configuration, reconnects to the teacher server, completes the mutual HMAC
handshake, and forwards authenticated commands to the active-session agent.
Agent status is returned over the same pipe and then reported to the server.
The stored server IPv4 address is a reconnect cache, not an identity anchor.

## Computer-based assessment path

The exam surface is a local, package-contained web UI under `exam/web/`. The
native `ExamHost` in `client/src/exam_host.cpp` is implemented in
`nstu-agent` and runs on the agent's UI/STA thread. In builds configured with
`NSTU_ENABLE_WEBVIEW2_HOST=ON`, it receives an already-unpacked package root,
validates the manifest and pinned SHA-256 digest, rechecks the package digest
immediately before virtual-host mapping, creates a topmost full-screen
Win32 window, maps the package to `https://nstu.exam/`, injects the
identity-only, package-bound context, and forwards validated browser messages through the
agent/service boundary. The host also blocks navigation outside the local
virtual host, package metadata outside the validated web root, popup windows,
developer tools, default context menus, and zoom;
it keeps the window foreground and suppresses common user-mode shortcuts while
the exam is active.

`nstu-service` remains the authenticated transport and durability boundary; it
must not create a desktop window, and the server never hosts the student's
browser. The service response path publishes bounded `ExamBridge` messages,
which the host drains on the agent UI thread. Builds with
`NSTU_ENABLE_WEBVIEW2_HOST=OFF` retain that bridge and the protocol tests for
offline/CI use, but intentionally fail closed when an exam is started. A
WebView2-enabled deployment requires the signed `WebView2Loader.dll` beside
`nstu-agent.exe` and a compatible WebView2 Runtime on the client. The agent
loads only that packaged loader and probes the installed Runtime before launch;
it does not use a system-DLL fallback.

Each native exam browser profile is derived from the package digest, provisioned
client identity, and authenticated session ID under the interactive user's
approved profile root. The fixed virtual origin therefore cannot carry a service
worker or cache from another package/context. A server-supplied profile path is
accepted only when it matches that locally derived path exactly.

```text
.nstuexam package (server-owned; release signing required for production)
  -> deployment-owned per-session staging and extraction with archive/content
     pins, detached publisher-signature validation, and atomic publication
  -> native ExamHost manifest/path/digest validation
  -> WebView2 exam surface in nstu-agent
  -> authenticated exam_answer_event / exam_state_request
  -> agent ExamBridge -> LocalSystem service
  -> DPAPI client outbox (retry only)
  -> authenticated TCP control channel
  -> append-only server AnswerJournal (authoritative)
```

The server keeps packages, answer journal records, state exports, and grading
data outside any UWF or Deep Freeze boundary. Only the bounded client retry
outbox may be placed in the approved client persistence location. A server
reconnect response carries the last event hash and chunk metadata so the
browser can rebase pending events without guessing over a conflict.

The server shell presents two operational modes. `Room screens` uses a
responsive grid for all visible clients, health summaries, search/filter, and a
5-10 second snapshot interval. `Selected client` provides focused telemetry, a
16:9 snapshot preview, lock/unlock, snapshot control, annotation, and chat. The
registry starts empty and does not inject demonstration client records. The
client agent uses standard Win32 LISTBOX/EDIT/BUTTON controls for a small chat
window; no UI framework is added to the client.

The server UI has runtime English/Vietnamese localization with Segoe UI's
Vietnamese glyph range and a light/dark semantic palette. It owns a native
notification-area icon. Minimize and close hide the window while the control
plane remains active; the tray menu restores the window or exits the process.
Windows recreating the taskbar causes the icon to be registered again.

## Snapshot path

```text
Client primary screen (GDI)
  -> bounded downscale
  -> WIC JPEG encode, maximum 60 KiB
  -> service-agent named pipe
  -> authenticated TCP control channel
  -> newest-frame client registry slot
  -> WIC decode and D3D11 snapshot texture
```

Snapshots are scheduled every 5-10 seconds. The service replaces an older
queued snapshot with the newest one instead of building a stale backlog. The
same bounded JPEG codec carries teacher-screen snapshots in the opposite
direction. Normalized annotation strokes are authenticated control commands and
are rendered by a transparent, click-through topmost window in the student's
interactive session. The lock window is explicitly kept above broadcast and
annotation windows.

The server accepts snapshot metadata no larger than 480x270 and a JPEG payload
no larger than 60 KiB. WIC verifies that the payload is actually JPEG and that
its decoded dimensions match the authenticated metadata before pixels are
allocated. Published JPEG bytes are immutable shared storage, so copying the
client registry for the render loop does not copy the image payload. The UI
uploads a generation to a D3D11 shader-resource texture at most once; a failed
decode or texture upload is negatively cached until a newer generation arrives.
The cache is explicitly invalidated when the graphics device is lost or
recreated, so stale shader-resource views cannot block a subsequent device
initialization. This keeps malformed or unsupported frames from causing a
per-frame retry loop while preserving a clean cache boundary for a future
device-recovery path.

## Optional continuous video path

```text
DXGI Desktop Duplication (BGRA texture)
  -> D3D11 Video Processor (NV12 texture)
  -> Media Foundation hardware H.264 MFT
  -> application packetization
  -> UDP multicast or UDP unicast fallback
```

Raw frames remain in GPU memory. The compressed H.264 access unit becomes
CPU-visible for Winsock packetization; therefore the architecture avoids raw
frame copies but is not literally copy-free through the NIC.

## Control path

TCP frames are length-prefixed and contain explicitly serialized little-endian
headers. No C++ struct memory layout is placed directly on the wire. Parsers
enforce payload and buffered-byte limits before allocating.

Control authentication uses a nonce-based mutual HMAC handshake, derived
session keys, strict per-direction command sequences, and replay protection.
See `docs/SECURITY.md`.

Each TCP connection begins with a fixed-size role/version preamble carrying the
client identity hint and key ID. It is a cheap admission filter and does not
replace cryptographic authentication.

The server uses a bounded IOCP dispatcher with posted `AcceptEx`, one outstanding
`WSARecv` per connection, serialized queued `WSASend`, source admission limits,
and structured audit callbacks. The raw receive path feeds an asynchronous
preamble/handshake state machine before authenticated frames can update the
registry or execute dashboard commands.

## Authenticated server endpoint recovery

The server binds an authenticated UDP discovery responder to the same numeric
port as the TCP control listener (`47001` by default). When the cached endpoint
cannot connect or complete mutual authentication, the client sends a fixed
104-byte discovery request to the directed broadcast addresses of active IPv4
LAN adapters. Loopback and tunnel adapters are excluded from automatic target
selection; tests and controlled deployments can provide explicit targets.

```text
try cached IPv4 endpoint
  -> on failure, send HMAC-authenticated UDP discovery request
  -> verify response client ID, key ID, nonce, timestamp, port, and HMAC
  -> connect to the response source address
  -> complete the normal mutual TCP handshake
  -> atomically replace the DPAPI-protected endpoint cache
  -> refresh the non-secret registry hint used by login diagnostics
```

Requests and responses use separate HMAC domain labels. The responder resolves
the requesting client's enrolled PSK, rejects stale or replayed nonces, and
rate-limits invalid requests by source address. A UDP response only supplies a
candidate endpoint: the client does not cache it until the existing mutual TCP
handshake succeeds. An IPv4 address or hardware MAC address is therefore never
used as proof of server identity.

Discovery is deliberately link-local in deployment scope. Routers normally do
not forward IPv4 broadcasts, so separate VLANs require a stable address,
controlled DHCP/DNS, or a future authenticated relay. After a control-session
disconnect, the service clears transient exam, lock, stream, snapshot,
teacher-broadcast, annotation, and remote-input state, then retries after 3-6
seconds of cryptographic jitter to avoid a classroom reconnect storm.

## Packet loss

Video protocol v2 includes a monotonic packet sequence independent of frame and
fragment identifiers. Receivers use a bounded reorder window and only confirm a
loss after the missing sequence leaves that window. See `docs/PACKET_LOSS.md`
for reporting, hysteresis, stream-reset, and fallback rules.

## Known limitations

- The dashboard and teacher broadcast use periodic JPEG snapshots, not
  continuous video. The authenticated packetizer, reassembler, jitter buffer,
  NACK policy, and recovery pipeline exist, but encoded UDP sockets, group-key
  rotation, H.264 decoding, and ImGui continuous-preview textures are not yet
  connected end to end.
- Video group-key payload codecs exist, but membership-driven key generation and
  distribution are not yet wired to stream startup.
- Video HMAC provides integrity and source authentication, not confidentiality.
- Long-duration rate control, repeated device loss, multicast/unicast behavior,
  and 50-client resource use still require the hardware validation matrix in
  `PRODUCTION_VALIDATION.md`.
- The native WebView2 host, answer bridge, and answer journal are implemented in
  the opt-in WebView2 build. Target-machine WebView2 Runtime/loader testing,
  production publisher certificate-chain/revocation policy, instructor
  package/session authorization, grading workflow, and full proctoring remain
  production gates.
- The exam window provides user-mode kiosk safeguards only. Secure-desktop,
  local-administrator, and kernel-level escape paths require an OS-managed
  policy and are outside this agent host's boundary.
- The current version-2 authenticated exam-start payload carries the required
  manifest package ID plus absolute UTF-8 package,
  web, and user-data roots. The agent canonicalizes and bounds them, rejects
  parent reparse points, rechecks the content digest before mapping, and binds
  the browser profile locally. Deployment staging now performs bounded ZIP
  extraction, path/reparse/duplicate checks, archive and content digest pinning,
  detached CMS publisher-signature validation, and atomic publication. Remaining
  release gates are validation on each supported Windows image, production
  certificate-chain/revocation policy, and instructor/deployment authorization.
  The native host intentionally does not unpack archives or repeat the
  deployment signature check; it rechecks the pinned content digest and confines
  navigation to the validated package root.
