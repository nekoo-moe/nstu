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
4. Handle `window.chrome.webview` messages with `{ type: "exam_submit" }` and
   persist the response on the server using the authenticated control channel.
5. Restore the last accepted answer state after a client reconnects. The
   browser's localStorage is only a crash-recovery cache, not the authoritative
   record.

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
