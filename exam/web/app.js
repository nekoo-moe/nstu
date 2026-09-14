(function () {
  "use strict";

  const DEFAULT_MANIFEST = {
    id: "ielts-sample-01",
    title: "IELTS sample assessment",
    subject: "English language",
    candidate: "Practice candidate",
    durationSeconds: 45 * 60,
    documents: [],
    questions: [
      {
        id: "q1",
        type: "multiple_choice",
        points: 1,
        prompt: "Which sentence best summarizes the speaker's main point?",
        instruction: "Choose one answer.",
        options: [
          "The timetable is changing next month.",
          "The library is extending its evening opening hours.",
          "Students must return all books today.",
          "The library will move to another building."
        ]
      },
      {
        id: "q2",
        type: "short_answer",
        points: 1,
        prompt: "Complete the sentence with one word.",
        instruction: "The research workshop begins at ____ on Thursday.",
        answerPlaceholder: "Type one word"
      },
      {
        id: "q3",
        type: "essay",
        points: 8,
        prompt: "Some people prefer studying alone, while others learn better in a group. Discuss both views and give your own opinion.",
        instruction: "Write a clear response. The suggested length is 180–220 words.",
        answerPlaceholder: "Write your response here",
        wordLimit: 220
      },
      {
        id: "q4",
        type: "listening",
        points: 2,
        prompt: "Listen to the announcement and answer the question.",
        instruction: "What should visitors bring to the field trip?",
        audio: "",
        options: ["A printed map", "A reusable water bottle", "A university ID", "A rain jacket"]
      },
      {
        id: "q5",
        type: "reading",
        points: 2,
        prompt: "Read the passage and identify the author's claim.",
        passage: "Short, deliberate breaks help learners return to difficult material with more attention. The most useful break is long enough to reset focus but short enough to preserve the context of the task.",
        instruction: "Choose the statement that matches the passage.",
        options: [
          "Long breaks always produce better results.",
          "Breaks should be planned around the learner's attention.",
          "Difficult material should be avoided after a break.",
          "Learners should never interrupt a study session."
        ]
      }
    ]
  };

  const I18N = {
    en: {
      computerBasedAssessment: "Computer-based assessment", timeRemaining: "Time remaining",
      settings: "Settings", submit: "Submit", ready: "Ready", localSession: "Local session",
      questions: "Questions", downloadResponse: "Download response", previous: "Previous", next: "Next",
      pdf: "PDF", notes: "Notes", noPdf: "No PDF attached",
      noPdfDetail: "The exam package can provide a local PDF reference for this section.",
      assetUnavailable: "This exam asset is not available in the local package.",
      notesPlaceholder: "Private notes for this session", sessionSettings: "Session settings",
      displayAndLanguage: "Display and language", language: "Language", textSize: "Text size",
      highContrast: "High contrast", reduceMotion: "Reduce motion", done: "Done",
      submitAssessment: "Submit assessment", submitPrompt: "Submit your answers now?",
      keepWorking: "Keep working", multipleChoice: "Multiple choice", shortAnswer: "Short answer",
      essay: "Essay", listening: "Listening comprehension", reading: "Reading comprehension",
      question: "Question", point: "point", points: "points", candidate: "Candidate",
      saved: "Saved", unsaved: "Not saved", complete: "complete", unanswered: "unanswered",
      submitSummary: (answered, total) => `${answered} of ${total} questions answered.`,
      submitted: "Response package prepared.", exported: "Response downloaded.",
      chooseOne: "Choose one answer.", writeResponse: "Write your response below."
    },
    vi: {
      computerBasedAssessment: "Bài thi trên máy tính", timeRemaining: "Thời gian còn lại",
      settings: "Cài đặt", submit: "Nộp bài", ready: "Sẵn sàng", localSession: "Phiên cục bộ",
      questions: "Câu hỏi", downloadResponse: "Tải câu trả lời", previous: "Trước", next: "Sau",
      pdf: "PDF", notes: "Ghi chú", noPdf: "Chưa có PDF đính kèm",
      noPdfDetail: "Gói bài thi có thể cung cấp tài liệu PDF cục bộ cho phần này.",
      assetUnavailable: "Tài nguyên này không có trong gói bài thi cục bộ.",
      notesPlaceholder: "Ghi chú riêng cho phiên này", sessionSettings: "Cài đặt phiên",
      displayAndLanguage: "Hiển thị và ngôn ngữ", language: "Ngôn ngữ", textSize: "Cỡ chữ",
      highContrast: "Tương phản cao", reduceMotion: "Giảm chuyển động", done: "Xong",
      submitAssessment: "Nộp bài đánh giá", submitPrompt: "Nộp câu trả lời ngay?",
      keepWorking: "Tiếp tục làm", multipleChoice: "Trắc nghiệm", shortAnswer: "Trả lời ngắn",
      essay: "Bài luận", listening: "Nghe hiểu", reading: "Đọc hiểu", question: "Câu",
      point: "điểm", points: "điểm", candidate: "Thí sinh", saved: "Đã lưu", unsaved: "Chưa lưu",
      complete: "hoàn thành", unanswered: "chưa trả lời", submitSummary: (answered, total) =>
        `Đã trả lời ${answered}/${total} câu.`, submitted: "Đã tạo gói câu trả lời.",
      exported: "Đã tải câu trả lời.", chooseOne: "Chọn một đáp án.", writeResponse: "Viết câu trả lời bên dưới."
    }
  };

  // Native package inspection rejects malformed manifests before WebView2 is
  // opened. Keep the static-preview and failure paths defensive as well: a
  // partially injected manifest must not make the kiosk page throw while
  // rendering its initial question list.
  function normalizeManifest(value) {
    if (!value || typeof value !== "object" || Array.isArray(value)) {
      return DEFAULT_MANIFEST;
    }
    const questions = [];
    const seenQuestionIds = new Set();
    if (Array.isArray(value.questions)) {
      for (const item of value.questions.slice(0, 500)) {
        if (!item || typeof item !== "object" || Array.isArray(item)) continue;
        let id;
        try { id = item.id; } catch (_) { continue; }
        if (typeof id !== "string" || id.length === 0 || id.length > 128 ||
            seenQuestionIds.has(id)) continue;
        seenQuestionIds.add(id);
        questions.push(item);
      }
    }
    let durationSeconds;
    try { durationSeconds = Number(value.durationSeconds); }
    catch (_) { durationSeconds = Number.NaN; }
    if (!Number.isSafeInteger(durationSeconds) || durationSeconds < 0 ||
        durationSeconds > 86400) {
      durationSeconds = DEFAULT_MANIFEST.durationSeconds;
    }
    return { ...value, questions, durationSeconds };
  }

  const manifest = normalizeManifest(window.NSTU_EXAM_MANIFEST);
  // Native package validation normally guarantees these shapes. Keep the
  // static-preview and failure paths defensive as well: a malformed manifest
  // must produce an unavailable-question message, not a blank kiosk page.
  const manifestDocuments = Array.isArray(manifest.documents)
    ? manifest.documents : [];

  // The native WebView2 host maps the validated package root to a fixed local
  // origin and injects that origin before the document is navigated.  Resolve
  // manifest-owned media through that base instead of letting a package URL
  // escape to the network or to a different directory.  When the page is
  // opened without a host (for example, during static UI development), keep
  // relative paths working while applying the same traversal and scheme
  // checks.
  function normalizeAssetBase(value) {
    if (typeof value !== "string") return "";
    const raw = value.trim();
    if (!raw || raw.length > 2048 || /[\u0000-\u001f\u007f\\]/.test(raw)) return "";
    const Url = typeof URL === "function" ? URL :
      (window && typeof window.URL === "function" ? window.URL : null);
    if (Url) {
      try {
        const parsed = new Url(raw);
        if (parsed.protocol !== "https:" || parsed.username || parsed.password ||
            parsed.search || parsed.hash) return "";
        if (!parsed.pathname.endsWith("/")) parsed.pathname += "/";
        return parsed.href;
      } catch (_) { return ""; }
    }
    // A small fallback keeps the page testable in non-browser shims that do
    // not provide the URL constructor.  The native host always supplies an
    // absolute HTTPS URL, so reject anything less specific here.
    const match = /^https:\/\/([^/?#]+)(\/[^?#]*)?$/i.exec(raw);
    if (!match || /[@\\]/.test(match[1])) return "";
    const path = match[2] || "/";
    if (path.split("/").some((segment) => segment === "..")) return "";
    return `https://${match[1]}${path.endsWith("/") ? path : `${path}/`}`;
  }

  const examAssetBase = normalizeAssetBase(window.NSTU_EXAM_ASSET_BASE);

  function resolveExamAsset(value) {
    if (typeof value !== "string") return "";
    const raw = value.trim();
    if (!raw || raw.length > 4096 || /[\u0000-\u001f\u007f\\]/.test(raw) ||
        /%(?:2f|2F|5c|5C|23|3f|3F)/.test(raw) ||
        raw.includes("?") || raw.includes("#")) return "";
    let decoded;
    try { decoded = decodeURIComponent(raw); }
    catch (_) { return ""; }
    if (/[\u0000-\u001f\u007f\\]/.test(decoded) ||
        decoded.includes("?") || decoded.includes("#") ||
        decoded.split("/").some((segment) => segment === "..")) return "";

    const hasScheme = /^[a-z][a-z0-9+.-]*:/i.test(raw);
    const isProtocolRelative = raw.startsWith("//");
    if (!examAssetBase) {
      // Without the injected host, only package-relative paths are usable.
      // This preserves file/static preview behavior but blocks absolute URLs.
      if (hasScheme || isProtocolRelative || raw.startsWith("/")) return "";
      return raw;
    }

    const Url = typeof URL === "function" ? URL :
      (window && typeof window.URL === "function" ? window.URL : null);
    if (Url) {
      try {
        const base = new Url(examAssetBase);
        const resolved = new Url(raw, base);
        if (resolved.protocol !== base.protocol || resolved.host !== base.host ||
            resolved.username || resolved.password ||
            !resolved.pathname.startsWith(base.pathname)) return "";
        return resolved.href;
      } catch (_) { return ""; }
    }

    // URL-less fallback for the test shim.  Absolute and protocol-relative
    // paths are accepted only when they use the exact injected origin.
    const baseMatch = /^https:\/\/([^/]+)(\/.*)?$/i.exec(examAssetBase);
    if (!baseMatch) return "";
    const origin = `https://${baseMatch[1]}`;
    const basePath = (baseMatch[2] || "/").endsWith("/")
      ? (baseMatch[2] || "/") : `${baseMatch[2]}/`;
    let path = raw;
    if (hasScheme || isProtocolRelative) {
      const absolute = /^https:\/\/([^/]+)(\/.*)?$/i.exec(raw);
      if (!absolute || absolute[1].toLowerCase() !== baseMatch[1].toLowerCase()) return "";
      path = absolute[2] || "/";
    }
    if (!path.startsWith("/")) path = `${basePath}${path}`;
    if (!path.startsWith(basePath)) return "";
    return `${origin}${path}`;
  }

  // Do not key recoverable data by the manifest's display candidate.  That
  // value is package-controlled and can be reused on a shared workstation.
  // A trusted, identity-bound context is added below before durable state is
  // loaded or transmitted.
  let storagePrefix = "nstu-exam:unknown";
  try {
    storagePrefix = `nstu-exam:${encodeURIComponent(String(manifest.id || "unknown")).slice(0, 192)}`;
  } catch (_) { /* Keep the fixed fallback for malformed package metadata. */ }
  const unboundStorageNonce = (() => {
    try {
      if (window.crypto && typeof window.crypto.randomUUID === "function") {
        return window.crypto.randomUUID();
      }
    } catch (_) { /* Fall through to a per-page nonce. */ }
    return `${Date.now()}-${Math.random().toString(36).slice(2)}`;
  })();
  let storageKey = "";
  let durableStorageKey = "";
  let activeStorageContextKey = "";
  const MAX_DURABLE_EVENTS = 256;
  const MAX_DURABLE_BYTES = 8 * 1024 * 1024;
  const MAX_QUARANTINED_EVENTS = 64;
  const MAX_QUARANTINED_BYTES = 512 * 1024;
  const MAX_ANSWER_BYTES = 16 * 1024;
  // Must match common::kMaximumStateChunks and ExamBridge's queue bound.
  const MAX_STATE_CHUNKS = 64;
  const MAX_STATE_ANSWERS = 512;
  const ANSWER_ACK_TIMEOUT_MS = 5000;
  const MAX_BRIDGE_RETRY_DELAY_MS = 10000;

  // The native WebView2 host injects this context before navigation. Keeping
  // the context outside the manifest prevents an exam package from choosing
  // its own client or session identity.
  function normalizeHex(value, bytes) {
    if (typeof value !== "string" || !new RegExp(`^[0-9a-fA-F]{${bytes * 2}}$`).test(value)) {
      return "";
    }
    return value.toLowerCase();
  }

  function normalizeNonZeroHex(value, bytes) {
    const normalized = normalizeHex(value, bytes);
    return normalized && !/^0+$/.test(normalized) ? normalized : "";
  }

  function normalizeExamContext(value) {
    const source = value && typeof value === "object" ? value : {};
    const packageId = typeof source.packageId === "string" &&
      source.packageId.indexOf("\0") === -1 &&
      utf8ByteLength(source.packageId) <= 128
      ? source.packageId : "";
    const candidateId = typeof source.candidateId === "string" &&
      source.candidateId.indexOf("\0") === -1 &&
      utf8ByteLength(source.candidateId) <= 128
      ? source.candidateId : "";
    return Object.freeze({
      packageId: packageId.indexOf("\0") === -1 ? packageId : "",
      packageDigestHex: normalizeNonZeroHex(source.packageDigestHex, 32),
      clientIdHex: normalizeNonZeroHex(source.clientIdHex, 16),
      sessionIdHex: normalizeNonZeroHex(source.sessionIdHex, 16),
      previousEventHashHex: normalizeNonZeroHex(source.previousEventHashHex, 32),
      candidateId: candidateId.indexOf("\0") === -1 ? candidateId : "",
      nextSequence: Number.isSafeInteger(source.nextSequence) && source.nextSequence > 0
        ? source.nextSequence
        : 1
    });
  }

  let examContext = normalizeExamContext(window.NSTU_EXAM_CONTEXT);

  function utf8ByteLength(value) {
    if (typeof value !== "string") return Number.MAX_SAFE_INTEGER;
    try {
      if (typeof TextEncoder === "function") {
        return new TextEncoder().encode(value).length;
      }
      return unescape(encodeURIComponent(value)).length;
    } catch (_) {
      return Number.MAX_SAFE_INTEGER;
    }
  }

  function validIdentifier(value, maximum, allowEmpty = false) {
    return typeof value === "string" &&
      (allowEmpty || value.length > 0) &&
      value.indexOf("\0") === -1 && utf8ByteLength(value) <= maximum;
  }

  function contextKey(context = examContext) {
    if (!context || context.packageId !== manifest.id ||
        !context.packageDigestHex || !context.clientIdHex ||
        !context.sessionIdHex || !context.candidateId) return "";
    return JSON.stringify([
      context.packageId, context.packageDigestHex, context.clientIdHex,
      context.sessionIdHex, context.candidateId
    ]);
  }

  function contextStorageKey(context = examContext) {
    const key = contextKey(context);
    if (!key) return `${storagePrefix}:unbound:${unboundStorageNonce}`;
    try {
      return `${storagePrefix}:context:${encodeURIComponent(key)}`;
    } catch (_) {
      return `${storagePrefix}:unbound:${unboundStorageNonce}`;
    }
  }

  function configureStorage(context = examContext) {
    activeStorageContextKey = contextKey(context);
    storageKey = contextStorageKey(context);
    durableStorageKey = `${storageKey}:durable-v1`;
  }

  configureStorage();

  function utf8Encode(value) {
    if (typeof value !== "string") return new Uint8Array();
    try {
      if (typeof TextEncoder === "function") {
        return new TextEncoder().encode(value);
      }
      const encoded = unescape(encodeURIComponent(value));
      const bytes = new Uint8Array(encoded.length);
      for (let index = 0; index < encoded.length; index += 1) {
        bytes[index] = encoded.charCodeAt(index);
      }
      return bytes;
    } catch (_) {
      return new Uint8Array();
    }
  }

  function hexBytes(value, bytes) {
    const normalized = normalizeHex(value, bytes);
    if (!normalized) return null;
    const output = new Uint8Array(bytes);
    for (let index = 0; index < bytes; index += 1) {
      output[index] = parseInt(normalized.slice(index * 2, index * 2 + 2), 16);
    }
    return output;
  }

  function hexString(bytes) {
    let output = "";
    for (const value of bytes) output += value.toString(16).padStart(2, "0");
    return output;
  }

  function pushUint16(output, value) {
    output.push(value & 0xff, (value >>> 8) & 0xff);
  }

  function pushUint32(output, value) {
    const normalized = Number(value) >>> 0;
    output.push(normalized & 0xff, (normalized >>> 8) & 0xff,
      (normalized >>> 16) & 0xff, (normalized >>> 24) & 0xff);
  }

  function pushUint64(output, value) {
    if (!Number.isSafeInteger(value) || value < 0) return false;
    const low = value >>> 0;
    const high = Math.floor(value / 0x100000000) >>> 0;
    pushUint32(output, low);
    pushUint32(output, high);
    return true;
  }

  // This is the same canonical event encoding used by common::encode_answer_event.
  // Keeping the digest local lets a browser recover an event after an ACK was
  // lost without trusting an unauthenticated answer value alone.
  function canonicalEventBytes(event) {
    if (!event || typeof event !== "object") return null;
    const packageBytes = utf8Encode(event.packageId);
    const candidateBytes = utf8Encode(event.candidateId);
    const questionBytes = utf8Encode(event.questionId);
    const answerBytes = utf8Encode(event.answer);
    const packageDigest = hexBytes(event.packageDigestHex, 32);
    const clientId = hexBytes(event.clientIdHex, 16);
    const sessionId = hexBytes(event.sessionIdHex, 16);
    const previousHash = event.previousEventHashHex
      ? hexBytes(event.previousEventHashHex, 32) : new Uint8Array(32);
    const kind = { upsert: 1, clear: 2, finalize: 3 }[event.kind];
    if (!kind || !packageDigest || !clientId || !sessionId || !previousHash ||
        packageBytes.length > 0xffff || candidateBytes.length > 0xffff ||
        questionBytes.length > 0xffff || answerBytes.length > 0xffffffff ||
        !Number.isSafeInteger(event.questionRevision) || event.questionRevision <= 0 ||
        !Number.isSafeInteger(event.sequence) || event.sequence <= 0 ||
        !Number.isSafeInteger(event.clientTimeUnixMilliseconds) ||
        event.clientTimeUnixMilliseconds <= 0 ||
        (event.kind === "finalize" &&
         (event.questionId !== "" || answerBytes.length !== 0)) ||
        (event.kind === "clear" && answerBytes.length !== 0) ||
        (event.kind !== "finalize" && event.questionId === "")) return null;
    const output = [];
    pushUint32(output, 0x3156454e); // NEV1
    pushUint16(output, 1);
    output.push(kind, 0);
    if (!pushUint64(output, event.sequence) ||
        !pushUint64(output, event.clientTimeUnixMilliseconds)) return null;
    pushUint32(output, event.questionRevision);
    pushUint16(output, packageBytes.length);
    pushUint16(output, candidateBytes.length);
    pushUint16(output, questionBytes.length);
    pushUint32(output, answerBytes.length);
    for (const bytes of [packageDigest, clientId, sessionId, previousHash,
      packageBytes, candidateBytes, questionBytes, answerBytes]) {
      output.push(...bytes);
    }
    return new Uint8Array(output);
  }

  const SHA256_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
    0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
    0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ];

  function rotateRight(value, amount) {
    return (value >>> amount) | (value << (32 - amount));
  }

  function sha256(bytes) {
    const bitLength = bytes.length * 8;
    const paddedLength = ((bytes.length + 9 + 63) >> 6) << 6;
    const padded = new Uint8Array(paddedLength);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    const lengthOffset = padded.length - 8;
    const high = Math.floor(bitLength / 0x100000000) >>> 0;
    const low = bitLength >>> 0;
    padded[lengthOffset] = (high >>> 24) & 0xff;
    padded[lengthOffset + 1] = (high >>> 16) & 0xff;
    padded[lengthOffset + 2] = (high >>> 8) & 0xff;
    padded[lengthOffset + 3] = high & 0xff;
    padded[lengthOffset + 4] = (low >>> 24) & 0xff;
    padded[lengthOffset + 5] = (low >>> 16) & 0xff;
    padded[lengthOffset + 6] = (low >>> 8) & 0xff;
    padded[lengthOffset + 7] = low & 0xff;
    const hash = new Uint32Array([
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ]);
    const schedule = new Uint32Array(64);
    for (let offset = 0; offset < padded.length; offset += 64) {
      for (let index = 0; index < 16; index += 1) {
        const base = offset + index * 4;
        schedule[index] = ((padded[base] << 24) | (padded[base + 1] << 16) |
          (padded[base + 2] << 8) | padded[base + 3]) >>> 0;
      }
      for (let index = 16; index < 64; index += 1) {
        const value = schedule[index - 15];
        const sigma0 = rotateRight(value, 7) ^ rotateRight(value, 18) ^ (value >>> 3);
        const previous = schedule[index - 2];
        const sigma1 = rotateRight(previous, 17) ^ rotateRight(previous, 19) ^ (previous >>> 10);
        schedule[index] = (schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1) >>> 0;
      }
      let [a, b, c, d, e, f, g, h] = hash;
      for (let index = 0; index < 64; index += 1) {
        const sigma1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const choice = (e & f) ^ (~e & g);
        const temp1 = (h + sigma1 + choice + SHA256_K[index] + schedule[index]) >>> 0;
        const sigma0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const majority = (a & b) ^ (a & c) ^ (b & c);
        const temp2 = (sigma0 + majority) >>> 0;
        h = g; g = f; f = e; e = (d + temp1) >>> 0;
        d = c; c = b; b = a; a = (temp1 + temp2) >>> 0;
      }
      hash[0] = (hash[0] + a) >>> 0;
      hash[1] = (hash[1] + b) >>> 0;
      hash[2] = (hash[2] + c) >>> 0;
      hash[3] = (hash[3] + d) >>> 0;
      hash[4] = (hash[4] + e) >>> 0;
      hash[5] = (hash[5] + f) >>> 0;
      hash[6] = (hash[6] + g) >>> 0;
      hash[7] = (hash[7] + h) >>> 0;
    }
    const output = new Uint8Array(32);
    for (let index = 0; index < hash.length; index += 1) {
      output[index * 4] = (hash[index] >>> 24) & 0xff;
      output[index * 4 + 1] = (hash[index] >>> 16) & 0xff;
      output[index * 4 + 2] = (hash[index] >>> 8) & 0xff;
      output[index * 4 + 3] = hash[index] & 0xff;
    }
    return output;
  }

  function eventHashHex(event) {
    const bytes = canonicalEventBytes(event);
    return bytes ? hexString(sha256(bytes)) : "";
  }

  function boundedJsonSize(value) {
    try { return utf8ByteLength(JSON.stringify(value)); }
    catch (_) { return Number.MAX_SAFE_INTEGER; }
  }

  function validStoredEvent(value) {
    const previousHashValue = value && typeof value.previousEventHashHex === "string"
      ? value.previousEventHashHex : "";
    if (!value || typeof value !== "object" ||
        !Number.isSafeInteger(value.sequence) || value.sequence <= 0 ||
        value.sequence === Number.MAX_SAFE_INTEGER ||
        !validIdentifier(value.packageId, 128) ||
        !normalizeNonZeroHex(value.packageDigestHex, 32) ||
        !normalizeNonZeroHex(value.clientIdHex, 16) ||
        !normalizeNonZeroHex(value.sessionIdHex, 16) ||
        !validIdentifier(value.candidateId, 128) ||
        !validIdentifier(value.questionId, 128, true) ||
        !Number.isSafeInteger(value.questionRevision) || value.questionRevision <= 0 ||
        value.questionRevision > 0xffffffff ||
         !Number.isSafeInteger(value.clientTimeUnixMilliseconds) ||
         value.clientTimeUnixMilliseconds <= 0 ||
         !["upsert", "clear", "finalize"].includes(value.kind) ||
         typeof value.answer !== "string" ||
         utf8ByteLength(value.answer) > MAX_ANSWER_BYTES ||
         (value.kind === "finalize" && value.answer !== "") ||
        (value.eventHashHex !== undefined &&
         !normalizeNonZeroHex(value.eventHashHex, 32)) ||
        (previousHashValue !== "" &&
         !normalizeNonZeroHex(previousHashValue, 32)) ||
        (value.kind === "finalize" && value.questionId !== "") ||
         (value.kind === "clear" && value.answer !== "") ||
         (value.kind !== "finalize" && value.questionId === "")) {
      return null;
    }
    const event = {
      packageId: value.packageId,
      packageDigestHex: normalizeNonZeroHex(value.packageDigestHex, 32),
      clientIdHex: normalizeNonZeroHex(value.clientIdHex, 16),
      sessionIdHex: normalizeNonZeroHex(value.sessionIdHex, 16),
      candidateId: value.candidateId,
      questionId: value.questionId,
      questionRevision: value.questionRevision,
      sequence: value.sequence,
      clientTimeUnixMilliseconds: value.clientTimeUnixMilliseconds,
      kind: value.kind,
      answer: value.answer,
      ...(value.eventHashHex
        ? { eventHashHex: normalizeNonZeroHex(value.eventHashHex, 32) } : {}),
      previousEventHashHex: previousHashValue
        ? normalizeNonZeroHex(previousHashValue, 32) : ""
    };
    const calculatedHash = eventHashHex(event);
    if (!calculatedHash ||
        (value.eventHashHex && value.eventHashHex.toLowerCase() !== calculatedHash)) {
      return null;
    }
    event.eventHashHex = calculatedHash;
    return boundedJsonSize(event) <= MAX_DURABLE_BYTES ? event : null;
  }

  function validStoredDraft(id, value) {
    if (!validIdentifier(id, 128) ||
        !value || typeof value !== "object" ||
        !Number.isSafeInteger(value.questionRevision) ||
        value.questionRevision <= 0 || value.questionRevision > 0xffffffff) {
      return null;
    }
    const draftValue = value.value;
    if (draftValue !== null && typeof draftValue !== "string" &&
        typeof draftValue !== "number" && typeof draftValue !== "boolean" &&
        !Array.isArray(draftValue) && typeof draftValue !== "object") {
      return null;
    }
    let answer = "";
    try {
      answer = Array.isArray(draftValue) ||
        (draftValue && typeof draftValue === "object")
        ? JSON.stringify(draftValue) : String(draftValue ?? "");
    } catch (_) {
      return null;
    }
    if (typeof answer !== "string" || utf8ByteLength(answer) > MAX_ANSWER_BYTES) {
      return null;
    }
    const draft = {
      value: draftValue,
      questionRevision: value.questionRevision,
      contextKey: typeof value.contextKey === "string" && value.contextKey.length <= 512
        ? value.contextKey : ""
    };
    return boundedJsonSize(draft) <= MAX_DURABLE_BYTES ? draft : null;
  }

  function addQuarantineEntry(target, entry) {
    if (!Array.isArray(target) || !entry || typeof entry !== "object") return;
    target.push(entry);
    while (target.length > MAX_QUARANTINED_EVENTS ||
           boundedJsonSize(target) > MAX_QUARANTINED_BYTES) {
      target.shift();
    }
  }

  function quarantineContextFingerprint(sourceKey) {
    if (typeof sourceKey !== "string" || sourceKey.length === 0) return "";
    try {
      return hexString(sha256(utf8Encode(sourceKey))).slice(0, 16);
    } catch (_) {
      return "";
    }
  }

  function quarantineSummary(kind, reason, value, sourceKey = "") {
    const entry = {
      kind, reason, contextKey: quarantineContextFingerprint(sourceKey),
      capturedAt: Date.now()
    };
    if (kind === "event" && value && typeof value === "object") {
      if (Number.isSafeInteger(value.sequence)) entry.sequence = value.sequence;
      if (typeof value.questionId === "string") entry.questionId = value.questionId.slice(0, 128);
      if (typeof value.kind === "string") entry.eventKind = value.kind.slice(0, 16);
      if (typeof value.answer === "string") entry.answerBytes = utf8ByteLength(value.answer);
    } else if (kind === "draft" && value && typeof value === "object") {
      if (typeof value.questionId === "string") entry.questionId = value.questionId.slice(0, 128);
      if (value.value !== null && value.value !== undefined) {
        entry.valueType = Array.isArray(value.value) ? "array" : typeof value.value;
        try { entry.answerBytes = utf8ByteLength(answerText(value.value)); }
        catch (_) { entry.answerBytes = 0; }
      }
    }
    return entry;
  }

  function readStorage(key, fallback = "") {
    try { return localStorage.getItem(key) ?? fallback; }
    catch (_) { return fallback; }
  }
  function writeStorage(key, value) {
    try {
      localStorage.setItem(key, value);
      return true;
    } catch (_) {
      /* Hosts may disable browser storage; server messages still work. */
      return false;
    }
  }

  function loadDurableState() {
    const value = loadJson(durableStorageKey, {});
    if (!value || typeof value !== "object") {
      return {
        nextSequence: examContext.nextSequence, previousEventHashHex: "",
        serverHighestSequence: 0, serverLastEventHashHex: "",
        serverStateHashHex: "", serverFinalized: false,
        contextKey: activeStorageContextKey, pending: [], drafts: Object.create(null), quarantined: [],
        finalizeRequested: false
      };
    }
    const pending = [];
    const drafts = Object.create(null);
    const quarantined = [];
    if (Array.isArray(value.pending)) {
      value.pending.slice(0, MAX_DURABLE_EVENTS * 2).forEach((rawEvent) => {
        const event = validStoredEvent(rawEvent);
        if (!event) {
          addQuarantineEntry(quarantined,
            quarantineSummary("event", "invalid", rawEvent));
          return;
        }
        const duplicateIndex = pending.findIndex((candidate) =>
          candidate.sequence === event.sequence);
        if (duplicateIndex >= 0) {
          const duplicate = pending[duplicateIndex];
          if (duplicate.eventHashHex !== event.eventHashHex) {
            // Conflicting records for one sequence cannot be resolved locally.
            // Remove both from the send queue and retain bounded evidence.
            pending.splice(duplicateIndex, 1);
            addQuarantineEntry(quarantined,
              quarantineSummary("event", "duplicate-sequence", duplicate));
            addQuarantineEntry(quarantined,
              quarantineSummary("event", "duplicate-sequence", event));
          }
          return;
        }
        pending.push(event);
      });
    }
    if (value.drafts && typeof value.drafts === "object") {
      Object.entries(value.drafts).slice(0, MAX_DURABLE_EVENTS * 2).forEach(([id, rawDraft]) => {
        const draft = validStoredDraft(id, rawDraft);
        if (draft) drafts[id] = draft;
        else addQuarantineEntry(quarantined,
          quarantineSummary("draft", "invalid", { ...rawDraft, questionId: id }));
      });
    }
    if (Array.isArray(value.quarantined)) {
      value.quarantined.slice(-MAX_QUARANTINED_EVENTS).forEach((entry) => {
        if (!entry || typeof entry !== "object" ||
            !["event", "draft", "finalize"].includes(entry.kind) ||
            typeof entry.reason !== "string") return;
        addQuarantineEntry(quarantined, {
          kind: entry.kind,
          reason: entry.reason.slice(0, 64),
          contextKey: quarantineContextFingerprint(
            typeof entry.contextKey === "string" ? entry.contextKey : ""),
          capturedAt: Number.isSafeInteger(entry.capturedAt) ? entry.capturedAt : Date.now(),
          ...(entry.sequence !== undefined && Number.isSafeInteger(entry.sequence)
            ? { sequence: entry.sequence } : {}),
          ...(typeof entry.questionId === "string"
            ? { questionId: entry.questionId.slice(0, 128) } : {})
        });
      });
    }
    const nextSequence = Number.isSafeInteger(value.nextSequence) && value.nextSequence > 0
      ? value.nextSequence : examContext.nextSequence;
    // The cursor fields were added after the initial browser bridge shipped.
    // Recover a conservative legacy cursor from the predecessor hash and the
    // first pending sequence so delayed state responses cannot rewind it.
    let serverHighestSequence = Number.isSafeInteger(value.serverHighestSequence) &&
      value.serverHighestSequence >= 0 ? value.serverHighestSequence : 0;
    let serverLastEventHashHex = normalizeNonZeroHex(value.serverLastEventHashHex, 32);
    let serverStateHashHex = normalizeNonZeroHex(value.serverStateHashHex, 32);
    const previousEventHashHex = normalizeNonZeroHex(value.previousEventHashHex, 32);
    if (serverHighestSequence === 0 && !serverLastEventHashHex && previousEventHashHex) {
      const firstPendingSequence = pending.reduce((minimum, event) =>
        Math.min(minimum, event.sequence), Number.MAX_SAFE_INTEGER);
      const inferred = firstPendingSequence !== Number.MAX_SAFE_INTEGER
        ? firstPendingSequence - 1 : nextSequence - 1;
      if (Number.isSafeInteger(inferred) && inferred > 0) {
        serverHighestSequence = inferred;
        serverLastEventHashHex = previousEventHashHex;
      }
    }
    if (serverHighestSequence === 0 || !serverLastEventHashHex) {
      serverHighestSequence = 0;
      serverLastEventHashHex = "";
      serverStateHashHex = "";
    }
    pending.sort((left, right) => left.sequence - right.sequence);
    if (pending.length > MAX_DURABLE_EVENTS) {
      pending.splice(MAX_DURABLE_EVENTS).forEach((event) => {
        addQuarantineEntry(quarantined,
          quarantineSummary("event", "queue-limit", event));
      });
    }
    return {
      nextSequence,
      previousEventHashHex,
      serverHighestSequence,
      serverLastEventHashHex,
      serverStateHashHex,
      serverFinalized: value.serverFinalized === true,
      pending,
      drafts,
      contextKey: typeof value.contextKey === "string" && value.contextKey
        ? value.contextKey.slice(0, 512) : activeStorageContextKey,
      quarantined,
      finalizeRequested: value.finalizeRequested === true
    };
  }

  function persistDurableState() {
    const durable = state.durable;
    const value = {
      nextSequence: durable.nextSequence,
      previousEventHashHex: durable.previousEventHashHex,
      serverHighestSequence: durable.serverHighestSequence,
      serverLastEventHashHex: durable.serverLastEventHashHex,
      serverStateHashHex: durable.serverStateHashHex,
      serverFinalized: durable.serverFinalized === true,
      pending: durable.pending,
      drafts: durable.drafts,
      contextKey: durable.contextKey,
      quarantined: durable.quarantined,
      finalizeRequested: durable.finalizeRequested === true
    };
    let serialized;
    try { serialized = JSON.stringify(value); }
    catch (_) {
      state.syncError = "Answer recovery storage contains invalid data.";
      return false;
    }
    if (utf8ByteLength(serialized) > MAX_DURABLE_BYTES) {
      state.syncError = "Answer recovery storage is full.";
      return false;
    }
    if (!writeStorage(durableStorageKey, serialized)) {
      state.syncError = "Answer recovery storage is unavailable.";
      return false;
    }
    return true;
  }

  function hasLocalFinalization(durable, context = examContext) {
    const currentKey = contextKey(context);
    if (!durable || !currentKey ||
        (durable.contextKey && durable.contextKey !== currentKey)) {
      return false;
    }
    return Boolean(durable.finalizeRequested === true ||
      (Array.isArray(durable.pending) && durable.pending.some((event) =>
        event && event.kind === "finalize" &&
        eventMatchesContext(event, context))));
  }

  const trustedStorageContext = Boolean(contextKey(examContext));
  const storedRemainingRaw = trustedStorageContext
    ? readStorage(`${storageKey}:remaining`, "") : "";
  const storedRemainingSeconds = storedRemainingRaw === ""
    ? Number.NaN : Number(storedRemainingRaw);
  const restoredRemainingSeconds = Number.isFinite(storedRemainingSeconds) &&
      storedRemainingSeconds >= 0
    ? Math.min(Math.floor(storedRemainingSeconds), manifest.durationSeconds)
    : manifest.durationSeconds;
  const state = {
    language: trustedStorageContext ? readStorage(`${storageKey}:language`, "en") : "en",
    index: 0,
    answers: trustedStorageContext ? loadAnswerMap() : Object.create(null),
    notes: trustedStorageContext ? readStorage(`${storageKey}:notes`) : "",
    remainingSeconds: restoredRemainingSeconds,
    submitted: false,
    durable: loadDurableState(),
    inFlightSequence: 0,
    inFlightEventHashHex: "",
    inFlightContextKey: "",
    retryTimer: 0,
    ackTimer: 0,
    retryAttempt: 0,
    stateRequestAt: 0,
    awaitingStateReconcile: false,
    recoveryConflict: false,
    eventTimers: Object.create(null),
    finalizeRequested: false,
    syncError: "",
    stateChunks: null,
    serverFinalized: false
  };
  function adoptTrustedContextCursor() {
    const durable = state.durable;
    durable.nextSequence = Math.max(durable.nextSequence, examContext.nextSequence);
    if (!durable.previousEventHashHex && examContext.previousEventHashHex) {
      durable.previousEventHashHex = examContext.previousEventHashHex;
    }
    if (durable.serverHighestSequence === 0 &&
        !durable.serverLastEventHashHex && durable.previousEventHashHex) {
      const firstPendingSequence = durable.pending.reduce((minimum, event) =>
        Math.min(minimum, event.sequence), Number.MAX_SAFE_INTEGER);
      const inferred = firstPendingSequence !== Number.MAX_SAFE_INTEGER
        ? firstPendingSequence - 1 : examContext.nextSequence - 1;
      if (Number.isSafeInteger(inferred) && inferred > 0) {
        durable.serverHighestSequence = inferred;
        durable.serverLastEventHashHex = durable.previousEventHashHex;
      }
    }
  }
  adoptTrustedContextCursor();
  state.finalizeRequested = state.durable.finalizeRequested;
  state.serverFinalized = state.durable.serverFinalized === true;
  state.submitted = state.serverFinalized || hasLocalFinalization(state.durable);
  if (!I18N[state.language]) state.language = "en";

  const el = (id) => document.getElementById(id);
  const text = (key) => (I18N[state.language][key] || I18N.en[key] || key);
  const question = () => manifest.questions[state.index];

  function updateSubmissionLock() {
    const locked = state.serverFinalized === true ||
      state.finalizeRequested === true ||
      hasLocalFinalization(state.durable);
    state.submitted = locked;
    document.querySelectorAll("button, input, textarea, select").forEach((node) => {
      node.disabled = locked;
    });
    if (!locked) {
      // Re-enable controls when switching back to an editable context, while
      // retaining the normal first/last-question navigation constraints.
      const previous = document.querySelector('[data-action="previous"]');
      const next = document.querySelector('[data-action="next"]');
      if (previous) previous.disabled = state.index === 0;
      if (next) next.disabled = state.index === manifest.questions.length - 1;
    }
  }

  function loadJson(key, fallback) {
    try { return JSON.parse(readStorage(key, "null")) || fallback; }
    catch (_) { return fallback; }
  }

  function loadAnswerMap() {
    const raw = loadJson(`${storageKey}:answers`, {});
    const answers = Object.create(null);
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return answers;
    for (const item of manifest.questions) {
      if (!item || typeof item.id !== "string" ||
          !Object.prototype.hasOwnProperty.call(raw, item.id)) continue;
      const value = raw[item.id];
      let encoded;
      try { encoded = answerText(value); } catch (_) { continue; }
      if (typeof encoded === "string" && utf8ByteLength(encoded) <= MAX_ANSWER_BYTES) {
        answers[item.id] = Array.isArray(value) ? value.slice() : value;
      }
    }
    return answers;
  }

  function hostWebView() {
    return window.chrome && window.chrome.webview &&
      typeof window.chrome.webview.postMessage === "function"
      ? window.chrome.webview : null;
  }

  function postHostMessage(message) {
    const webview = hostWebView();
    if (!webview) return false;
    try {
      webview.postMessage(message);
      return true;
    } catch (_) {
      return false;
    }
  }

  function bridgeReady() {
    return Boolean(examContext.packageId && examContext.packageId === manifest.id &&
      examContext.packageDigestHex && examContext.clientIdHex &&
      examContext.sessionIdHex && examContext.candidateId);
  }

  function answerIsEmpty(value) {
    if (Array.isArray(value)) return value.length === 0;
    return value === null || value === undefined || String(value).trim() === "";
  }

  // Answer bytes are UTF-8 text. Complex answer controls are represented as
  // canonical JSON so the native host can convert them to the wire payload
  // without guessing at JavaScript types.
  function answerText(value) {
    if (Array.isArray(value) || (value && typeof value === "object")) {
      return JSON.stringify(value);
    }
    return value === null || value === undefined ? "" : String(value);
  }

  function questionById(id) {
    return manifest.questions.find((item) => item && item.id === id) || null;
  }

  function questionRevision(item) {
    const revision = Number(item && (item.questionRevision || item.revision));
    return Number.isSafeInteger(revision) && revision > 0 && revision <= 0xffffffff
      ? revision : 1;
  }

  function setConnectionStatus(value) {
    const node = el("connection-status");
    if (node) node.textContent = value;
  }

  function durableEventForDraft(id, draft) {
    const item = questionById(id);
    if (!item || !bridgeReady() ||
        (draft && draft.contextKey && draft.contextKey !== contextKey())) return null;
    const value = draft ? draft.value : "";
    let encodedAnswer;
    try { encodedAnswer = answerText(value); }
    catch (_) { return null; }
    if (typeof encodedAnswer !== "string" ||
        utf8ByteLength(encodedAnswer) > MAX_ANSWER_BYTES) return null;
    if (!Number.isSafeInteger(state.durable.nextSequence) ||
        state.durable.nextSequence <= 0 ||
        state.durable.nextSequence >= Number.MAX_SAFE_INTEGER ||
        state.durable.pending.some((event) =>
          event && event.sequence === state.durable.nextSequence)) return null;
    const event = {
      packageId: manifest.id,
      packageDigestHex: examContext.packageDigestHex,
      clientIdHex: examContext.clientIdHex,
      sessionIdHex: examContext.sessionIdHex,
      candidateId: examContext.candidateId,
      questionId: item.id,
      questionRevision: questionRevision(item),
      sequence: state.durable.nextSequence,
      clientTimeUnixMilliseconds: Date.now(),
      kind: answerIsEmpty(value) ? "clear" : "upsert",
      answer: encodedAnswer,
      previousEventHashHex: state.durable.previousEventHashHex || ""
    };
    const digest = eventHashHex(event);
    if (!digest) return null;
    event.eventHashHex = digest;
    return event;
  }

  function durableEventSize(event) {
    try { return utf8ByteLength(JSON.stringify(event)); }
    catch (_) { return MAX_DURABLE_BYTES; }
  }

  function requestExamState() {
    if (!bridgeReady()) return false;
    const now = Date.now();
    if (now - state.stateRequestAt < 1000) return false;
    state.stateRequestAt = now;
    return postHostMessage({
      type: "exam_state_request",
      request: {
        packageId: manifest.id,
        packageDigestHex: examContext.packageDigestHex,
        clientIdHex: examContext.clientIdHex,
        sessionIdHex: examContext.sessionIdHex,
        candidateId: examContext.candidateId
      }
    });
  }

  function requestExamStateWithRetry() {
    requestExamState();
    // Posting to the host does not guarantee that the server response or all
    // response chunks will arrive. Keep a bounded retry pending until a
    // complete state response clears it.
    scheduleBridgeRetry();
  }

  function beginStateReconcile() {
    state.awaitingStateReconcile = true;
    requestExamStateWithRetry();
  }

  function clearAckTimer() {
    if (state.ackTimer) {
      window.clearTimeout(state.ackTimer);
      state.ackTimer = 0;
    }
  }

  function clearInFlight() {
    state.inFlightSequence = 0;
    state.inFlightEventHashHex = "";
    state.inFlightContextKey = "";
  }

  function scheduleAckTimeout(sequence, eventHash, contextKeyValue) {
    clearAckTimer();
    state.ackTimer = window.setTimeout(() => {
      state.ackTimer = 0;
      if (state.inFlightSequence !== sequence ||
          state.inFlightEventHashHex !== eventHash ||
          state.inFlightContextKey !== contextKeyValue) return;
      clearInFlight();
      setConnectionStatus("Recovery acknowledgement timed out");
      // A lost ACK can affect the very first event as well.  Always request
      // the authoritative watermark; the service may already have retired
      // the event from its inflight set, so a blind resend is insufficient.
      if (state.durable.pending[0]) beginStateReconcile();
      scheduleBridgeRetry();
    }, ANSWER_ACK_TIMEOUT_MS);
  }

  function scheduleBridgeRetry() {
    if (state.retryTimer) return;
    const attempt = Math.min(state.retryAttempt, 4);
    const delay = Math.min(MAX_BRIDGE_RETRY_DELAY_MS, 1000 * (2 ** attempt));
    state.retryAttempt += 1;
    let timerId = 0;
    timerId = window.setTimeout(() => {
      if (state.retryTimer !== timerId) return;
      state.retryTimer = 0;
      // A retry timer can outlive an ACK that already advanced to another
      // event. Never clear that newer in-flight guard; its ACK timeout owns it.
      if (state.inFlightSequence !== 0) return;
      flushAnswerBridge();
    }, delay);
    state.retryTimer = timerId;
  }

  function flushAnswerBridge() {
    if (!bridgeReady() || !hostWebView()) {
      setConnectionStatus("Local recovery");
      return;
    }
    const durable = state.durable;
    // Validate the durable namespace before honoring a finalization marker.
    // A stale marker from another context must not short-circuit quarantine
    // and permanently lock the newly bound exam session.
    if (quarantineMismatchedDurableEntries() === null) return;
    if (state.serverFinalized || durable.serverFinalized === true) {
      state.serverFinalized = true;
      updateSubmissionLock();
      setConnectionStatus("Assessment finalized");
      return;
    }
    if (state.recoveryConflict) {
      setConnectionStatus("Recovery conflict requires manual review");
      return;
    }
    if (state.awaitingStateReconcile) {
      setConnectionStatus("Waiting for server recovery state");
      requestExamStateWithRetry();
      return;
    }
    if (durable.pending.length > 0) {
      const event = durable.pending[0];
      if (state.inFlightSequence === event.sequence) return;
      if (event.sequence > 1 && !durable.previousEventHashHex) {
        setConnectionStatus("Waiting for server recovery state");
        requestExamStateWithRetry();
        return;
      }
      if (event.sequence > 1 && eventMatchesContext(event) &&
          event.previousEventHashHex !== durable.previousEventHashHex) {
        event.previousEventHashHex = durable.previousEventHashHex;
        event.eventHashHex = eventHashHex(event);
        if (!event.eventHashHex) {
          state.syncError = "Answer recovery event could not be rehashed.";
          setConnectionStatus(state.syncError);
          return;
        }
        if (!persistDurableState()) return;
      }
      if (event.sequence === 1 && event.previousEventHashHex !== "") {
        event.previousEventHashHex = "";
        event.eventHashHex = eventHashHex(event);
        if (!event.eventHashHex) {
          state.syncError = "Answer recovery event could not be rehashed.";
          setConnectionStatus(state.syncError);
          return;
        }
        if (!persistDurableState()) return;
      }
      if (!postHostMessage({ type: "exam_answer_event", event })) {
        setConnectionStatus("Recovery queued");
        scheduleBridgeRetry();
        return;
      }
      state.inFlightSequence = event.sequence;
      state.inFlightEventHashHex = event.eventHashHex || eventHashHex(event);
      state.inFlightContextKey = activeStorageContextKey;
      scheduleAckTimeout(event.sequence, state.inFlightEventHashHex,
        state.inFlightContextKey);
      setConnectionStatus("Recovery syncing");
      return;
    }

    const draftIds = Object.keys(durable.drafts);
    if (draftIds.length > 0) {
      if (durable.nextSequence > 1 && !durable.previousEventHashHex) {
        setConnectionStatus("Waiting for server recovery state");
        requestExamStateWithRetry();
        return;
      }
      const id = draftIds[0];
      const previousDraft = durable.drafts[id];
      const event = durableEventForDraft(id, previousDraft);
      if (!event) return;
      if (durableEventSize(event) > MAX_DURABLE_BYTES ||
          durable.nextSequence >= Number.MAX_SAFE_INTEGER) {
        state.syncError = "Answer recovery storage cannot accept this answer.";
        setConnectionStatus(state.syncError);
        return;
      }
      delete durable.drafts[id];
      durable.pending.push(event);
      durable.nextSequence += 1;
      if (durable.pending.length > MAX_DURABLE_EVENTS ||
          boundedJsonSize(durable) > MAX_DURABLE_BYTES) {
        // Restore the draft rather than silently dropping an answer.
        durable.pending.pop();
        durable.nextSequence -= 1;
        durable.drafts[id] = previousDraft;
        state.syncError = "Answer recovery storage is full.";
        setConnectionStatus(state.syncError);
        persistDurableState();
        return;
      }
      if (!persistDurableState()) {
        durable.pending.pop();
        durable.nextSequence -= 1;
        durable.drafts[id] = previousDraft;
        setConnectionStatus("Answer recovery storage is unavailable.");
        return;
      }
      flushAnswerBridge();
      return;
    }

    if (durable.finalizeRequested) {
      if (durable.nextSequence > 1 && !durable.previousEventHashHex) {
        setConnectionStatus("Waiting for server recovery state");
        requestExamStateWithRetry();
        return;
      }
      if (durable.nextSequence >= Number.MAX_SAFE_INTEGER) {
        state.syncError = "Answer recovery sequence limit reached.";
        setConnectionStatus(state.syncError);
        return;
      }
      const finalEvent = {
        packageId: manifest.id,
        packageDigestHex: examContext.packageDigestHex,
        clientIdHex: examContext.clientIdHex,
        sessionIdHex: examContext.sessionIdHex,
        candidateId: examContext.candidateId,
        questionId: "",
        questionRevision: 1,
        sequence: durable.nextSequence,
        clientTimeUnixMilliseconds: Date.now(),
        kind: "finalize",
        answer: "",
        previousEventHashHex: durable.previousEventHashHex || ""
      };
      const finalHash = eventHashHex(finalEvent);
      if (!finalHash) {
        state.syncError = "Answer recovery could not hash the finalization event.";
        setConnectionStatus(state.syncError);
        return;
      }
      finalEvent.eventHashHex = finalHash;
      durable.finalizeRequested = false;
      durable.pending.push(finalEvent);
      durable.nextSequence += 1;
      if (!persistDurableState()) {
        durable.pending.pop();
        durable.nextSequence -= 1;
        durable.finalizeRequested = true;
        setConnectionStatus("Answer recovery storage is unavailable.");
        return;
      }
      flushAnswerBridge();
      return;
    }
    setConnectionStatus("Server recovery ready");
  }

  function queueAnswerDraft(id, value) {
    if (!questionById(id)) return;
    let encodedValue;
    try { encodedValue = answerText(value); }
    catch (_) { return; }
    if (typeof encodedValue !== "string" ||
        utf8ByteLength(encodedValue) > MAX_ANSWER_BYTES) return;
    const hadPrevious = Object.prototype.hasOwnProperty.call(state.durable.drafts, id);
    const previousDraft = state.durable.drafts[id];
    state.durable.drafts[id] = {
      value: Array.isArray(value) ? value.slice() : value,
      questionRevision: questionRevision(questionById(id)),
      contextKey: contextKey()
    };
    if (!persistDurableState()) {
      if (hadPrevious) state.durable.drafts[id] = previousDraft;
      else delete state.durable.drafts[id];
      setConnectionStatus("Answer recovery storage is unavailable.");
      return false;
    }
    window.clearTimeout(state.eventTimers[id]);
    state.eventTimers[id] = window.setTimeout(() => {
      delete state.eventTimers[id];
      flushAnswerBridge();
    }, 350);
    return true;
  }

  function handleAnswerAck(message) {
    const ack = message && message.ack && typeof message.ack === "object"
      ? message.ack : message;
    if (!ack || typeof ack !== "object") return;
    const sequence = Number(responseField(ack, "sequence", "sequence", 0));
    if (!Number.isSafeInteger(sequence) || sequence <= 0) return;
    const pending = state.durable.pending[0];
    const ackSessionId = normalizeNonZeroHex(
      responseField(ack, "sessionIdHex", "session_id_hex", ""), 16);
    if (!pending || pending.sequence !== sequence ||
        !eventMatchesContext(pending) ||
        !ackSessionId || ackSessionId !== pending.sessionIdHex) return;
    const statusNames = ["", "accepted", "duplicate", "rejected", "conflict", "gap", "unavailable"];
    const rawStatus = responseField(ack, "status", "status", "");
    const status = typeof rawStatus === "number"
      ? (statusNames[rawStatus] || "")
      : String(rawStatus || "").toLowerCase();
    if (!status) return;
    const eventHash = normalizeNonZeroHex(
      responseField(ack, "eventHashHex", "event_hash_hex", ""), 32);
    const pendingEventHash = normalizeNonZeroHex(pending.eventHashHex, 32) ||
      eventHashHex(pending);
    const currentContextKey = contextKey();
    const inFlightMatches = state.inFlightSequence === sequence &&
      state.inFlightContextKey === currentContextKey &&
      Boolean(pendingEventHash) &&
      state.inFlightEventHashHex === pendingEventHash;
    // Every journal outcome except an unavailable response carries the hash of
    // the event it evaluated. Do not let a delayed outcome for another event
    // clear the current ACK timer or retire the wrong record. An unavailable
    // response may have no hash when the server journal is closed, so it must
    // still be tied to the exact in-flight event locally.
    if ((status !== "unavailable" &&
         (!eventHash || !pendingEventHash || eventHash !== pendingEventHash)) ||
        (status === "unavailable" && !eventHash && !inFlightMatches) ||
        (eventHash && pendingEventHash && eventHash !== pendingEventHash)) {
      return;
    }
    if (status === "accepted" || status === "duplicate") {
      const highest = Number(responseField(
        ack, "highestContiguousSequence", "highest_contiguous_sequence", 0));
      const acknowledgedStateHash = normalizeNonZeroHex(
        responseField(ack, "stateHashHex", "state_hash_hex", ""), 32);
      const knownHighest = Number.isSafeInteger(state.durable.serverHighestSequence)
        ? state.durable.serverHighestSequence : 0;
      const knownLastHash = normalizeNonZeroHex(
        state.durable.serverLastEventHashHex, 32);
      const knownStateHash = normalizeNonZeroHex(
        state.durable.serverStateHashHex, 32);
      const cursorMismatch = Number.isSafeInteger(highest) &&
        (highest < knownHighest ||
         (highest === sequence && knownLastHash && eventHash !== knownLastHash) ||
         (highest === knownHighest && knownStateHash &&
          acknowledgedStateHash !== knownStateHash));
      if (!eventHash || !acknowledgedStateHash ||
          !Number.isSafeInteger(highest) || highest < sequence || cursorMismatch) {
        clearInFlight();
        clearAckTimer();
        state.syncError = "Invalid answer recovery acknowledgement.";
        setConnectionStatus(state.syncError);
        if (cursorMismatch) beginStateReconcile();
        else scheduleBridgeRetry();
        return;
      }
      // Persist the server-confirmed digest on legacy pending records before
      // they can participate in a later watermark reconciliation.
      pending.eventHashHex = eventHash;
      const acknowledgedEvent = state.durable.pending.shift();
      const previousDurableHash = state.durable.previousEventHashHex;
      const previousServerHighest = state.durable.serverHighestSequence;
      const previousServerLastHash = state.durable.serverLastEventHashHex;
      const previousServerStateHash = state.durable.serverStateHashHex;
      const previousServerFinalized = state.durable.serverFinalized === true;
      const previousFinalizeRequested = state.durable.finalizeRequested === true;
      const previousStateServerFinalized = state.serverFinalized;
      const previousStateFinalizeRequested = state.finalizeRequested;
      const previousAwaitingReconcile = state.awaitingStateReconcile;
      const previousRecoveryConflict = state.recoveryConflict;
      state.durable.previousEventHashHex = eventHash;
      if (highest === sequence) {
        state.durable.serverHighestSequence = sequence;
        state.durable.serverLastEventHashHex = eventHash;
        state.durable.serverStateHashHex = acknowledgedStateHash;
      }
      if (acknowledgedEvent.kind === "finalize" && highest === sequence) {
        state.durable.finalizeRequested = false;
        state.finalizeRequested = false;
        state.durable.serverFinalized = true;
        state.serverFinalized = true;
      }
      clearInFlight();
      state.awaitingStateReconcile = false;
      state.recoveryConflict = false;
      clearAckTimer();
      if (state.retryTimer) {
        window.clearTimeout(state.retryTimer);
        state.retryTimer = 0;
      }
      state.retryAttempt = 0;
      state.syncError = "";
      if (!persistDurableState()) {
        // Keep the acknowledged event in memory when the durable write fails.
        // It must remain eligible for retry after a browser restart.
        state.durable.pending.unshift(acknowledgedEvent);
        state.durable.previousEventHashHex = previousDurableHash;
        state.durable.serverHighestSequence = previousServerHighest;
        state.durable.serverLastEventHashHex = previousServerLastHash;
        state.durable.serverStateHashHex = previousServerStateHash;
        state.durable.serverFinalized = previousServerFinalized;
        state.durable.finalizeRequested = previousFinalizeRequested;
        state.serverFinalized = previousStateServerFinalized;
        state.finalizeRequested = previousStateFinalizeRequested;
        state.awaitingStateReconcile = previousAwaitingReconcile;
        state.recoveryConflict = previousRecoveryConflict;
        clearInFlight();
        setConnectionStatus("Answer recovery storage is unavailable.");
        scheduleBridgeRetry();
        return;
      }
      markSaved();
      if (highest > sequence) {
        // A duplicate can acknowledge a watermark advanced by another
        // delivery attempt. Reconcile the complete server state before
        // sending the next local event; otherwise an unseen sequence could
        // create a gap or reuse the wrong predecessor hash.
        beginStateReconcile();
        return;
      }
      flushAnswerBridge();
      return;
    }
    clearInFlight();
    clearAckTimer();
    state.syncError = `Answer recovery ${status || "failed"}.`;
    setConnectionStatus(state.syncError);
    // A rejected/conflict result is still retryable.  In particular, a late
    // result from an earlier delivery attempt must not leave the pending event
    // without either an in-flight timeout or a retry timer.
    if (status === "rejected" || status === "conflict" ||
        status === "gap" || status === "unavailable") {
      if (status === "gap" || status === "conflict") {
        beginStateReconcile();
      }
      scheduleBridgeRetry();
    }
  }

  function responseField(response, camelName, snakeName, fallback) {
    if (Object.prototype.hasOwnProperty.call(response, camelName)) {
      return response[camelName];
    }
    if (snakeName && Object.prototype.hasOwnProperty.call(response, snakeName)) {
      return response[snakeName];
    }
    return fallback;
  }

  function eventMatchesContext(event, context = examContext) {
    return event && context && event.packageId === context.packageId &&
      event.packageDigestHex === context.packageDigestHex &&
      event.clientIdHex === context.clientIdHex &&
      event.candidateId === context.candidateId &&
      event.sessionIdHex === context.sessionIdHex;
  }

  function quarantineMismatchedDurableEntries() {
    if (!bridgeReady()) return false;
    const currentKey = contextKey();
    const durable = state.durable;
    const pendingBefore = durable.pending.slice();
    const draftsBefore = Object.create(null);
    Object.entries(durable.drafts).forEach(([id, draft]) => {
      draftsBefore[id] = draft && typeof draft === "object"
        ? { ...draft } : draft;
    });
    const quarantinedBefore = durable.quarantined.slice();
    const cursorBefore = {
      contextKey: durable.contextKey,
      previousEventHashHex: durable.previousEventHashHex,
      nextSequence: durable.nextSequence,
      serverHighestSequence: durable.serverHighestSequence,
      serverLastEventHashHex: durable.serverLastEventHashHex,
      serverStateHashHex: durable.serverStateHashHex,
      serverFinalized: durable.serverFinalized,
      finalizeRequested: durable.finalizeRequested
    };
    const stateBefore = {
      submitted: state.submitted,
      finalizeRequested: state.finalizeRequested,
      serverFinalized: state.serverFinalized
    };
    const previousKey = durable.contextKey || "";
    const contextChanged = previousKey !== currentKey;
    let changed = false;
    const pending = [];
    durable.pending.forEach((event) => {
      if (eventMatchesContext(event)) {
        pending.push(event);
      } else {
        addQuarantineEntry(durable.quarantined,
          quarantineSummary("event", "context-mismatch", event,
            `${event.packageDigestHex || ""}|${event.clientIdHex || ""}|` +
            `${event.sessionIdHex || ""}|${event.candidateId || ""}`));
        changed = true;
      }
    });
    durable.pending = pending;
    const drafts = Object.create(null);
    Object.entries(durable.drafts).forEach(([id, draft]) => {
      if (!draft.contextKey || draft.contextKey === currentKey) {
        draft.contextKey = currentKey;
        drafts[id] = draft;
      } else {
        addQuarantineEntry(durable.quarantined,
          quarantineSummary("draft", "context-mismatch",
            { ...draft, questionId: id }, draft.contextKey));
        changed = true;
      }
    });
    durable.drafts = drafts;
    if (contextChanged) {
      if (durable.finalizeRequested) {
        addQuarantineEntry(durable.quarantined,
          quarantineSummary("finalize", "context-mismatch", {}, previousKey));
        durable.finalizeRequested = false;
      }
      if (durable.serverFinalized) {
        addQuarantineEntry(durable.quarantined,
          quarantineSummary("finalize", "context-mismatch", {
            sequence: durable.serverHighestSequence
          }, previousKey));
        durable.serverFinalized = false;
      }
      // The predecessor hash and sequence are scoped to one package/client/
      // session/candidate context. Never carry them into a new context; the
      // next state response supplies a fresh chain cursor.
      durable.previousEventHashHex = "";
      durable.serverHighestSequence = 0;
      durable.serverLastEventHashHex = "";
      durable.serverStateHashHex = "";
      const maxPendingSequence = durable.pending.reduce((maximum, event) =>
        Number.isSafeInteger(event.sequence) ? Math.max(maximum, event.sequence) : maximum, 0);
      durable.nextSequence = Math.max(
        examContext.nextSequence,
        maxPendingSequence < Number.MAX_SAFE_INTEGER
          ? maxPendingSequence + 1 : Number.MAX_SAFE_INTEGER);
      clearInFlight();
      state.awaitingStateReconcile = false;
      state.recoveryConflict = false;
      clearAckTimer();
      if (state.retryTimer) {
        window.clearTimeout(state.retryTimer);
        state.retryTimer = 0;
      }
      state.stateChunks = null;
      changed = true;
    }
    if (durable.contextKey !== currentKey) {
      durable.contextKey = currentKey;
      changed = true;
    }
    if (changed) {
      if (!persistDurableState()) {
        durable.pending = pendingBefore;
        durable.drafts = draftsBefore;
        durable.quarantined = quarantinedBefore;
        durable.contextKey = cursorBefore.contextKey;
        durable.previousEventHashHex = cursorBefore.previousEventHashHex;
        durable.nextSequence = cursorBefore.nextSequence;
        durable.serverHighestSequence = cursorBefore.serverHighestSequence;
        durable.serverLastEventHashHex = cursorBefore.serverLastEventHashHex;
        durable.serverStateHashHex = cursorBefore.serverStateHashHex;
        durable.serverFinalized = cursorBefore.serverFinalized;
        durable.finalizeRequested = cursorBefore.finalizeRequested;
        state.submitted = stateBefore.submitted;
        state.finalizeRequested = stateBefore.finalizeRequested;
        state.serverFinalized = stateBefore.serverFinalized;
        updateSubmissionLock();
        setConnectionStatus("Answer recovery storage is unavailable.");
        return null;
      }
      setConnectionStatus("Recovery found data from another session");
    }
    // Quarantine can remove a stale finalization event or marker. Recompute
    // the lock from the surviving, context-bound state instead of retaining
    // the value derived before quarantine ran.
    state.finalizeRequested = durable.finalizeRequested === true;
    state.serverFinalized = durable.serverFinalized === true;
    state.submitted = state.serverFinalized || hasLocalFinalization(durable);
    updateSubmissionLock();
    return changed;
  }

  function removeStorage(key) {
    try { localStorage.removeItem(key); } catch (_) { /* Best effort. */ }
  }

  function resetForTrustedContext(previousStorageKey) {
    // Load the exact target namespace before retiring the old active one. A
    // reconnect can legitimately return to a session that already has locally
    // buffered answers; contexts are never merged.
    Object.values(state.eventTimers).forEach((timer) => window.clearTimeout(timer));
    state.eventTimers = Object.create(null);
    window.clearTimeout(markSaved.timeout);
    state.answers = loadAnswerMap();
    state.notes = readStorage(`${storageKey}:notes`, "");
    window.NSTU_EXAM_RESPONSE = undefined;
    const storedRemainingRaw = readStorage(`${storageKey}:remaining`, "");
    const storedRemaining = storedRemainingRaw === ""
      ? Number.NaN : Number(storedRemainingRaw);
    state.remainingSeconds = Number.isFinite(storedRemaining) && storedRemaining >= 0
      ? Math.min(Math.floor(storedRemaining), manifest.durationSeconds)
      : manifest.durationSeconds;
    state.submitted = false;
    state.index = 0;
    clearInFlight();
    state.retryAttempt = 0;
    state.stateRequestAt = 0;
    state.awaitingStateReconcile = false;
    state.recoveryConflict = false;
    clearAckTimer();
    if (state.retryTimer) {
      window.clearTimeout(state.retryTimer);
      state.retryTimer = 0;
    }
    state.stateChunks = null;
    state.syncError = "";
    state.finalizeRequested = false;
    state.durable = loadDurableState();
    // Keep the namespace's recorded identity until it has been checked. A
    // stale marker must be quarantined before it can lock the newly bound
    // context; overwriting contextKey here would make that impossible.
    quarantineMismatchedDurableEntries();
    adoptTrustedContextCursor();
    state.finalizeRequested = state.durable.finalizeRequested === true;
    state.serverFinalized = state.durable.serverFinalized === true;
    state.submitted = state.serverFinalized || hasLocalFinalization(state.durable);
    const storedLanguage = readStorage(`${storageKey}:language`, state.language);
    if (I18N[storedLanguage]) state.language = storedLanguage;
    let serializedAnswers;
    try { serializedAnswers = JSON.stringify(state.answers); }
    catch (_) { serializedAnswers = "{}"; }
    writeStorage(`${storageKey}:answers`, serializedAnswers);
    writeStorage(`${storageKey}:notes`, state.notes);
    writeStorage(`${storageKey}:remaining`, String(state.remainingSeconds));
    // Keep the previous context's namespace intact. Contexts are keyed by the
    // complete package/client/session/candidate tuple, so retaining it does not
    // merge records or make them visible to the active context. A candidate
    // may legitimately return to a disconnected session and recover its own
    // locally buffered answers; deleting the predecessor here would make that
    // recovery impossible after every context notification.
    persistDurableState();
    document.querySelectorAll("button, input, textarea, select").forEach((node) => {
      node.disabled = state.serverFinalized;
    });
    if (el("notes")) el("notes").value = state.notes;
    if (el("candidate-label")) {
      el("candidate-label").textContent = `${text("candidate")}: ${examContext.candidateId || "—"}`;
    }
    applyLanguage();
    updateSubmissionLock();
    updateTimer();
  }

  function repairPendingChain(lastHash, highest, acceptedAnswers = [], stateHash = "") {
    const normalizedHash = normalizeNonZeroHex(lastHash, 32);
    const normalizedStateHash = normalizeNonZeroHex(stateHash, 32);
    if (!Number.isSafeInteger(highest) || highest < 0 ||
        (highest > 0 && (!normalizedHash || !normalizedStateHash)) ||
        (highest === 0 && (normalizedHash || normalizedStateHash))) {
      return false;
    }
    const durable = state.durable;
    const pendingBeforeRepair = durable.pending.map((event) => ({ ...event }));
    const previousHashBeforeRepair = durable.previousEventHashHex;
    const nextSequenceBeforeRepair = durable.nextSequence;
    const serverHighestBeforeRepair = durable.serverHighestSequence;
    const serverLastHashBeforeRepair = durable.serverLastEventHashHex;
    const serverStateHashBeforeRepair = durable.serverStateHashHex;
    const current = durable.pending.filter((event) => eventMatchesContext(event))
      .sort((left, right) => left.sequence - right.sequence);
    const acceptedBySequence = new Map();
    if (Array.isArray(acceptedAnswers)) {
      for (const answer of acceptedAnswers) {
        if (!answer || !Number.isSafeInteger(answer.sequence) ||
            answer.sequence <= 0) continue;
        acceptedBySequence.set(answer.sequence, {
          questionId: answer.questionId,
          questionRevision: answer.questionRevision,
          kind: normalizeAnswerKind(answer.kind),
          answer: typeof answer.answer === "string" ? answer.answer : "",
          eventHashHex: normalizeNonZeroHex(answer.eventHashHex, 32)
        });
      }
    }
    // A state watermark can cover events that never produced a browser ACK.
    // Do not discard those local records unless each covered event can be
    // individually verified by its event hash. The state answer value alone
    // is not an authenticated event identity and cannot prove equivalence.
    for (const event of current) {
      if (event.sequence > highest) continue;
      const accepted = acceptedBySequence.get(event.sequence);
      const localHash = normalizeNonZeroHex(event.eventHashHex, 32) ||
        eventHashHex(event);
      const expectedHash = event.sequence === highest
        ? normalizedHash : (accepted && accepted.eventHashHex);
      if (event.sequence !== highest && (!accepted ||
          accepted.questionId !== event.questionId ||
          accepted.questionRevision !== event.questionRevision ||
          accepted.kind !== event.kind || accepted.answer !== event.answer)) {
        return false;
      }
      const hashVerified = Boolean(localHash && expectedHash &&
        localHash === expectedHash);
      if (!hashVerified) return false;
    }
    const remaining = current.filter((event) => event.sequence > highest);
    if (remaining.length > 0) {
      const expected = highest + 1;
      if (remaining[0].sequence !== expected ||
          remaining.some((event, index) => index > 0 &&
            event.sequence !== remaining[index - 1].sequence + 1)) {
        return false;
      }
    }
    const currentSet = new Set(current);
    durable.pending = durable.pending.filter((event) =>
      !currentSet.has(event) || event.sequence > highest)
      .sort((left, right) => left.sequence - right.sequence);
    durable.previousEventHashHex = highest === 0 ? "" : normalizedHash;
    durable.nextSequence = Math.max(durable.nextSequence,
      highest === Number.MAX_SAFE_INTEGER ? highest : highest + 1);
    durable.serverHighestSequence = highest;
    durable.serverLastEventHashHex = highest === 0 ? "" : normalizedHash;
    durable.serverStateHashHex = highest === 0 ? "" : normalizedStateHash;

    // Only the first pending event can carry a known predecessor. Later
    // events are rebased after each matching ACK, preventing stale hashes from
    // an interrupted browser session from being submitted.
    let first = true;
    for (const event of durable.pending) {
      if (!eventMatchesContext(event) || event.sequence <= highest) continue;
      const previous = first
        ? (highest === 0 ? "" : normalizedHash) : "";
      if (event.previousEventHashHex !== previous || !event.eventHashHex) {
        event.previousEventHashHex = previous;
        event.eventHashHex = eventHashHex(event);
        if (!event.eventHashHex) {
          durable.pending = pendingBeforeRepair;
          durable.previousEventHashHex = previousHashBeforeRepair;
          durable.nextSequence = nextSequenceBeforeRepair;
          durable.serverHighestSequence = serverHighestBeforeRepair;
          durable.serverLastEventHashHex = serverLastHashBeforeRepair;
          durable.serverStateHashHex = serverStateHashBeforeRepair;
          return false;
        }
      }
      first = false;
    }
    if (!persistDurableState()) {
      durable.pending = pendingBeforeRepair;
      durable.previousEventHashHex = previousHashBeforeRepair;
      durable.nextSequence = nextSequenceBeforeRepair;
      durable.serverHighestSequence = serverHighestBeforeRepair;
      durable.serverLastEventHashHex = serverLastHashBeforeRepair;
      durable.serverStateHashHex = serverStateHashBeforeRepair;
      return false;
    }
    clearInFlight();
    clearAckTimer();
    return true;
  }

  function normalizeAnswerKind(value) {
    const names = ["", "upsert", "clear", "finalize"];
    if (typeof value === "number" && Number.isSafeInteger(value)) {
      return names[value] || "";
    }
    const normalized = String(value || "").toLowerCase();
    return ["upsert", "clear", "finalize"].includes(normalized)
      ? normalized : "";
  }

  function validateRecoveredAnswer(value, highest) {
    if (!value || typeof value !== "object") return null;
    const questionId = responseField(value, "questionId", "question_id", "");
    const revision = Number(responseField(
      value, "questionRevision", "question_revision", 0));
    const sequence = Number(responseField(value, "sequence", "sequence", 0));
    const kind = normalizeAnswerKind(responseField(value, "kind", "kind", ""));
    const rawAnswer = responseField(value, "answer", "answer", "");
    const rawEventHash = responseField(
      value, "eventHashHex", "event_hash_hex", undefined);
    const eventHash = rawEventHash === undefined
      ? "" : normalizeNonZeroHex(rawEventHash, 32);
    if (!validIdentifier(questionId, 128) ||
        !questionById(questionId) || !Number.isSafeInteger(revision) ||
        revision <= 0 || revision > 0xffffffff ||
        !Number.isSafeInteger(sequence) || sequence <= 0 ||
        sequence > highest || !["upsert", "clear"].includes(kind) ||
        typeof rawAnswer !== "string" ||
        utf8ByteLength(rawAnswer) > MAX_ANSWER_BYTES ||
        (kind === "clear" && rawAnswer !== "") ||
        (highest > 0 && !eventHash) ||
        (rawEventHash !== undefined && !eventHash)) {
      return null;
    }
    return {
      questionId,
      questionRevision: revision,
      sequence,
      kind,
      answer: rawAnswer,
      ...(eventHash ? { eventHashHex: eventHash } : {})
    };
  }

  function validateRecoveredAnswers(values, highest,
                                    questionIds = new Set(),
                                    sequences = new Set()) {
    if (!Array.isArray(values)) return null;
    const normalized = [];
    for (const value of values) {
      const answer = validateRecoveredAnswer(value, highest);
      if (!answer || questionIds.has(answer.questionId) ||
          sequences.has(answer.sequence)) return null;
      questionIds.add(answer.questionId);
      sequences.add(answer.sequence);
      normalized.push(answer);
    }
    return normalized;
  }

  function collectStateChunk(response) {
    const rawIndex = responseField(response, "chunkIndex", "chunk_index", 0);
    const rawCount = responseField(response, "chunkCount", "chunk_count", 1);
    const chunkIndex = Number(rawIndex);
    const chunkCount = Number(rawCount);
    if (!Number.isSafeInteger(chunkIndex) || !Number.isSafeInteger(chunkCount) ||
        chunkCount < 1 || chunkCount > MAX_STATE_CHUNKS || chunkIndex < 0 ||
        chunkIndex >= chunkCount || !Array.isArray(response.answers)) {
      return null;
    }
    // A state response is intentionally bounded even though each individual
    // command is authenticated. This prevents a malicious/replayed host
    // message from growing the browser's reassembly buffer without limit.
    if (response.answers.length > MAX_STATE_ANSWERS) return null;
    const packageId = String(responseField(response, "packageId", "package_id", ""));
    const packageDigestHex = normalizeNonZeroHex(
      responseField(response, "packageDigestHex", "package_digest_hex", ""), 32);
    const clientIdHex = normalizeNonZeroHex(
      responseField(response, "clientIdHex", "client_id_hex", ""), 16);
    const sessionIdHex = normalizeNonZeroHex(
      responseField(response, "sessionIdHex", "session_id_hex", ""), 16);
    const candidateId = String(responseField(response, "candidateId", "candidate_id", ""));
    const highest = Number(responseField(
      response, "highestContiguousSequence", "highest_contiguous_sequence", 0));
    const rawStateHash = responseField(
      response, "stateHashHex", "state_hash_hex", undefined);
    const stateHashHex = rawStateHash === undefined
      ? "" : normalizeHex(rawStateHash, 32);
    const finalized = responseField(response, "finalized", "finalized", false);
    const rawLastEventHash = responseField(
      response, "lastEventHashHex", "last_event_hash_hex", undefined);
    const lastEventHashHex = rawLastEventHash === undefined
      ? "" : normalizeHex(rawLastEventHash, 32);
    if (!validIdentifier(packageId, 128) ||
        !validIdentifier(candidateId, 128) ||
        packageId !== manifest.id || packageId !== examContext.packageId ||
        packageDigestHex !== examContext.packageDigestHex ||
        clientIdHex !== examContext.clientIdHex || !sessionIdHex ||
        sessionIdHex !== examContext.sessionIdHex || candidateId !== examContext.candidateId ||
        !Number.isSafeInteger(highest) || highest < 0 ||
        (finalized !== true && finalized !== false) ||
        (finalized === true && highest === 0)) {
      return null;
    }
    const stateHashNonZero = stateHashHex && !/^0+$/.test(stateHashHex);
    const lastEventHashNonZero = lastEventHashHex && !/^0+$/.test(lastEventHashHex);
    const stateHashProvided = rawStateHash !== undefined && rawStateHash !== "";
    const lastEventHashProvided = rawLastEventHash !== undefined && rawLastEventHash !== "";
    if ((stateHashProvided && !stateHashHex) ||
        (lastEventHashProvided && !lastEventHashHex) ||
        (highest > 0 && (!stateHashNonZero || !lastEventHashNonZero)) ||
        (highest === 0 && (stateHashNonZero || lastEventHashNonZero))) {
      return null;
    }
    if (chunkCount > 1 && !stateHashHex) return null;
    const firstAnswers = validateRecoveredAnswers(response.answers, highest);
    if (!firstAnswers) return null;
    const key = `${packageId}|${packageDigestHex}|${clientIdHex}|${sessionIdHex}|` +
      `${candidateId}|${highest}|${stateHashHex}|${finalized}|${lastEventHashHex}`;
    if (chunkCount === 1) {
      state.stateChunks = null;
      return {
        ...response,
        packageId,
        packageDigestHex,
        clientIdHex,
        sessionIdHex,
        candidateId,
        stateHashHex,
        lastEventHashHex,
        chunkIndex: 0,
        chunkCount: 1,
        answers: firstAnswers
      };
    }
    const now = Date.now();
    if (!state.stateChunks || state.stateChunks.key !== key ||
        state.stateChunks.count !== chunkCount ||
        now - state.stateChunks.createdAt > 15000) {
      state.stateChunks = { key, count: chunkCount, createdAt: now, chunks: new Map(), answerCount: 0 };
    }
    const existing = state.stateChunks.chunks.get(chunkIndex);
    if (existing) {
      try {
        // Compare the validated, normalized representation.  A host may use
        // either camelCase or snake_case field names; equivalent chunks must
        // not be treated as conflicting merely because their wire spelling
        // differs.
        if (JSON.stringify(existing.answers) !== JSON.stringify(firstAnswers)) {
          state.stateChunks = null;
          return null;
        }
      } catch (_) {
        state.stateChunks = null;
        return null;
      }
    } else {
      state.stateChunks.answerCount += firstAnswers.length;
      if (state.stateChunks.answerCount > MAX_STATE_ANSWERS) {
        state.stateChunks = null;
        return null;
      }
      state.stateChunks.chunks.set(chunkIndex, {
        ...response,
        packageId,
        packageDigestHex,
        clientIdHex,
        sessionIdHex,
        candidateId,
        stateHashHex,
        lastEventHashHex,
        answers: firstAnswers
      });
    }
    if (state.stateChunks.chunks.size !== chunkCount) {
      setConnectionStatus(`Recovery chunks ${state.stateChunks.chunks.size}/${chunkCount}`);
      return null;
    }
    const chunks = [];
    const questionIds = new Set();
    const sequences = new Set();
    let answerBytes = 0;
    for (let index = 0; index < chunkCount; index += 1) {
      const chunk = state.stateChunks.chunks.get(index);
      if (!chunk) {
        state.stateChunks = null;
        return null;
      }
      for (const answer of chunk.answers) {
        if (!answer || typeof answer.questionId !== "string" ||
            questionIds.has(answer.questionId) ||
            sequences.has(answer.sequence)) {
          state.stateChunks = null;
          return null;
        }
        questionIds.add(answer.questionId);
        sequences.add(answer.sequence);
        try {
          answerBytes += utf8ByteLength(JSON.stringify(answer));
        } catch (_) {
          state.stateChunks = null;
          return null;
        }
        if (answerBytes > MAX_DURABLE_BYTES) {
          state.stateChunks = null;
          return null;
        }
        chunks.push(answer);
      }
    }
    const firstChunk = state.stateChunks.chunks.get(0);
    const complete = {
      ...(firstChunk || response),
      packageId,
      packageDigestHex,
      clientIdHex,
      sessionIdHex,
      candidateId,
      stateHashHex,
      lastEventHashHex,
      chunkIndex: 0,
      chunkCount: 1,
      answers: chunks
    };
    state.stateChunks = null;
    return complete;
  }

  function applyStateResponse(message) {
    const response = message && message.response && typeof message.response === "object"
      ? message.response : message;
    if (!response || typeof response !== "object") {
      if (state.awaitingStateReconcile) scheduleBridgeRetry();
      return;
    }
    const completeResponse = collectStateChunk(response);
    if (!completeResponse) {
      if (state.awaitingStateReconcile) scheduleBridgeRetry();
      return;
    }
    const packageId = responseField(completeResponse, "packageId", "package_id", "");
    const sessionIdHex = responseField(completeResponse, "sessionIdHex", "session_id_hex", "");
    if ((packageId && (packageId !== manifest.id || packageId !== examContext.packageId)) ||
        (sessionIdHex && sessionIdHex !== examContext.sessionIdHex)) return;
    const highest = Number(responseField(
      completeResponse, "highestContiguousSequence", "highest_contiguous_sequence", 0));
    const lastHash = normalizeNonZeroHex(responseField(
      completeResponse, "lastEventHashHex", "last_event_hash_hex", ""), 32);
    const stateHash = normalizeNonZeroHex(responseField(
      completeResponse, "stateHashHex", "state_hash_hex", ""), 32);
    const finalized = responseField(completeResponse, "finalized", "finalized", false) === true;
    const durable = state.durable;
    const knownHighest = Number.isSafeInteger(durable.serverHighestSequence)
      ? durable.serverHighestSequence : 0;
    const knownLastHash = normalizeNonZeroHex(durable.serverLastEventHashHex, 32);
    const knownStateHash = normalizeNonZeroHex(durable.serverStateHashHex, 32);
    const untrackedPreviousHash = Boolean(durable.previousEventHashHex &&
      !knownLastHash && knownHighest === 0);
    // State responses are authenticated, but they can still arrive late after
    // a newer response. A lower watermark, or an equivocation at the same
    // watermark, must never rewind the browser's predecessor hash.
    const lowerWatermark = Number.isSafeInteger(highest) && highest < knownHighest;
    const sameWatermarkConflict = Number.isSafeInteger(highest) &&
      highest === knownHighest && highest > 0 &&
      ((!lastHash || (knownLastHash && lastHash !== knownLastHash)) ||
       (knownStateHash && (!stateHash || stateHash !== knownStateHash)));
    const finalizedRegression = durable.serverFinalized === true && !finalized;
    const zeroCursorConflict = highest === 0 && untrackedPreviousHash;
    if (!Number.isSafeInteger(highest) || highest < 0 || lowerWatermark ||
        zeroCursorConflict ||
        sameWatermarkConflict || finalizedRegression) {
      if (state.awaitingStateReconcile &&
          (sameWatermarkConflict || finalizedRegression || zeroCursorConflict ||
           !Number.isSafeInteger(highest))) {
        state.recoveryConflict = true;
        state.syncError = "Stale or conflicting server recovery state.";
        setConnectionStatus(state.syncError);
      } else if (state.awaitingStateReconcile && lowerWatermark) {
        // A late response from an older request is harmless. Keep waiting for
        // the current request instead of converting it into a manual conflict.
        setConnectionStatus("Waiting for server recovery state");
        scheduleBridgeRetry();
      }
      return;
    }
    if (Number.isSafeInteger(highest) && highest >= 0) {
      if (!repairPendingChain(lastHash, highest, completeResponse.answers, stateHash)) {
        state.recoveryConflict = true;
        state.syncError = "Server recovery state does not match pending answers.";
        setConnectionStatus(state.syncError);
        return;
      }
      state.awaitingStateReconcile = false;
      state.recoveryConflict = false;
      state.syncError = "";
      if (state.retryTimer) {
        window.clearTimeout(state.retryTimer);
        state.retryTimer = 0;
      }
      state.retryAttempt = 0;
    }
    if (Array.isArray(completeResponse.answers)) {
      const locallyPending = new Set(Object.keys(state.durable.drafts));
      state.durable.pending.forEach((event) => {
        if (event && typeof event.questionId === "string" && event.questionId) {
          locallyPending.add(event.questionId);
        }
      });
      // The server response is authoritative for every non-pending question.
      // Rebuild that portion instead of merging fields into an older browser
      // map, otherwise a server-side clear/reset can leave stale answers
      // visible after reconnect.
      const authoritative = Object.create(null);
      completeResponse.answers.forEach((answer) => {
        if (!answer || typeof answer.questionId !== "string") return;
        if (String(answer.kind || "").toLowerCase() === "clear") {
          return;
        } else if (Object.prototype.hasOwnProperty.call(answer, "answer")) {
          authoritative[answer.questionId] = answer.answer;
        }
      });
      // Start with the server-authoritative map, then apply local pending
      // events and drafts. A pending browser write is newer than the
      // checkpoint that arrived over the bridge and must never be overwritten
      // by that checkpoint during reconnect.
      const nextAnswers = Object.create(null);
      Object.assign(nextAnswers, authoritative);
      for (const [id, value] of Object.entries(state.answers)) {
        if (locallyPending.has(id)) nextAnswers[id] = value;
      }
      state.durable.pending.forEach((event) => {
        if (!event || !event.questionId) return;
        if (event.kind === "clear") delete nextAnswers[event.questionId];
        else if (event.kind === "upsert") nextAnswers[event.questionId] = event.answer;
      });
      // A draft represents the newest local edit and has not yet become a
      // numbered event. Apply it after queued events so an older pending
      // record cannot overwrite the value the candidate just entered.
      Object.entries(state.durable.drafts).forEach(([id, draft]) => {
        if (draft && Object.prototype.hasOwnProperty.call(draft, "value")) {
          nextAnswers[id] = draft.value;
        }
      });
      state.answers = nextAnswers;
      let serializedAnswers;
      try { serializedAnswers = JSON.stringify(state.answers); }
      catch (_) { serializedAnswers = null; }
      if (typeof serializedAnswers !== "string" ||
          !writeStorage(`${storageKey}:answers`, serializedAnswers)) {
        markUnsaved();
        setConnectionStatus("Answer recovery storage is unavailable.");
      }
      renderQuestionList();
      renderQuestion();
    }
    if (finalized) {
      quarantineMismatchedDurableEntries();
      const pendingBeyondFinal = state.durable.pending.some((event) =>
        eventMatchesContext(event) && event.sequence > highest);
      const draftsBeyondFinal = Object.keys(state.durable.drafts).length > 0;
      if (pendingBeyondFinal || draftsBeyondFinal) {
        state.recoveryConflict = true;
        state.syncError = "Server finalized while local recovery data remains.";
        setConnectionStatus(state.syncError);
        return;
      }
      const previousDurableFinalizeRequested = state.durable.finalizeRequested;
      const previousDurableServerFinalized = state.durable.serverFinalized;
      const previousStateFinalizeRequested = state.finalizeRequested;
      const previousStateServerFinalized = state.serverFinalized;
      state.durable.finalizeRequested = false;
      state.finalizeRequested = false;
      state.durable.serverFinalized = true;
      state.serverFinalized = true;
      if (!persistDurableState()) {
        state.durable.finalizeRequested = previousDurableFinalizeRequested;
        state.durable.serverFinalized = previousDurableServerFinalized;
        state.finalizeRequested = previousStateFinalizeRequested;
        state.serverFinalized = previousStateServerFinalized;
        state.recoveryConflict = true;
        state.syncError = "Answer recovery storage is unavailable.";
        setConnectionStatus(state.syncError);
        return;
      }
      state.submitted = true;
      document.querySelectorAll("button, input, textarea, select").forEach((node) => { node.disabled = true; });
    }
    setConnectionStatus("Server recovery ready");
    if (!state.serverFinalized) flushAnswerBridge();
  }

  function handleHostMessage(event) {
    let message = event && event.data;
    if (typeof message === "string") {
      try { message = JSON.parse(message); } catch (_) { return; }
    }
    if (!message || typeof message.type !== "string") return;
    if (message.type === "exam_context") {
      const nextContext = normalizeExamContext(message.context || message);
      // Context changes are allowed for reconnects within this loaded package
      // (for example, a new authenticated candidate/session tuple), but a
      // package-originated message must never move the page into another
      // package namespace. The native host separately authenticates all
      // answer/state payloads against its own immutable context.
      if (nextContext.packageId !== manifest.id) {
        setConnectionStatus("Exam context does not match this package");
        return;
      }
      const nextContextKey = contextKey(nextContext);
      if (!nextContextKey) {
        setConnectionStatus("Exam identity is unavailable");
        return;
      }
      const previousStorageKey = storageKey;
      const contextChanged = nextContextKey !== activeStorageContextKey;
      examContext = nextContext;
      configureStorage(nextContext);
      if (contextChanged) {
        // Answers, notes, timers, and durable events are all candidate/session
        // scoped. Never display or transmit data from the prior context.
        resetForTrustedContext(previousStorageKey);
    } else {
      quarantineMismatchedDurableEntries();
      state.durable.contextKey = activeStorageContextKey;
      adoptTrustedContextCursor();
      state.serverFinalized = state.durable.serverFinalized === true;
      state.submitted = state.serverFinalized || hasLocalFinalization(state.durable);
      persistDurableState();
      updateSubmissionLock();
      }
      beginStateReconcile();
      flushAnswerBridge();
    } else if (message.type === "exam_answer_ack") {
      handleAnswerAck(message);
    } else if (message.type === "exam_state_response") {
      applyStateResponse(message);
    } else if (message.type === "exam_host_ready") {
      beginStateReconcile();
      flushAnswerBridge();
    }
  }

  function applyLanguage() {
    document.documentElement.lang = state.language;
    document.querySelectorAll("[data-i18n]").forEach((node) => {
      const value = text(node.dataset.i18n);
      if (typeof value === "string") node.textContent = value;
    });
    document.querySelectorAll("[data-i18n-placeholder]").forEach((node) => {
      node.placeholder = text(node.dataset.i18nPlaceholder);
    });
    renderQuestionList();
    renderQuestion();
  }

  function typeLabel(type) {
    return ({ multiple_choice: text("multipleChoice"), short_answer: text("shortAnswer"),
      essay: text("essay"), listening: text("listening"), reading: text("reading") })[type] || type;
  }

  function renderQuestionList() {
    const list = el("question-list");
    list.textContent = "";
    manifest.questions.forEach((item, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `question-button${index === state.index ? " active" : ""}${hasAnswer(item) ? " answered" : ""}`;
      button.textContent = String(index + 1);
      button.setAttribute("aria-label", `${text("question")} ${index + 1}`);
      button.addEventListener("click", () => { state.index = index; renderQuestionList(); renderQuestion(); });
      list.appendChild(button);
    });
    el("question-count").textContent = `${state.index + 1} / ${manifest.questions.length}`;
  }

  function hasAnswer(item) {
    const answer = state.answers[item.id];
    return Array.isArray(answer) ? answer.length > 0 : String(answer || "").trim().length > 0;
  }

  function renderQuestion() {
    const item = question();
    if (!item) return;
    el("question-position").textContent = `${text("question")} ${state.index + 1}`;
    el("question-type").textContent = typeLabel(item.type);
    el("question-points").textContent = `${item.points || 0} ${item.points === 1 ? text("point") : text("points")}`;
    const content = el("question-content");
    content.textContent = "";
    const heading = document.createElement("h2");
    heading.textContent = item.prompt || "";
    content.appendChild(heading);
    if (item.instruction) {
      const instruction = document.createElement("p");
      instruction.className = "question-instruction";
      instruction.textContent = item.instruction;
      content.appendChild(instruction);
    }
    if (item.passage) {
      const passage = document.createElement("div");
      passage.className = "reading-passage";
      passage.textContent = item.passage;
      content.appendChild(passage);
    }
    if (item.audio) {
      const audioUrl = resolveExamAsset(item.audio);
      if (!audioUrl) {
        setConnectionStatus(text("assetUnavailable"));
      }
      const audioBlock = document.createElement("div");
      audioBlock.className = "audio-block";
      const audio = document.createElement("audio");
      audio.controls = true;
      audio.src = audioUrl;
      audioBlock.appendChild(audio);
      if (item.transcript) {
        const transcript = document.createElement("details");
        const summary = document.createElement("summary");
        summary.textContent = "Transcript";
        transcript.append(summary, document.createTextNode(item.transcript));
        audioBlock.appendChild(transcript);
      }
      content.appendChild(audioBlock);
    }
    if (item.type === "multiple_choice" || item.type === "listening" || item.type === "reading") {
      renderOptions(content, item);
    } else if (item.type === "short_answer") {
      renderShortAnswer(content, item);
    } else if (item.type === "essay") {
      renderEssay(content, item);
    }
    document.querySelector('[data-action="previous"]').disabled = state.index === 0;
    document.querySelector('[data-action="next"]').disabled = state.index === manifest.questions.length - 1;
    updatePdf(item);
    updateSubmissionLock();
  }

  function renderOptions(parent, item) {
    const list = document.createElement("div");
    list.className = "option-list";
    if (!Array.isArray(item.options) || item.options.length === 0) {
      const unavailable = document.createElement("p");
      unavailable.className = "asset-unavailable";
      unavailable.textContent = text("assetUnavailable");
      list.appendChild(unavailable);
      setConnectionStatus(text("assetUnavailable"));
      parent.appendChild(list);
      return;
    }
    item.options.forEach((option, index) => {
      const label = document.createElement("label");
      label.className = "option-label";
      const input = document.createElement("input");
      input.type = "radio";
      input.name = item.id;
      input.value = String(index);
      input.checked = String(state.answers[item.id] || "") === String(index);
      input.addEventListener("change", () => saveAnswer(item.id, index));
      label.append(input, document.createTextNode(option));
      list.appendChild(label);
    });
    parent.appendChild(list);
  }

  function renderShortAnswer(parent, item) {
    const input = document.createElement("input");
    input.className = "answer-input";
    input.type = "text";
    input.placeholder = item.answerPlaceholder || "";
    input.value = state.answers[item.id] || "";
    input.addEventListener("input", () => saveAnswer(item.id, input.value));
    parent.appendChild(input);
  }

  function renderEssay(parent, item) {
    const area = document.createElement("textarea");
    area.className = "answer-textarea";
    area.placeholder = item.answerPlaceholder || text("writeResponse");
    area.value = state.answers[item.id] || "";
    const count = document.createElement("div");
    count.className = "answer-count";
    const updateCount = () => {
      const words = area.value.trim() ? area.value.trim().split(/\s+/).length : 0;
      count.textContent = `${words}${item.wordLimit ? ` / ${item.wordLimit}` : ""} words`;
    };
    area.addEventListener("input", () => { saveAnswer(item.id, area.value); updateCount(); });
    updateCount();
    parent.append(area, count);
  }

  function updatePdf(item) {
    const candidate = item.pdf || (manifestDocuments[0] && manifestDocuments[0].url) || "";
    const url = resolveExamAsset(candidate);
    el("pdf-frame").hidden = !url;
    el("pdf-empty").hidden = Boolean(url);
    el("pdf-frame").src = url;
    if (candidate && !url) {
      setConnectionStatus(text("assetUnavailable"));
    }
  }

  function saveAnswer(id, value) {
    let encodedValue;
    try { encodedValue = answerText(value); }
    catch (_) { encodedValue = null; }
    if (typeof encodedValue !== "string" ||
        utf8ByteLength(encodedValue) > MAX_ANSWER_BYTES) {
      state.syncError = "Answer exceeds the 16 KiB recovery limit.";
      setConnectionStatus(state.syncError);
      return false;
    }
    // Persist the write-ahead draft first.  If the answer-map write fails,
    // the newly entered value still has a durable recovery record; if the
    // draft cannot be persisted, do not advance the in-memory answer map.
    if (!queueAnswerDraft(id, value)) {
      markUnsaved();
      return false;
    }
    state.answers[id] = Array.isArray(value) ? value.slice() : value;
    let serialized;
    try { serialized = JSON.stringify(state.answers); }
    catch (_) { serialized = null; }
    if (typeof serialized !== "string" ||
        !writeStorage(`${storageKey}:answers`, serialized)) {
      markUnsaved();
      setConnectionStatus("Answer recovery storage is unavailable.");
      return false;
    }
    markSaved();
    renderQuestionList();
    return true;
  }

  function markUnsaved() {
    el("save-status").textContent = text("unsaved");
    window.clearTimeout(markSaved.timeout);
  }

  function durableAnswerPresent(id, value) {
    let encoded;
    try { encoded = answerText(value); } catch (_) { return false; }
    const draft = state.durable.drafts[id];
    if (draft) {
      try {
        if (answerText(draft.value) === encoded) return true;
      } catch (_) { /* Treat an invalid draft as not durable. */ }
    }
    const pending = state.durable.pending.filter((event) => event.questionId === id)
      .sort((left, right) => left.sequence - right.sequence);
    const latest = pending[pending.length - 1];
    if (!latest) return false;
    return (answerIsEmpty(value) ? latest.kind === "clear" : latest.kind === "upsert") &&
      latest.answer === encoded;
  }

  function ensureDurableAnswers() {
    for (const item of manifest.questions) {
      if (!item || typeof item.id !== "string" ||
          !Object.prototype.hasOwnProperty.call(state.answers, item.id)) continue;
      const value = state.answers[item.id];
      if (!durableAnswerPresent(item.id, value) && !queueAnswerDraft(item.id, value)) {
        return false;
      }
    }
    return true;
  }

  function markSaved() {
    el("save-status").textContent = text("saved");
    window.clearTimeout(markSaved.timeout);
    markSaved.timeout = window.setTimeout(() => { el("save-status").textContent = text("ready"); }, 1600);
  }

  function updateTimer() {
    const minutes = Math.floor(Math.max(0, state.remainingSeconds) / 60);
    const seconds = Math.max(0, state.remainingSeconds) % 60;
    el("timer").textContent = `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
    const timer = document.querySelector(".timer");
    timer.classList.toggle("warning", state.remainingSeconds <= 300 && state.remainingSeconds > 60);
    timer.classList.toggle("danger", state.remainingSeconds <= 60);
  }

  function tick() {
    if (state.submitted) return;
    const nextRemainingSeconds = Math.max(0, state.remainingSeconds - 1);
    if (!writeStorage(`${storageKey}:remaining`, String(nextRemainingSeconds))) {
      state.syncError = "Answer recovery storage is unavailable.";
      setConnectionStatus(state.syncError);
      return;
    }
    state.remainingSeconds = nextRemainingSeconds;
    updateTimer();
    if (state.remainingSeconds === 0) submitResponse();
  }

  function openSubmitDialog() {
    const answered = manifest.questions.filter(hasAnswer).length;
    el("submit-summary").textContent = I18N[state.language].submitSummary(answered, manifest.questions.length);
    el("submit-dialog").showModal();
  }

  function submitResponse() {
    if (state.submitted) return false;
    if (!bridgeReady()) {
      state.syncError = "Exam identity is unavailable.";
      setConnectionStatus(state.syncError);
      markUnsaved();
      return false;
    }
    if (!ensureDurableAnswers()) {
      markUnsaved();
      setConnectionStatus("Answer recovery storage is unavailable.");
      return false;
    }
    let serialized;
    try { serialized = JSON.stringify(state.answers); }
    catch (_) { serialized = null; }
    if (typeof serialized !== "string" ||
        !writeStorage(`${storageKey}:answers`, serialized)) {
      markUnsaved();
      setConnectionStatus("Answer recovery storage is unavailable.");
      return false;
    }
    const previousFinalizeRequested = state.durable.finalizeRequested;
    state.durable.finalizeRequested = true;
    if (!persistDurableState()) {
      state.durable.finalizeRequested = previousFinalizeRequested;
      markUnsaved();
      setConnectionStatus("Answer recovery storage is unavailable.");
      return false;
    }
    state.finalizeRequested = true;
    state.submitted = true;
    const response = {
      examId: manifest.id,
      packageDigestHex: examContext.packageDigestHex || "",
      clientIdHex: examContext.clientIdHex || "",
      sessionIdHex: examContext.sessionIdHex || "",
      candidate: examContext.candidateId || "",
      submittedAt: new Date().toISOString(),
      answers: state.answers,
      notes: state.notes
    };
    window.NSTU_EXAM_RESPONSE = response;
    el("save-status").textContent = text("submitted");
    document.querySelectorAll("button, input, textarea, select").forEach((node) => { node.disabled = true; });
    postHostMessage({
      type: "exam_submit", response,
      pendingAnswerEvents: state.durable.pending.length + Object.keys(state.durable.drafts).length
    });
    flushAnswerBridge();
    return true;
  }

  function downloadResponse() {
    const response = {
      examId: manifest.id,
      packageDigestHex: examContext.packageDigestHex || "",
      clientIdHex: examContext.clientIdHex || "",
      sessionIdHex: examContext.sessionIdHex || "",
      candidate: examContext.candidateId || "",
      exportedAt: new Date().toISOString(),
      answers: state.answers,
      notes: state.notes
    };
    const blob = new Blob([JSON.stringify(response, null, 2)], { type: "application/json" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `${manifest.id}-response.json`;
    link.click();
    URL.revokeObjectURL(link.href);
    el("save-status").textContent = text("exported");
  }

  function bindEvents() {
    document.querySelectorAll("[data-action]").forEach((button) => button.addEventListener("click", () => {
      const action = button.dataset.action;
      if (action === "settings") el("settings-dialog").showModal();
      if (action === "submit") openSubmitDialog();
      if (action === "download") downloadResponse();
      if (action === "previous" && state.index > 0) { state.index--; renderQuestionList(); renderQuestion(); }
      if (action === "next" && state.index < manifest.questions.length - 1) { state.index++; renderQuestionList(); renderQuestion(); }
    }));
    el("language-setting").value = state.language;
    el("language-setting").addEventListener("change", (event) => {
      state.language = event.target.value;
      writeStorage(`${storageKey}:language`, state.language);
      applyLanguage();
    });
    el("text-size-setting").addEventListener("change", (event) => {
      document.documentElement.style.setProperty("--scale", event.target.value);
    });
    el("contrast-setting").addEventListener("change", (event) => document.body.classList.toggle("high-contrast", event.target.checked));
    el("motion-setting").addEventListener("change", (event) => document.body.classList.toggle("reduce-motion", event.target.checked));
    el("notes").value = state.notes;
    el("notes").addEventListener("input", (event) => {
      state.notes = event.target.value;
      if (writeStorage(`${storageKey}:notes`, state.notes)) {
        markSaved();
      } else {
        markUnsaved();
        setConnectionStatus("Answer recovery storage is unavailable.");
      }
    });
    el("submit-form").addEventListener("submit", (event) => {
      if (event.submitter && event.submitter.value === "submit") submitResponse();
    });
    document.querySelectorAll("[data-reference]").forEach((tab) => tab.addEventListener("click", () => {
      const reference = tab.dataset.reference;
      document.querySelectorAll("[data-reference]").forEach((item) => { item.classList.toggle("active", item === tab); item.setAttribute("aria-selected", item === tab ? "true" : "false"); });
      el("pdf-pane").hidden = reference !== "pdf";
      el("notes-pane").hidden = reference !== "notes";
    }));
    document.addEventListener("keydown", (event) => {
      if (event.target.matches("input, textarea, select")) return;
      if (event.key === "ArrowLeft" && state.index > 0) { state.index--; renderQuestionList(); renderQuestion(); }
      if (event.key === "ArrowRight" && state.index < manifest.questions.length - 1) { state.index++; renderQuestionList(); renderQuestion(); }
    });
    const webview = hostWebView();
    if (webview && typeof webview.addEventListener === "function") {
      webview.addEventListener("message", handleHostMessage);
      postHostMessage({
        type: "exam_ready",
        context: {
          packageId: manifest.id,
          packageDigestHex: examContext.packageDigestHex,
          clientIdHex: examContext.clientIdHex,
          sessionIdHex: examContext.sessionIdHex,
          candidateId: examContext.candidateId
        },
        pendingAnswerEvents: state.durable.pending.length + Object.keys(state.durable.drafts).length
      });
      beginStateReconcile();
      flushAnswerBridge();
    }
  }

  el("exam-title").textContent = manifest.title || DEFAULT_MANIFEST.title;
  el("exam-subject").textContent = manifest.subject || DEFAULT_MANIFEST.subject;
  el("candidate-label").textContent = `${text("candidate")}: ${examContext.candidateId || "—"}`;
  bindEvents();
  applyLanguage();
  updateTimer();
  window.setInterval(tick, 1000);
}());
