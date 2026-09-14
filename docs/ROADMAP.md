# Current Task Log and Roadmap

## Completed in the experimental MVP

- [x] Modular CMake build for common, video, client, server, and tests.
- [x] Local-only memory, dump, trace, capture, and build exclusions.
- [x] Versioned binary protocol and bounded incremental TCP parser.
- [x] Winsock TCP/IOCP and UDP multicast foundations.
- [x] Multicast-to-unicast fallback state machine.
- [x] Reorder-aware packet-loss estimator and server health hysteresis.
- [x] Mutual HMAC handshake, replay cache, control/video MAC primitives.
- [x] Live blocking TCP handshake and authenticated control channel.
- [x] Fixed connection preamble for early role/version/identity filtering.
- [x] DPAPI machine-scoped secret storage with restrictive ACL and atomic replace.
- [x] In-process key enrollment, monotonic rotation, revocation, and zeroization.
- [x] Bounded unauthenticated handshake rate limiter with temporary blocking.
- [x] Unified NSIS role-selecting installer with automatic client service
      registration, recovery policy, reboot activation, conflict checks, and
      package-owned uninstall handling.
- [x] Teacher-focused server UI with a responsive latest-snapshot wall,
      adjustable 5-10 second interval, health summary, filtering, focused
      telemetry, chat and controls; lightweight Native Win32 client chat shell.
- [x] Runtime English/Vietnamese localization, Vietnamese font glyph coverage,
      light/dark dashboard themes, and a resilient native server tray icon with
      hide, restore, taskbar-recreation, and exit handling.
- [x] Bounded frame reassembly, deadlines, duplicate/conflict handling.
- [x] Desktop Duplication capture and device-loss reporting.
- [x] GPU BGRA-to-NV12 conversion.
- [x] Hardware H.264 MFT configuration and asynchronous event handling.
- [x] Service/session-agent separation, DACL helpers, named pipe, tray, overlay.
- [x] Dear ImGui/D3D11 server shell and thread-safe client state registry.
- [x] Empty-by-default server registry with no demonstration client injection.
- [x] IOCP completion-port create, socket association, wait, and post wrappers.
- [x] Unit/integration tests and Windows CI.
- [x] Bilingual deployment-facing README with installation, network, licensing,
      Deep Freeze, and honest MVP-status guidance.

## Computer-based assessment

- [x] Package-contained exam UI with multiple-choice, short-answer, essay,
      listening, reading, PDF/audio references, timer, autosave, bilingual
      display settings, and local JSON response export.
- [x] Authenticated answer events with per-session sequence numbers, hash-chain
      validation, bounded state responses, and framed diagnostic exports.
- [x] Durable client retry outbox with machine-scope DPAPI protection,
      migration, quota enforcement, ACK validation, and crash-tail-safe server
      journal persistence.
- [x] Implement the optional native WebView2 host with pinned digest/path
      validation, strict local navigation, user-mode kiosk safeguards,
      process-failure handling, and a tested agent/service bridge.
- [x] Bind the authenticated exam start to the manifest package ID (EXS2),
      carry that identity into the client host, and reject package-ID
      mismatches in browser recovery, answer events, and state requests.
- [x] Add deployment-owned package staging with bounded ZIP extraction,
      traversal/reparse/duplicate/path-collision rejection, archive and
      content-digest pins, content-addressed publication, and a detached
      CMS/PKCS#7 publisher-thumbprint gate. The
      `packaging/stage-exam-package.ps1` helper is covered by an
      adversarial disposable test and is shipped in the deployment
      documentation payload. The native host still rechecks the pinned package
      digest immediately before mapping, rejects parent reparse points,
      confines navigation to the validated web root, and requires the packaged
      loader plus a runtime probe.
- [ ] Validate the staging helper, publisher chain/revocation policy,
      `WebView2Loader.dll`, and the WebView2 Runtime across the supported
      Windows images with production certificates and signed fixtures.
- [ ] Add instructor/deployment authorization for package, session, and
       candidate bindings; authenticate and audit every official assessment.
- [ ] Roll out direction-bound control MACs with an authenticated capability or
       protocol-version negotiation; retain v1 compatibility until the fleet
       is upgraded.
- [ ] Add server-side grading, review, sealed exports, and recovery drills
      covering client resets, offline replay, and school-approved retention.
- [ ] Run the exam-specific production gates in
      `PRODUCTION_VALIDATION.md` before enabling official assessments.

## Completed production engineering

- [x] Persisted, versioned DPAPI keyring with active keys and revoked-ID
      tombstones, plus replay-resistant authenticated bootstrap enrollment.
- [x] Live IOCP `AcceptEx`/`WSARecv`/`WSASend` dispatcher with bounded connection
      capacity, source rate limiting, audit events, and a 64-client integration
      test.
- [x] Authenticated server control plane and reconnecting service client with
      status, heartbeat, lock/unlock, chat, stream, stop, and keyframe commands.
- [x] Live service-agent named-pipe routing and status reporting.
- [x] Server dashboard actions connected to authenticated client sessions.
- [x] Bounded authenticated JPEG snapshots from clients to the dashboard, with
      newest-frame queue replacement, shared immutable payloads, bounded
      JPEG-only WIC decode, negative generation caching, and D3D11 preview
      textures.
- [x] Authenticated normalized annotation strokes rendered by a transparent
      click-through client overlay, plus clear-overlay control.
- [x] Bounded teacher-screen snapshot broadcast to authenticated clients, with
      deterministic lock/broadcast/annotation window ordering.
- [x] Installer setup validation for elevation, supported Windows, data-root
      ACL filesystem, firewall state, ports, and optional standard-user
      autologon guidance without credential storage.
