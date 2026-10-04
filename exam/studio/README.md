# NSTU Exam Studio

**NSTU Exam Studio** is a browser-based, zero-dependency visual authoring tool designed for non-technical teachers and examiners to create, preview, and package computer-based assessments for NSTU.

---

## Key Features

1. **IELTS Dual-Pane Layout**:
   - **Left Screen (Stimulus)**: Reading passages with automatic `[Paragraph A]`, `[Paragraph B]` section tagging, audio players for listening tests, or PDF documents.
   - **Right Screen (Questions & Answers)**: Interactive question list with timer, point counters, and response surfaces.
   - **Interactive Splitter**: Resizable divider that lets candidates adjust the split ratio on any screen size.

2. **Drag-and-Drop Matching (IELTS Style)**:
   - Token Bank tray with draggable heading/word chips.
   - Drop Target slots with smooth snap-in animations.
   - Click-to-place alternative for laptop trackpads and touch devices.
   - Reversible chip assignment with one-click return to tray.

3. **Complete Question Types**:
   - **Matching / Gap-Fill** (Drag & Drop)
   - **Reading Comprehension** (Passage + Multiple Choice)
   - **Multiple Choice** (Single/Multi-select)
   - **Short Answer** (Text input with placeholder)
   - **Essay / Academic Writing** (Live word count tracker)
   - **Listening Comprehension** (Embedded audio track playback)

4. **100% Offline & Private**:
   - Runs locally in Microsoft Edge, Google Chrome, or any modern web browser.
   - Requires **zero external servers, zero Node.js / npm packages, and zero internet connection**.
   - Test questions and materials never leave the teacher's machine.

5. **One-Click `.nstuexam` Package Export**:
   - Validates the manifest against `manifest.schema.json`.
   - Packages `manifest.json`, the student kiosk web runner (`exam/web/`), and media assets into an `.nstuexam` container using browser-native ZIP encoding (`zip.js`).
   - Automatically computes:
     - **Archive SHA-256**
     - **Canonical Content SHA-256** (strictly matching NSTU's C++ `ExamHost` specification)
   - Displays the ready-to-run PowerShell command for `stage-exam-package.ps1`.

---

## How to Run

1. Open `exam/studio/index.html` directly in your browser:
   ```text
   file:///C:/Program Files/NSTU/exam-studio/index.html
   ```
   *(Or double-click `index.html` in Windows Explorer).*

2. **Authoring Mode**:
   - Set the assessment title, subject, and duration.
   - Add questions using the **✚ Add** dropdown.
   - Configure reading passages, audio files, and answer options.

3. **IELTS Live Preview**:
   - Click **👁️ IELTS Live Preview** in the top bar to test the candidate experience with live drag-and-drop, timer simulation, and split-screen scrolling.

4. **Exporting**:
   - Click **📦 Export .nstuexam**.
   - Verify the green validation checklist and download your `.nstuexam` bundle.
   - Copy the generated staging command to deploy to student client machines:
     ```powershell
     & "$env:ProgramFiles\NSTU\docs\deployment\stage-exam-package.ps1" `
       -ArchivePath "C:\Path\To\your-exam.nstuexam" `
       -PublishRoot "$env:ProgramData\NSTU\exams\packages" `
       -ExpectedArchiveSha256 "<archive-hash>" `
       -ExpectedContentSha256 "<content-hash>"
     ```
