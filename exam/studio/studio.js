/**
 * NSTU Exam Studio - Authoring Engine & IELTS Split-Screen Controller
 */

(function () {
  "use strict";

  // Default sample exam modeled after computer-delivered IELTS
  const DEFAULT_EXAM = {
    id: "ielts-sample-01",
    title: "IELTS Academic Reading & Listening Mock Assessment",
    subject: "English Language",
    durationSeconds: 3600, // 60 minutes
    documents: [],
    questions: [
      {
        id: "q1-matching-headings",
        type: "reading",
        points: 4,
        prompt: "Choose the correct heading for Paragraphs A–D from the list of headings below.",
        instruction: "Drag each heading into the appropriate paragraph answer slot, or click a heading then click a slot to place it.",
        passage: "[Paragraph A]\nThe initial exploration of the southern polar continent began in the early nineteenth century with maritime expeditions probing the pack ice. Early navigators braved uncharted waters, fierce katabatic winds, and freezing temperatures without reliable navigational charts.\n\n[Paragraph B]\nEquipping polar expeditions proved to be the single greatest logistical challenge. Teams required specialized high-calorie rations, draft animals such as Manchurian ponies and Greenland sled dogs, and windproof canvas tents capable of withstanding blizzards.\n\n[Paragraph C]\nBy the mid-twentieth century, seasonal exploration gave way to permanent scientific habitations. Modern research outposts now maintain sophisticated meteorological stations, satellite tracking dishes, and subglacial laboratories operating year-round.\n\n[Paragraph D]\nInternational diplomacy culminated in the landmark Antarctic Treaty, signed in Washington in 1959. The treaty established the continent as a scientific preserve, strictly banned all military activity, and postponed indefinitely all national territorial claims.",
        isMatching: true,
        options: [
          "i. Early maritime voyages and extreme survival conditions",
          "ii. Logistical hurdles and specialized expedition gear",
          "iii. Permanent research stations and scientific facilities",
          "iv. Diplomatic agreements and international stewardship",
          "v. Commercial mining and exploitation of subglacial fuel reserves"
        ],
        matchingTargets: [
          "Paragraph A",
          "Paragraph B",
          "Paragraph C",
          "Paragraph D"
        ]
      },
      {
        id: "q2-reading-mc",
        type: "reading",
        points: 1,
        prompt: "According to Paragraph B, what was the primary obstacle facing early expedition planners?",
        instruction: "Choose the correct letter, A, B, C, or D.",
        passage: "[Paragraph A]\nThe initial exploration of the southern polar continent began in the early nineteenth century with maritime expeditions probing the pack ice...\n\n[Paragraph B]\nEquipping polar expeditions proved to be the single greatest logistical challenge. Teams required specialized high-calorie rations, draft animals such as Manchurian ponies and Greenland sled dogs, and windproof canvas tents capable of withstanding blizzards.",
        isMatching: false,
        options: [
          "A. Securing government permission to sail into Antarctic waters",
          "B. Procuring sufficient provisions, specialized equipment, and draft animals",
          "C. Overcoming language barriers between international expedition members",
          "D. Navigating through densely populated sub-Antarctic islands"
        ]
      },
      {
        id: "q3-listening-announcement",
        type: "listening",
        points: 2,
        prompt: "Listen to the polar station orientation briefing and answer the question.",
        instruction: "What mandatory procedure must all researchers complete before leaving the base perimeter?",
        audio: "media/listening-01.mp3",
        options: [
          "Sign the radio logbook and carry a dual-frequency transceiver",
          "Obtain written authorization from the expedition commander",
          "Wait for wind speeds to drop below five knots",
          "Check out an emergency snowmobile with auxiliary fuel tanks"
        ]
      },
      {
        id: "q4-academic-writing",
        type: "essay",
        points: 8,
        prompt: "Some individuals believe that extreme wilderness regions like Antarctica should be completely closed to human activity, while others maintain that scientific exploration brings vital knowledge for planet Earth. Discuss both views and give your own opinion.",
        instruction: "Write at least 250 words in clear academic English.",
        wordLimit: 300,
        answerPlaceholder: "Type your essay response here..."
      }
    ]
  };

  // State
  let currentExam = JSON.parse(JSON.stringify(DEFAULT_EXAM));
  let activeQuestionIndex = 0;
  let previewAnswers = {}; // { qId: { targetIdx: tokenIdx } or value }
  let selectedTokenForPlacement = null; // For click-to-place fallback

  // DOM Elements
  const el = (id) => document.getElementById(id);

  function showToast(message, type = "info") {
    const container = el("toast-container");
    const toast = document.createElement("div");
    toast.className = `toast ${type}`;
    toast.textContent = message;
    container.appendChild(toast);
    setTimeout(() => {
      toast.style.opacity = "0";
      setTimeout(() => toast.remove(), 250);
    }, 3200);
  }

  /* ========================================================================
     Navigation & Mode Switching
     ======================================================================== */

  function setMode(mode) {
    const editorView = el("editor-view");
    const previewView = el("preview-view");
    const btnEditor = el("btn-mode-editor");
    const btnPreview = el("btn-mode-preview");

    if (mode === "preview") {
      editorView.style.display = "none";
      previewView.style.display = "flex";
      btnEditor.classList.remove("active");
      btnPreview.classList.add("active");
      renderPreview();
    } else {
      previewView.style.display = "none";
      editorView.style.display = "flex";
      btnPreview.classList.remove("active");
      btnEditor.classList.add("active");
      renderEditor();
    }
  }

  /* ========================================================================
     Editor: Render and Synchronize
     ======================================================================== */

  function renderEditor() {
    // Bind exam settings
    el("exam-id").value = currentExam.id || "";
    el("exam-title").value = currentExam.title || "";
    el("exam-subject").value = currentExam.subject || "";
    el("exam-duration").value = Math.round((currentExam.durationSeconds || 3600) / 60);
    el("exam-allowed-origins").value = Array.isArray(currentExam.allowedOrigins)
      ? currentExam.allowedOrigins.join("\n")
      : "";

    // Questions rail
    const navList = el("question-nav-list");
    navList.innerHTML = "";
    el("question-count-badge").textContent = currentExam.questions.length;

    currentExam.questions.forEach((q, idx) => {
      const item = document.createElement("div");
      item.className = `question-nav-item ${idx === activeQuestionIndex ? "active" : ""}`;
      
      let badgeType = q.type;
      let badgeLabel = q.type;
      if (q.isMatching) {
        badgeType = "matching";
        badgeLabel = "Matching";
      } else if (q.type === "multiple_choice") {
        badgeLabel = "MC";
      } else if (q.type === "short_answer") {
        badgeLabel = "Short";
      } else if (q.type === "essay") {
        badgeLabel = "Essay";
      } else if (q.type === "listening") {
        badgeLabel = "Audio";
      } else if (q.type === "reading") {
        badgeLabel = "Reading";
      }

      item.innerHTML = `
        <div class="nav-item-left">
          <span class="nav-item-index">${idx + 1}</span>
          <span class="nav-item-type-badge ${badgeType}">${badgeLabel}</span>
          <span class="nav-item-title">${escapeHtml(q.prompt || "Untitled Question")}</span>
        </div>
        <div class="nav-item-actions">
          <button class="icon-btn delete" type="button" title="Delete question" data-delete="${idx}">🗑️</button>
        </div>
      `;

      item.addEventListener("click", (e) => {
        if (e.target.closest("[data-delete]")) {
          deleteQuestion(idx);
          return;
        }
        activeQuestionIndex = idx;
        renderEditor();
      });

      navList.appendChild(item);
    });

    renderActiveQuestionForm();
  }

  function renderActiveQuestionForm() {
    const q = currentExam.questions[activeQuestionIndex];
    if (!q) return;

    el("active-q-title").textContent = `Question ${activeQuestionIndex + 1}`;
    el("q-points").value = q.points || 1;
    el("q-prompt").value = q.prompt || "";
    el("q-instruction").value = q.instruction || "";

    // Badge
    const badge = el("active-q-badge");
    badge.className = "nav-item-type-badge " + (q.isMatching ? "matching" : q.type);
    badge.textContent = q.isMatching ? "Matching (IELTS Drag & Drop)" : q.type.toUpperCase();

    // Stimulus (Passage)
    const stimulus = el("stimulus-container");
    if (q.type === "reading" || q.isMatching) {
      stimulus.style.display = "block";
      el("q-passage").value = q.passage || "";
    } else {
      stimulus.style.display = "none";
    }

    // Audio container
    const audioContainer = el("audio-container");
    if (q.type === "listening") {
      audioContainer.style.display = "block";
      el("q-audio-path").value = q.audio || "";
    } else {
      audioContainer.style.display = "none";
    }

    // Type sections
    el("matching-editor").style.display = q.isMatching ? "grid" : "none";
    el("mc-editor").style.display = (!q.isMatching && (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading")) ? "block" : "none";
    el("sa-editor").style.display = (q.type === "short_answer") ? "block" : "none";
    el("essay-editor").style.display = (q.type === "essay") ? "block" : "none";

    // Populate matching tokens & targets
    if (q.isMatching) {
      renderTokenEditor(q);
    } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
      renderMcOptionEditor(q);
    } else if (q.type === "short_answer") {
      el("q-sa-placeholder").value = q.answerPlaceholder || "";
    } else if (q.type === "essay") {
      el("q-essay-limit").value = q.wordLimit || 250;
    }
  }

  function renderTokenEditor(q) {
    if (!Array.isArray(q.options)) q.options = [];
    if (!Array.isArray(q.matchingTargets)) q.matchingTargets = [];

    // Tokens list
    const tokenList = el("token-input-list");
    tokenList.innerHTML = "";
    q.options.forEach((opt, idx) => {
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">#${idx + 1}</span>
        <input class="form-input" type="text" value="${escapeHtml(opt)}" data-token-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-token="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.options[idx] = e.target.value;
      });
      row.querySelector("[data-del-token]").addEventListener("click", () => {
        q.options.splice(idx, 1);
        renderTokenEditor(q);
      });
      tokenList.appendChild(row);
    });

    // Targets list
    const targetList = el("target-input-list");
    targetList.innerHTML = "";
    q.matchingTargets.forEach((tgt, idx) => {
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">Target ${idx + 1}</span>
        <input class="form-input" type="text" value="${escapeHtml(tgt)}" data-target-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-target="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.matchingTargets[idx] = e.target.value;
      });
      row.querySelector("[data-del-target]").addEventListener("click", () => {
        q.matchingTargets.splice(idx, 1);
        renderTokenEditor(q);
      });
      targetList.appendChild(row);
    });
  }

  function renderMcOptionEditor(q) {
    if (!Array.isArray(q.options)) q.options = [];
    const list = el("mc-option-list");
    list.innerHTML = "";
    q.options.forEach((opt, idx) => {
      const letter = String.fromCharCode(65 + idx);
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">${letter}.</span>
        <input class="form-input" type="text" value="${escapeHtml(opt)}" data-mc-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-mc="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.options[idx] = e.target.value;
      });
      row.querySelector("[data-del-mc]").addEventListener("click", () => {
        q.options.splice(idx, 1);
        renderMcOptionEditor(q);
      });
      list.appendChild(row);
    });
  }

  function addQuestion(type) {
    const nextNum = currentExam.questions.length + 1;
    let newQ;

    if (type === "matching") {
      newQ = {
        id: `q${nextNum}-matching`,
        type: "reading",
        points: 4,
        prompt: `Questions ${nextNum}: Match each heading to the appropriate section.`,
        instruction: "Drag each heading into the target slot, or click to place.",
        passage: `[Paragraph A]\nInsert your reading passage here...\n\n[Paragraph B]\nSecond paragraph text...`,
        isMatching: true,
        options: [
          "i. First heading description",
          "ii. Second heading description",
          "iii. Third heading description"
        ],
        matchingTargets: [
          "Paragraph A",
          "Paragraph B"
        ]
      };
    } else if (type === "reading") {
      newQ = {
        id: `q${nextNum}-reading`,
        type: "reading",
        points: 1,
        prompt: `According to the passage, what is the author's primary conclusion?`,
        instruction: "Choose one answer.",
        passage: `[Paragraph A]\nInsert your reading passage here...`,
        isMatching: false,
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    } else if (type === "multiple_choice") {
      newQ = {
        id: `q${nextNum}-mc`,
        type: "multiple_choice",
        points: 1,
        prompt: "Choose the statement that best answers the question.",
        instruction: "Select one option.",
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    } else if (type === "short_answer") {
      newQ = {
        id: `q${nextNum}-short-answer`,
        type: "short_answer",
        points: 1,
        prompt: "Complete the sentence with NO MORE THAN TWO WORDS.",
        instruction: "Write your answer in the box below.",
        answerPlaceholder: "Type your answer here..."
      };
    } else if (type === "essay") {
      newQ = {
        id: `q${nextNum}-essay`,
        type: "essay",
        points: 8,
        prompt: "Discuss the advantages and disadvantages of this development.",
        instruction: "Write at least 250 words.",
        wordLimit: 250,
        answerPlaceholder: "Write your essay response here..."
      };
    } else if (type === "listening") {
      newQ = {
        id: `q${nextNum}-listening`,
        type: "listening",
        points: 1,
        prompt: "Listen to the recording and choose the correct answer.",
        instruction: "Select one option.",
        audio: "media/listening-01.mp3",
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    }

    currentExam.questions.push(newQ);
    activeQuestionIndex = currentExam.questions.length - 1;
    renderEditor();
    showToast(`Added Question ${nextNum}`, "success");
  }

  function deleteQuestion(idx) {
    if (currentExam.questions.length <= 1) {
      showToast("An exam must have at least one question.", "error");
      return;
    }
    currentExam.questions.splice(idx, 1);
    if (activeQuestionIndex >= currentExam.questions.length) {
      activeQuestionIndex = currentExam.questions.length - 1;
    }
    renderEditor();
    showToast("Question deleted", "info");
  }

  /* ========================================================================
     IELTS Live Preview: Split Screen & Drag-and-Drop
     ======================================================================== */

  let previewIndex = 0;

  function renderPreview() {
    el("preview-exam-title").textContent = currentExam.title || "IELTS Assessment Preview";
    const minutes = Math.round((currentExam.durationSeconds || 3600) / 60);
    el("preview-timer").textContent = `${String(minutes).padStart(2, "0")}:00`;

    const q = currentExam.questions[previewIndex];
    if (!q) return;

    el("p-nav-counter").textContent = `Question ${previewIndex + 1} of ${currentExam.questions.length}`;
    el("btn-p-prev").disabled = previewIndex === 0;
    el("btn-p-next").disabled = previewIndex === currentExam.questions.length - 1;

    // Header info
    el("p-question-badge").textContent = q.isMatching ? "IELTS Matching" : q.type.toUpperCase();
    el("p-question-points").textContent = `${q.points || 1} points`;
    el("p-question-prompt").textContent = q.prompt || "";
    el("p-question-instruction").textContent = q.instruction || "";

    // Left Column: Stimulus Passage
    const passageContent = el("p-passage-content");
    const audioContainer = el("p-audio-container");
    const leftPane = el("preview-pane-left");

    if (q.passage) {
      leftPane.style.display = "block";
      passageContent.innerHTML = formatIeltsParagraphs(q.passage);
    } else if (q.audio) {
      leftPane.style.display = "block";
      passageContent.innerHTML = `<p style="color: var(--text-muted); font-style: italic;">Listen to the audio track below while completing the questions on the right.</p>`;
    } else {
      // If there's no passage/stimulus, expand the right column
      leftPane.style.display = "none";
    }

    if (q.audio) {
      audioContainer.style.display = "block";
      el("p-audio-player").src = q.audio;
    } else {
      audioContainer.style.display = "none";
    }

    // Right Column: Interactive Answer Surface
    const matchingSurface = el("p-matching-surface");
    const mcSurface = el("p-mc-surface");
    const saSurface = el("p-sa-surface");
    const essaySurface = el("p-essay-surface");

    matchingSurface.style.display = "none";
    mcSurface.style.display = "none";
    saSurface.style.display = "none";
    essaySurface.style.display = "none";

    if (q.isMatching) {
      matchingSurface.style.display = "block";
      renderPreviewMatching(q);
    } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
      mcSurface.style.display = "flex";
      renderPreviewMc(q);
    } else if (q.type === "short_answer") {
      saSurface.style.display = "block";
      const input = el("p-sa-input");
      input.placeholder = q.answerPlaceholder || "Type your answer here...";
      input.value = previewAnswers[q.id] || "";
      input.oninput = (e) => { previewAnswers[q.id] = e.target.value; };
    } else if (q.type === "essay") {
      essaySurface.style.display = "block";
      const area = el("p-essay-input");
      area.value = previewAnswers[q.id] || "";
      updateEssayWordCount(area.value, q.wordLimit);
      area.oninput = (e) => {
        previewAnswers[q.id] = e.target.value;
        updateEssayWordCount(e.target.value, q.wordLimit);
      };
    }
  }

  function formatIeltsParagraphs(text) {
    if (!text) return "";
    const lines = text.split(/\n\s*\n/);
    return lines.map(line => {
      const trimmed = line.trim();
      if (!trimmed) return "";
      // Check for [Paragraph A] or [A] marker
      const markerMatch = trimmed.match(/^\[(Paragraph\s+[A-Za-z0-9]+|[A-Za-z0-9]+)\]\s*(.*)/i);
      if (markerMatch) {
        return `<p class="ielts-paragraph"><span class="paragraph-marker">${escapeHtml(markerMatch[1])}</span>${escapeHtml(markerMatch[2])}</p>`;
      }
      return `<p class="ielts-paragraph">${escapeHtml(trimmed)}</p>`;
    }).join("");
  }

  function updateEssayWordCount(text, limit) {
    const words = text.trim() ? text.trim().split(/\s+/).length : 0;
    el("p-essay-count").textContent = `${words}${limit ? ` / ${limit}` : ""} words`;
  }

  /* ========================================================================
     Drag-and-Drop & Matching Engine (with Click-to-Place Fallback)
     ======================================================================== */

  function renderPreviewMatching(q) {
    if (!previewAnswers[q.id]) {
      previewAnswers[q.id] = {}; // { targetIdx: tokenIdx }
    }
    const currentAssignments = previewAnswers[q.id];

    // Identify which tokens are already placed
    const placedTokenIndices = new Set(Object.values(currentAssignments));

    // 1. Render Token Tray
    const tray = el("p-token-tray");
    tray.innerHTML = "";

    q.options.forEach((optText, tokenIdx) => {
      if (placedTokenIndices.has(tokenIdx)) return; // Already placed in a slot

      const chip = document.createElement("div");
      chip.className = "drag-chip";
      chip.draggable = true;
      chip.dataset.tokenIdx = tokenIdx;
      chip.innerHTML = `
        <span class="chip-handle-icon">⠿</span>
        <span>${escapeHtml(optText)}</span>
      `;

      if (selectedTokenForPlacement === tokenIdx) {
        chip.classList.add("selected-for-click");
      }

      // Drag events
      chip.addEventListener("dragstart", (e) => {
        chip.classList.add("is-dragging");
        e.dataTransfer.setData("text/plain", String(tokenIdx));
        e.dataTransfer.effectAllowed = "move";
      });

      chip.addEventListener("dragend", () => {
        chip.classList.remove("is-dragging");
      });

      // Click to select (fallback for trackpad / touch)
      chip.addEventListener("click", () => {
        if (selectedTokenForPlacement === tokenIdx) {
          selectedTokenForPlacement = null;
        } else {
          selectedTokenForPlacement = tokenIdx;
        }
        renderPreviewMatching(q);
      });

      tray.appendChild(chip);
    });

    if (tray.children.length === 0) {
      tray.innerHTML = `<span style="font-size: 12px; color: var(--text-faint); padding: 8px;">All headings have been placed in slots.</span>`;
    }

    // 2. Render Target Slots
    const targetList = el("p-target-list");
    targetList.innerHTML = "";

    (q.matchingTargets || []).forEach((tgtLabel, targetIdx) => {
      const row = document.createElement("div");
      row.className = "target-row";

      const labelBox = document.createElement("div");
      labelBox.className = "target-label-box";
      labelBox.textContent = tgtLabel;

      const slot = document.createElement("div");
      slot.className = "target-drop-slot";
      slot.dataset.targetIdx = targetIdx;

      const assignedTokenIdx = currentAssignments[targetIdx];

      if (assignedTokenIdx !== undefined && assignedTokenIdx !== null) {
        // Slot is filled
        slot.classList.add("filled");
        const tokenText = q.options[assignedTokenIdx] || "";
        slot.innerHTML = `
          <div class="placed-chip">
            <span>${escapeHtml(tokenText)}</span>
            <button class="chip-remove-btn" type="button" title="Remove heading back to tray">✕</button>
          </div>
        `;
        slot.querySelector(".chip-remove-btn").addEventListener("click", (e) => {
          e.stopPropagation();
          delete currentAssignments[targetIdx];
          renderPreviewMatching(q);
        });
      } else {
        // Slot is empty
        slot.innerHTML = `<span class="slot-placeholder-text">Drop heading here or click to assign</span>`;
      }

      // Drag Over / Leave
      slot.addEventListener("dragover", (e) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = "move";
        slot.classList.add("drag-over");
      });

      slot.addEventListener("dragleave", () => {
        slot.classList.remove("drag-over");
      });

      // Drop
      slot.addEventListener("drop", (e) => {
        e.preventDefault();
        slot.classList.remove("drag-over");
        const tokenIdxStr = e.dataTransfer.getData("text/plain");
        if (tokenIdxStr !== "") {
          const tIdx = parseInt(tokenIdxStr, 10);
          assignTokenToSlot(q, targetIdx, tIdx);
        }
      });

      // Click to place
      slot.addEventListener("click", () => {
        if (selectedTokenForPlacement !== null) {
          assignTokenToSlot(q, targetIdx, selectedTokenForPlacement);
          selectedTokenForPlacement = null;
        } else if (assignedTokenIdx !== undefined) {
          // If clicked on an already filled slot without a token selected, remove it
          delete currentAssignments[targetIdx];
          renderPreviewMatching(q);
        }
      });

      row.append(labelBox, slot);
      targetList.appendChild(row);
    });
  }

  function assignTokenToSlot(q, targetIdx, tokenIdx) {
    const assignments = previewAnswers[q.id];
    // If this token was already in another slot, remove it from that slot first
    for (const [sKey, tVal] of Object.entries(assignments)) {
      if (tVal === tokenIdx) {
        delete assignments[sKey];
      }
    }
    assignments[targetIdx] = tokenIdx;
    renderPreviewMatching(q);
  }

  function renderPreviewMc(q) {
    const surface = el("p-mc-surface");
    surface.innerHTML = "";
    const selected = previewAnswers[q.id];

    (q.options || []).forEach((optText, optIdx) => {
      const label = document.createElement("label");
      label.className = `mc-option-label ${selected === optIdx ? "checked" : ""}`;
      label.innerHTML = `
        <div class="mc-radio-circle"></div>
        <span>${escapeHtml(optText)}</span>
      `;
      label.addEventListener("click", () => {
        previewAnswers[q.id] = optIdx;
        renderPreviewMc(q);
      });
      surface.appendChild(label);
    });
  }

  /* ========================================================================
     Splitter Resizer Logic
     ======================================================================== */

  function initSplitter() {
    const splitter = el("preview-splitter");
    const container = el("preview-split-container");
    const leftPane = el("preview-pane-left");

    let isDragging = false;

    splitter.addEventListener("mousedown", (e) => {
      isDragging = true;
      splitter.classList.add("active");
      document.body.style.cursor = "col-resize";
      e.preventDefault();
    });

    window.addEventListener("mousemove", (e) => {
      if (!isDragging) return;
      const rect = container.getBoundingClientRect();
      const offset = e.clientX - rect.left;
      const percentage = (offset / rect.width) * 100;
      if (percentage >= 25 && percentage <= 75) {
        leftPane.style.flex = "none";
        leftPane.style.width = `${percentage}%`;
      }
    });

    window.addEventListener("mouseup", () => {
      if (isDragging) {
        isDragging = false;
        splitter.classList.remove("active");
        document.body.style.cursor = "";
      }
    });
  }

  /* ========================================================================
     Validation & Packaging (.nstuexam Export)
     ======================================================================== */

  function parseOriginsInput(raw) {
    if (!raw || typeof raw !== "string") return [];
    const lines = raw.split(/[\r\n,]+/);
    const seen = new Set();
    const origins = [];
    for (const item of lines) {
      const trimmed = item.trim();
      if (!trimmed) continue;
      if (!seen.has(trimmed)) {
        seen.add(trimmed);
        origins.push(trimmed);
      }
    }
    return origins;
  }

  function validateOrigin(origin) {
    if (typeof origin !== "string" || origin.length < 1 || origin.length > 253) return false;
    if (origin.includes("@")) return false; // no userinfo
    let host = origin;
    if (/^https?:\/\//i.test(host)) {
      host = host.replace(/^https?:\/\//i, "");
    } else if (host.includes("://")) {
      return false; // unsupported scheme
    }
    const cut = host.search(/[\/?#]/);
    if (cut !== -1) {
      host = host.substring(0, cut);
    }
    const colon = host.lastIndexOf(":");
    if (colon !== -1) {
      const port = host.substring(colon + 1);
      if (!/^\d{1,5}$/.test(port) || Number(port) < 1 || Number(port) > 65535) return false;
      host = host.substring(0, colon);
    }
    if (!host || host.length > 253) return false;
    const labels = host.split(".");
    for (const label of labels) {
      if (!label || label.length > 63) return false;
      if (!/^[a-zA-Z0-9-]+$/.test(label)) return false;
      if (label.startsWith("-") || label.endsWith("-")) return false;
    }
    return true;
  }

  function validateExam(exam) {
    const errors = [];
    if (!exam.id || !/^[a-zA-Z0-9._-]{1,96}$/.test(exam.id)) {
      errors.push("Package ID must be 1–96 alphanumeric characters (dashes and underscores allowed).");
    }
    if (!exam.title || exam.title.length < 1 || exam.title.length > 240) {
      errors.push("Assessment title must be between 1 and 240 characters.");
    }
    if (!exam.durationSeconds || exam.durationSeconds < 60 || exam.durationSeconds > 86400) {
      errors.push("Duration must be between 1 minute and 24 hours.");
    }
    if (exam.allowedOrigins) {
      if (!Array.isArray(exam.allowedOrigins)) {
        errors.push("Allowed origins must be a list.");
      } else if (exam.allowedOrigins.length > 32) {
        errors.push("Allowed origins list cannot exceed 32 items.");
      } else {
        const seen = new Set();
        exam.allowedOrigins.forEach((orig, idx) => {
          if (seen.has(orig)) {
            errors.push(`Duplicate origin declared: ${orig}`);
          }
          seen.add(orig);
          if (!validateOrigin(orig)) {
            errors.push(`Allowed origin #${idx + 1} (${orig}) is invalid. Must be a bare hostname or http(s) URL (1–253 chars, no userinfo).`);
          }
        });
      }
    }
    if (!Array.isArray(exam.questions) || exam.questions.length === 0) {
      errors.push("Exam must contain at least one question.");
    } else if (exam.questions.length > 500) {
      errors.push("Exam exceeds the 500 questions quota limit.");
    }

    exam.questions.forEach((q, idx) => {
      const qNum = idx + 1;
      if (!q.id || !/^[a-zA-Z0-9._-]{1,96}$/.test(q.id)) {
        errors.push(`Question #${qNum} ID is invalid.`);
      }
      if (!q.prompt || q.prompt.trim().length === 0) {
        errors.push(`Question #${qNum} has an empty prompt.`);
      }
      if (q.isMatching) {
        if (!q.options || q.options.length < 2) {
          errors.push(`Question #${qNum} (Matching) must have at least 2 draggable items in the token bank.`);
        }
        if (!q.matchingTargets || q.matchingTargets.length < 1) {
          errors.push(`Question #${qNum} (Matching) must have at least 1 target slot.`);
        }
      } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
        if (!q.options || q.options.length < 2) {
          errors.push(`Question #${qNum} must have at least 2 answer options.`);
        }
      }
    });

    return errors;
  }

  function compileManifest(exam) {
    // Generate clean manifest matching manifest.schema.json
    const manifest = {
      id: exam.id,
      title: exam.title,
      subject: exam.subject || "General",
      durationSeconds: exam.durationSeconds,
      documents: exam.documents || [],
      questions: exam.questions.map(q => {
        const item = {
          id: q.id,
          type: q.type,
          prompt: q.prompt,
          instruction: q.instruction || "",
          points: Number(q.points) || 1
        };
        if (q.passage) item.passage = q.passage;
        if (q.options && q.options.length > 0) item.options = q.options;
        if (q.audio) item.audio = q.audio;
        if (q.wordLimit) item.wordLimit = Number(q.wordLimit);
        if (q.answerPlaceholder) item.answerPlaceholder = q.answerPlaceholder;
        return item;
      })
    };
    if (Array.isArray(exam.allowedOrigins) && exam.allowedOrigins.length > 0) {
      manifest.allowedOrigins = exam.allowedOrigins;
    }
    return manifest;
  }

  async function exportPackage() {
    // Sync current values from editor inputs
    currentExam.id = el("exam-id").value.trim();
    currentExam.title = el("exam-title").value.trim();
    currentExam.subject = el("exam-subject").value.trim();
    currentExam.durationSeconds = Math.max(60, parseInt(el("exam-duration").value, 10) * 60 || 3600);
    const origins = parseOriginsInput(el("exam-allowed-origins").value);
    if (origins.length > 0) {
      currentExam.allowedOrigins = origins;
    } else {
      delete currentExam.allowedOrigins;
    }

    const modal = el("export-modal");
    const statusBox = el("export-validation-status");
    const hashesBox = el("export-hashes");
    const btnDownload = el("btn-download-nstuexam");

    modal.classList.add("open");
    statusBox.style.color = "var(--text-main)";
    statusBox.innerHTML = "⏳ Validating exam schema and compiling package...";
    hashesBox.style.display = "none";
    btnDownload.disabled = true;

    const errors = validateExam(currentExam);
    if (errors.length > 0) {
      statusBox.style.color = "var(--danger)";
      statusBox.innerHTML = `<strong>Validation Failed (${errors.length} errors):</strong><ul style="margin: 8px 0 0 18px;">${errors.map(e => `<li>${escapeHtml(e)}</li>`).join("")}</ul>`;
      return;
    }

    try {
      const manifestObj = compileManifest(currentExam);
      const manifestJson = JSON.stringify(manifestObj, null, 2);

      const zip = new NstuZip.Writer();
      zip.add("manifest.json", manifestJson);

      // Add web runner files from template.js or relative fetch
      const runner = window.NSTU_RUNNER_TEMPLATE || {};
      if (runner.indexHtml && runner.stylesCss && runner.appJs) {
        zip.add("exam/web/index.html", runner.indexHtml);
        zip.add("exam/web/styles.css", runner.stylesCss);
        zip.add("exam/web/app.js", runner.appJs);
      } else {
        // Fallback fetch
        const [hRes, cRes, jRes] = await Promise.all([
          fetch("../web/index.html").then(r => r.text()),
          fetch("../web/styles.css").then(r => r.text()),
          fetch("../web/app.js").then(r => r.text())
        ]);
        zip.add("exam/web/index.html", hRes);
        zip.add("exam/web/styles.css", cRes);
        zip.add("exam/web/app.js", jRes);
      }

      const result = await zip.build();

      statusBox.style.color = "var(--success)";
      statusBox.innerHTML = `✓ <strong>Exam Package Validated & Compiled Successfully!</strong><br><span style="font-size: 12px; color: var(--text-muted);">${result.fileCount} files · ${Math.round(result.totalBytes / 1024)} KiB</span>`;

      el("lbl-archive-hash").textContent = result.archiveSha256;
      el("lbl-content-hash").textContent = result.contentSha256;

      const stageCmd = `& stage-exam-package.ps1 -ArchivePath "$env:TEMP\\${currentExam.id}.nstuexam" -PublishRoot "$env:ProgramData\\NSTU\\exams\\packages" -ExpectedArchiveSha256 "${result.archiveSha256}" -ExpectedContentSha256 "${result.contentSha256}" -AllowUnsigned`;
      el("lbl-stage-command").textContent = stageCmd;

      hashesBox.style.display = "flex";
      btnDownload.disabled = false;

      btnDownload.onclick = () => {
        const url = URL.createObjectURL(result.blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = `${currentExam.id}.nstuexam`;
        a.click();
        URL.revokeObjectURL(url);
        showToast("Package downloaded successfully", "success");
      };

    } catch (err) {
      statusBox.style.color = "var(--danger)";
      statusBox.innerHTML = `<strong>Build Error:</strong> ${escapeHtml(err.message)}`;
    }
  }

  /* ========================================================================
     Import & Save Draft
     ======================================================================== */

  async function handleImportFile(file) {
    try {
      if (file.name.endsWith(".json")) {
        const text = await file.text();
        const data = JSON.parse(text);
        if (!data.id || !data.questions) throw new Error("Invalid manifest JSON structure.");
        currentExam = data;
        activeQuestionIndex = 0;
        renderEditor();
        showToast("Manifest imported successfully", "success");
      } else if (file.name.endsWith(".nstuexam") || file.name.endsWith(".zip")) {
        const buffer = await file.arrayBuffer();
        const reader = new NstuZip.Reader(buffer);
        const entries = await reader.readEntries();
        const manifestEntry = entries.find(e => e.path === "manifest.json");
        if (!manifestEntry) throw new Error("Package does not contain manifest.json.");
        const text = new TextDecoder().decode(manifestEntry.bytes);
        const data = JSON.parse(text);
        currentExam = data;
        activeQuestionIndex = 0;
        renderEditor();
        showToast(`Imported .nstuexam package (${entries.length} files)`, "success");
      } else {
        throw new Error("Please select a .nstuexam or .json file.");
      }
    } catch (err) {
      showToast("Import failed: " + err.message, "error");
    }
  }

  function saveDraft() {
    currentExam.id = el("exam-id").value.trim();
    currentExam.title = el("exam-title").value.trim();
    currentExam.subject = el("exam-subject").value.trim();
    currentExam.durationSeconds = Math.max(60, parseInt(el("exam-duration").value, 10) * 60 || 3600);
    const origins = parseOriginsInput(el("exam-allowed-origins").value);
    if (origins.length > 0) {
      currentExam.allowedOrigins = origins;
    } else {
      delete currentExam.allowedOrigins;
    }

    const manifestObj = compileManifest(currentExam);
    const jsonStr = JSON.stringify(manifestObj, null, 2);
    const blob = new Blob([jsonStr], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `${currentExam.id}-manifest.json`;
    a.click();
    URL.revokeObjectURL(url);
    showToast("Manifest draft downloaded", "success");
  }

  function escapeHtml(str) {
    if (typeof str !== "string") return "";
    return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  /* ========================================================================
     Initialization & Event Binding
     ======================================================================== */

  function init() {
    // Mode toggles
    el("btn-mode-editor").addEventListener("click", () => setMode("editor"));
    el("btn-mode-preview").addEventListener("click", () => setMode("preview"));
    el("btn-preview-exit").addEventListener("click", () => setMode("editor"));

    // Header actions
    el("btn-load-sample").addEventListener("click", () => {
      currentExam = JSON.parse(JSON.stringify(DEFAULT_EXAM));
      activeQuestionIndex = 0;
      renderEditor();
      showToast("Loaded IELTS Academic Sample Assessment", "info");
    });

    el("btn-open-file").addEventListener("click", () => el("file-import-input").click());
    el("file-import-input").addEventListener("change", (e) => {
      if (e.target.files && e.target.files[0]) {
        handleImportFile(e.target.files[0]);
      }
    });

    el("btn-save-draft").addEventListener("click", saveDraft);
    el("btn-export-pkg").addEventListener("click", exportPackage);

    // Modal controls
    el("btn-close-export").addEventListener("click", () => el("export-modal").classList.remove("open"));
    el("btn-cancel-export").addEventListener("click", () => el("export-modal").classList.remove("open"));

    // Add Question
    el("btn-add-question").addEventListener("click", () => {
      const type = el("select-add-type").value;
      addQuestion(type);
    });

    // Question form bindings
    el("q-points").addEventListener("change", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) q.points = parseInt(e.target.value, 10) || 1;
    });

    el("q-prompt").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) {
        q.prompt = e.target.value;
        const navItem = el("question-nav-list").children[activeQuestionIndex];
        if (navItem) navItem.querySelector(".nav-item-title").textContent = e.target.value || "Untitled Question";
      }
    });

    el("q-instruction").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) q.instruction = e.target.value;
    });

    el("q-passage").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) q.passage = e.target.value;
    });

    el("btn-add-token").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q && q.isMatching) {
        const nextNum = (q.options ? q.options.length : 0) + 1;
        q.options.push(`Item ${nextNum} description`);
        renderTokenEditor(q);
      }
    });

    el("btn-add-target").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q && q.isMatching) {
        const nextLetter = String.fromCharCode(65 + (q.matchingTargets ? q.matchingTargets.length : 0));
        q.matchingTargets.push(`Paragraph ${nextLetter}`);
        renderTokenEditor(q);
      }
    });

    el("btn-add-mc-option").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) {
        const nextLetter = String.fromCharCode(65 + (q.options ? q.options.length : 0));
        q.options.push(`Option ${nextLetter}`);
        renderMcOptionEditor(q);
      }
    });

    // Preview Navigation
    el("btn-p-prev").addEventListener("click", () => {
      if (previewIndex > 0) {
        previewIndex--;
        renderPreview();
      }
    });

    el("btn-p-next").addEventListener("click", () => {
      if (previewIndex < currentExam.questions.length - 1) {
        previewIndex++;
        renderPreview();
      }
    });

    initSplitter();
    renderEditor();
  }

  // Start application on DOM ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

})();
