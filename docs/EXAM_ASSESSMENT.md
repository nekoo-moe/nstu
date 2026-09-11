# Computer-Based Assessment

The first exam UI is a self-contained web surface under `exam/web/`. It is
designed to run from a local `.nstuexam` package inside WebView2 or another
embedded browser. It has no CDN or runtime download dependency.

## Package boundary

An `.nstuexam` package is a ZIP container with a UTF-8 `manifest.json`, local
media, and optional PDF references:

```text
ielts-sample-01.nstuexam
|-- manifest.json
|-- documents/
|   `-- reading.pdf
`-- media/
    `-- listening-01.mp3
```

The manifest is validated against
`exam/schema/manifest.schema.json`. Supported question types are
`multiple_choice`, `short_answer`, `essay`, `listening`, and `reading`.
Question and media URLs must resolve inside the unpacked package directory;
the host must reject path traversal and network URLs before creating a WebView2
navigation request.

## Host integration

The WebView2 host should:

1. Unpack a signed package into a per-session temporary directory.
2. Validate the manifest, media paths, duration, question count, and package
   digest before navigation.
3. Expose the manifest as `window.NSTU_EXAM_MANIFEST` and navigate to the local
   `exam/web/index.html` file.
4. Handle the WebView2 answer-recovery messages described below and persist
   them on the server using the authenticated control channel.
5. Restore the last accepted answer state after a client reconnects. The
   browser's localStorage is only a crash-recovery cache, not the authoritative
   record.

## Answer recovery bridge

Before navigation, the host should inject an identity-only context. The exam
manifest must never be allowed to select the client or session identity:

```js
window.NSTU_EXAM_CONTEXT = {
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
The event remains in a bounded browser queue until the host sends an
`exam_answer_ack` with status `accepted` or `duplicate` and the matching
`eventHashHex`. A rejected or unavailable event is retained for retry and is
shown as a pending recovery state. On submit, `exam_submit` remains available
for the host's response-package workflow, and a durable `finalize` event is
queued after all answer events.

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
`lastEventHashHex`; the page merges that state and never overwrites newer
pending events. The native host must validate every identity, revision, size,
and hash-chain field before converting the message to the packed C++ protocol.

The page supports a PDF reference pane, listening audio, multiple-choice,
short-answer, essay, reading, timer, autosave, language selection, text-size
and contrast settings, keyboard navigation, and JSON response export. It does
not grade essays or accept arbitrary JavaScript from an exam package.

## Security and recovery requirements

- Keep exam packages and answer records on the persistent server; do not place
  them in a client path managed by UWF or third-party freezing software.
- Disable external navigation, downloads, clipboard injection, and developer
  tools in the exam WebView2 profile.
- Sign packages and pin the expected package digest before an exam starts.
- Treat the server's answer log as append-only and include package version,
  question revision, candidate identity, and event time.
- Close the exam through an authenticated server command or a local technician
  action. A client reboot must not erase the server-side response record.

The sample manifest is in `exam/examples/ielts-sample.json`. This UI is the
interaction layer; scoring rules, teacher authoring, proctor controls, and
server-side finalization remain separate milestones in the roadmap.