- [x] Authenticated video packetizer, jitter buffer, bounded NACK policy, and
      keyframe scheduler with deterministic reordering/loss tests.
- [x] Duplication/converter/encoder recovery orchestration with bounded
      exponential retry and hardware keyframe control.
- [x] Authenticode hooks, a certificate-gated production workflow, protected
      configurable data roots for Deep Freeze thaw spaces, and deployment
      validation scripts.
- [x] Reproducible benchmark and multicast-matrix tooling plus a production
      evidence record.
- [x] Add opt-in server diagnostic collection with a 64-event in-memory bound,
      local privacy filtering, independent error-prompt control, a complete
      review/copy UI, and manual-only public GitHub Issue submission. No event
      payload is persisted or uploaded automatically.

## Optional continuous-video work

Decision (2026-09-09): keep this work deferred. The production monitoring path
is periodic JPEG snapshots because independently streaming more than 50 client
feeds creates avoidable switch, CPU, decoder, and failure-domain pressure.
Do not advertise, enable, or make H.264 a prerequisite for classroom use.

- [ ] Connect encoded UDP send/receive, authenticated group-key rotation, H.264
      decode, and D3D11 continuous-preview textures. The production monitoring
      path is snapshot-first; this work is required only before advertising or
      enabling continuous H.264 mode.

## Managed reboot-to-restore work

- [x] Document a UWF-first architecture, Windows edition gate, threat model,
      persistence boundary, servicing/recovery lifecycle, and staged test plan.
- [x] Implement the read-only SKU/optional-feature/provider/current-next UWF
      capability probe, including an explicit `probe unavailable` state when a
      supported image cannot be classified without mutation.
- [x] Extend the read-only capability probe to protected volumes, exclusion
      counts without path disclosure, overlay configuration/consumption, and
      recent UWF event health, with bounded queries and explicit partial or
      truncated-result warnings.
- [x] Keep this extension local, client-only, and read-only: it does not
      implement or authorize UWF mutation, and the server role skips UWF so its
      data remains persistent.
- [x] Add the standalone contract for a separate deployment-administrator
      credential and HMAC-authenticated, replay-resistant, strictly typed
      maintenance intents. The canonical codec binds intent/nonce IDs, target
      client, exact expected restore state, policy revision, a 15-minute
      maximum validity window, and bounded operation-specific parameters. It
      returns an opaque capability only after successful authorization and is
      deliberately not connected to the teacher control channel, UWF WMI,
      reboot, UI, or helper execution; durable replay/transaction state
      remains part of the next milestone.
- [ ] Implement the typed UWF WMI controller behind a lab-only feature flag,
      with local confirmation for first activation, recovery, and decommission.
- [ ] Add bounded persistent maintenance transactions, overlay monitoring,
      update/servicing integration, post-boot verification, quarantine, and an
      offline recovery runbook without automatic reboot loops.
- [ ] Test UWF warning/critical events separately from maximum-overlay OS
      automatic-restart behavior, including power loss, overlay exhaustion, and
      a guarantee that network or power failure cannot leave servicing silently
      unprotected.
- [ ] Complete persistent-VM mutation tests, the exact LTSC image matrix,
      i5-6400/8 GB physical validation, at least 50 reset cycles, independent
      security review, and a non-production school pilot before opt-in release.

Windows Pro and Home do not support Microsoft UWF. On those editions this work
must remain audit-only; the supported fallback is an edition upgrade, a
separately managed third-party product, or centrally managed reimaging. NSTU
will not implement a script-based or custom-kernel imitation of UWF.

## Managed LTSC update work

- [x] Document the nine-month LTSC/Deep Freeze maintenance model and provide a
      non-destructive hash, staging, health-check, and rollback trial harness.
- [ ] Implement a signed release manifest and authenticated outbound update
      check in `nstu-service`, disabled by default behind administrator policy.
- [ ] Add resumable protected staging, role/version/OS validation, anti-downgrade
      checks, and reboot-bound package activation.
- [ ] Add server fleet status, pilot/wave controls, post-reboot health reports,
      automatic rollback, and audit retention without screen or secret data.
- [ ] Validate the entire thaw, update, reboot, health-check, rollback, and
      refreeze sequence against each supported Windows LTSC and Deep Freeze
      edition before enabling school deployments.

## Remaining production release gates

- [ ] Run the production workflow with the real code-signing certificate and
      verify signatures/timestamps on the unified installer and installed
      binaries.
- [ ] Validate the conservative Deep Freeze install/uninstall/data-root behavior
      against every edition and version the project claims to support.
- [ ] Execute and attach evidence for the 50-client snapshot soak, authenticated
      TCP snapshot-network/uplink capacity, Windows build matrix, Intel driver
      matrix, and CPU/RAM/network benchmarks.
- [ ] If continuous H.264 is enabled in a future release, execute and attach the
      separate multicast switch matrix and forced unicast-fallback evidence
      before advertising that mode.
- [ ] Complete independent protocol review and fuzzing. Where the LAN threat
      model requires screen confidentiality, add authenticated encryption before
      deployment; the current video format authenticates but does not encrypt.
- [ ] Validate the optional diagnostic sanitizer and consent/deletion UI with
      school-approved test fixtures before considering any client/fleet
      reporting. Session 0 clients must not publish diagnostics or infer consent.

## Context note

The repository now contains the control, enrollment, persistence, bounded
snapshot, annotation, teacher-broadcast, packetization, and recovery
implementations that were previously roadmap stubs. A green build still does
not establish production reliability, certificate trust, or compatibility with
heterogeneous school hardware. The evidence gates above remain
release-blocking.
