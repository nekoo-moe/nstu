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
      notesPlaceholder: "Private notes for this session", sessionSettings: "Session settings",
      displayAndLanguage: "Display and language", language: "Language", textSize: "Text size",
      highContrast: "High contrast", reduceMotion: "Reduce motion", done: "Done",
      submitAssessment: "Submit assessment", submitPrompt: "Submit your answers now?",
      keepWorking: "Keep working", multipleChoice: "Multiple choice", shortAnswer: "Short answer",
      essay: "Essay", listening: "Listening comprehension", reading: "Reading comprehension",
      question: "Question", point: "point", points: "points", candidate: "Candidate",
      saved: "Saved", unsaved: "Saving...", complete: "complete", unanswered: "unanswered",
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
      notesPlaceholder: "Ghi chú riêng cho phiên này", sessionSettings: "Cài đặt phiên",
      displayAndLanguage: "Hiển thị và ngôn ngữ", language: "Ngôn ngữ", textSize: "Cỡ chữ",
      highContrast: "Tương phản cao", reduceMotion: "Giảm chuyển động", done: "Xong",
      submitAssessment: "Nộp bài đánh giá", submitPrompt: "Nộp câu trả lời ngay?",
      keepWorking: "Tiếp tục làm", multipleChoice: "Trắc nghiệm", shortAnswer: "Trả lời ngắn",
      essay: "Bài luận", listening: "Nghe hiểu", reading: "Đọc hiểu", question: "Câu",
      point: "điểm", points: "điểm", candidate: "Thí sinh", saved: "Đã lưu", unsaved: "Đang lưu...",
      complete: "hoàn thành", unanswered: "chưa trả lời", submitSummary: (answered, total) =>
        `Đã trả lời ${answered}/${total} câu.`, submitted: "Đã tạo gói câu trả lời.",
      exported: "Đã tải câu trả lời.", chooseOne: "Chọn một đáp án.", writeResponse: "Viết câu trả lời bên dưới."
    }
  };

  const manifest = window.NSTU_EXAM_MANIFEST || DEFAULT_MANIFEST;
  const storageKey = `nstu-exam:${manifest.id}:${manifest.candidate || "candidate"}`;
  function readStorage(key, fallback = "") {
    try { return localStorage.getItem(key) ?? fallback; }
    catch (_) { return fallback; }
  }
  function writeStorage(key, value) {
    try { localStorage.setItem(key, value); }
    catch (_) { /* Hosts may disable browser storage; server messages still work. */ }
  }
  const state = {
    language: readStorage(`${storageKey}:language`, "en"),
    index: 0,
    answers: loadJson(`${storageKey}:answers`, {}),
    notes: readStorage(`${storageKey}:notes`),
    remainingSeconds: Number(readStorage(`${storageKey}:remaining`)) || manifest.durationSeconds,
    submitted: false
  };
  if (!I18N[state.language]) state.language = "en";

  const el = (id) => document.getElementById(id);
  const text = (key) => (I18N[state.language][key] || I18N.en[key] || key);
  const question = () => manifest.questions[state.index];

  function loadJson(key, fallback) {
    try { return JSON.parse(readStorage(key, "null")) || fallback; }
    catch (_) { return fallback; }
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
      const audioBlock = document.createElement("div");
      audioBlock.className = "audio-block";
      const audio = document.createElement("audio");
      audio.controls = true;
      audio.src = item.audio;
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
  }

  function renderOptions(parent, item) {
    const list = document.createElement("div");
    list.className = "option-list";
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
    const url = item.pdf || (manifest.documents[0] && manifest.documents[0].url) || "";
    el("pdf-frame").hidden = !url;
    el("pdf-empty").hidden = Boolean(url);
    if (url) el("pdf-frame").src = url;
  }

  function saveAnswer(id, value) {
    state.answers[id] = value;
    writeStorage(`${storageKey}:answers`, JSON.stringify(state.answers));
    markSaved();
    renderQuestionList();
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
    state.remainingSeconds = Math.max(0, state.remainingSeconds - 1);
    writeStorage(`${storageKey}:remaining`, String(state.remainingSeconds));
    updateTimer();
    if (state.remainingSeconds === 0) submitResponse();
  }

  function openSubmitDialog() {
    const answered = manifest.questions.filter(hasAnswer).length;
    el("submit-summary").textContent = I18N[state.language].submitSummary(answered, manifest.questions.length);
    el("submit-dialog").showModal();
  }

  function submitResponse() {
    state.submitted = true;
    const response = { examId: manifest.id, candidate: manifest.candidate || "", submittedAt: new Date().toISOString(), answers: state.answers, notes: state.notes };
    window.NSTU_EXAM_RESPONSE = response;
    el("save-status").textContent = text("submitted");
    document.querySelectorAll("button, input, textarea, select").forEach((node) => { node.disabled = true; });
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage({ type: "exam_submit", response });
    }
  }

  function downloadResponse() {
    const response = { examId: manifest.id, candidate: manifest.candidate || "", exportedAt: new Date().toISOString(), answers: state.answers, notes: state.notes };
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
      writeStorage(`${storageKey}:notes`, state.notes);
      markSaved();
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
  }

  el("exam-title").textContent = manifest.title || DEFAULT_MANIFEST.title;
  el("exam-subject").textContent = manifest.subject || DEFAULT_MANIFEST.subject;
  el("candidate-label").textContent = `${text("candidate")}: ${manifest.candidate || "—"}`;
  bindEvents();
  applyLanguage();
  updateTimer();
  window.setInterval(tick, 1000);
}());
