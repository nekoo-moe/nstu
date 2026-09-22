# Optional Diagnostics and Public Reports

[English](TELEMETRY.md) | [Tiếng Việt](TELEMETRY.vi.md)

NSTU Server includes an opt-in diagnostic collection mode for troubleshooting
graphics, snapshot decoding, and other bounded server errors. It is disabled by
default and is not a remote telemetry service.

## Consent and data flow

1. A teacher or technician explicitly enables **Collect privacy-filtered
   diagnostics locally** in `Settings`.
2. NSTU keeps at most 64 recent sanitized events in process memory.
3. If **Prompt when an error report is ready** is enabled, the first error in a
   collected batch opens the report-review popup. Clearing the batch or
   re-enabling the prompt rearms it, preventing a repeating graphics error from
   reopening the popup every frame. Otherwise the operator can open the report
   from `Settings` or `Diagnostics`.
4. The operator reviews the complete Markdown report and can copy it.
5. **Copy and open GitHub** copies the text and opens the public GitHub Issue
   form. The operator must paste, review, and submit it manually.

NSTU does not embed a GitHub credential, create an issue in the background, or
transmit the report to an NSTU endpoint. Closing the application discards the
in-memory events. Disabling collection clears them immediately.

The existing bounded graphics log used by the local `Diagnostics` window and
startup failure message remains available even when public-report collection is
off. It is process-local, is not included in a shareable report while collection
is disabled, and is discarded when NSTU exits.

## Included fields

The report uses a strict application-owned field list:

- NSTU application version and build channel;
- D3D11 device mode, graphics adapter/vendor, and feature level;
- Desktop Duplication availability and the number of registered hardware H.264
  encoders;
- configured snapshot interval;
- a bucketed client count (`0`, `1-10`, `11-25`, `26-50`, or `51+`);
- up to 64 recent internal diagnostic events after local sanitization.

The report intentionally excludes exact client counts above zero to reduce
environment fingerprinting.

## Excluded data

Do not add any of the following to a public report:

- IP addresses, MAC addresses, client IDs, host names, user names, or school
  names;
- local/UNC paths, credentials, enrollment secrets, tokens, or private keys;
- screenshots, screen contents, chat messages, or remote-control input;
- exam packages, questions, answers, candidate identity, grading data, or answer
  recovery journals;
- dumps or unrestricted application logs.

The sanitizer removes common forms of IP/MAC address, path, email, secret,
credential assignment, and numeric client-reference data. This is defense in
depth, not permission to collect arbitrary user content. Event producers must
still use controlled, non-user-authored diagnostic messages.

## Controls and storage

The server stores only the two consent choices under the interactive operator's
profile:

```text
HKCU\Software\NSTU\Server\TelemetryEnabled
HKCU\Software\NSTU\Server\TelemetryPromptOnError
```

Both values are `REG_DWORD`. The default for a missing value is disabled. No
event payload is written to the registry or disk.

## Current scope

This implementation is server-side only. A Session 0 client service must never
show consent on behalf of a student or silently publish machine diagnostics.
Any future fleet-reporting design requires administrator policy, authenticated
transport, a documented retention period, aggregation on the persistent server,
and a separate privacy/security review before it can be enabled.

## Reporting safely

GitHub Issues are public. Before submitting:

1. read every report line;
2. remove anything identifying a person, device, school, or network;
3. describe reproduction steps without names or addresses;
4. attach no screenshot, dump, exam material, or unrestricted log;
5. submit only if the school or organization permits public disclosure.

Security vulnerabilities or reports containing sensitive data must not be filed
through the public diagnostic template. Use a private maintainer channel or
GitHub private vulnerability reporting when available; see
[SECURITY.md](SECURITY.md).

## Activity audit log (distinct from telemetry)

NSTU also keeps an operational **activity audit log**, which is not the same as
the opt-in public diagnostics described above and not the same as the exam
answer journal. The audit log records that NSTU activity happened - enrollment,
managed-mode changes, UWF fleet operations, exam authorization, session and
security events - with a category, severity, component, action, result, and
bounded sanitized detail. It never records exam questions, answer bodies,
screenshots, chat or remote-input contents, credentials, secrets, keys, SAS
codes, tokens, raw file paths, or raw network identifiers: every field is passed
through the same public-text sanitizer plus a hard denylist before it is written
or sent.

Client activity is spooled with a bounded, drop-oldest queue and uploaded to the
server in small, rate-limited, sequence-acknowledged chunks. The server persists
records centrally to a rotating newline-delimited sink. This is always-on
operational record keeping, separate from the consent-gated diagnostics above.
