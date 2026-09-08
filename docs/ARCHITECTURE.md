# Architecture

## Process model

```text
nstu-server.exe       ImGui/D3D11 administration UI and client state
nstu-service.exe      LocalSystem Session 0 service, policy and lifecycle
nstu-agent.exe        Logged-in-user tray and fullscreen overlay
```

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
