"use strict";

// Runs the browser recovery state machine without a browser dependency. The
// shim intentionally exposes only the DOM and WebView2 calls used by app.js.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const APP_SOURCE = fs.readFileSync(
  path.join(__dirname, "..", "exam", "web", "app.js"), "utf8");
const MANIFEST = {
  id: "exam-web-recovery-test",
  title: "Recovery test",
  subject: "Test",
  durationSeconds: 600,
  documents: [],
  questions: [{
    id: "q1",
    type: "short_answer",
    points: 1,
    prompt: "Answer",
    answerPlaceholder: "Type an answer"
  }]
};

const HEX16 = "11".repeat(16);
const HEX16_B = "22".repeat(16);
const HEX32_A = "aa".repeat(32);
const HEX32_B = "bb".repeat(32);
const HEX32_C = "cc".repeat(32);
const HEX32_D = "dd".repeat(32);

class FakeClassList {
  constructor() { this.values = new Set(); }
  toggle(name, force) {
    const enabled = force === undefined ? !this.values.has(name) : Boolean(force);
    if (enabled) this.values.add(name); else this.values.delete(name);
    return enabled;
  }
  add(...names) { names.forEach((name) => this.values.add(name)); }
  remove(...names) { names.forEach((name) => this.values.delete(name)); }
  contains(name) { return this.values.has(name); }
}

class FakeNode {
  constructor(tagName = "div", id = "") {
    this.tagName = String(tagName).toUpperCase();
    this.id = id;
    this.dataset = Object.create(null);
    this.children = [];
    this.listeners = Object.create(null);
    this.classList = new FakeClassList();
    this.className = "";
    this.style = { setProperty: (name, value) => { this.style[name] = value; } };
    this.attributes = Object.create(null);
    this.value = "";
    this.checked = false;
    this.disabled = false;
    this.hidden = false;
    this.placeholder = "";
    this.open = false;
    this._textContent = "";
  }

  get textContent() { return this._textContent; }
  set textContent(value) {
    this._textContent = String(value ?? "");
    if (this._textContent === "") this.children = [];
  }

  append(...nodes) { nodes.forEach((node) => this.appendChild(node)); }
  appendChild(node) { this.children.push(node); return node; }
  addEventListener(type, listener) {
    (this.listeners[type] ||= []).push(listener);
  }
  dispatchEvent(event) {
    const current = event || {};
    if (!current.target) current.target = this;
    for (const listener of this.listeners[current.type] || []) {
      listener.call(this, current);
    }
    return !current.defaultPrevented;
  }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  showModal() { this.open = true; }
  matches(selector) {
    return selector.split(",").map((item) => item.trim()).some((item) => {
      return item === this.tagName.toLowerCase() || item === this.tagName;
    });
  }
}

class FakeDocument {
  constructor() {
    this.nodes = [];
    this.byId = new Map();
    this.listeners = Object.create(null);
    this.documentElement = this.addNode("html");
    this.documentElement.style = {
      setProperty: (name, value) => { this.documentElement.style[name] = value; }
    };
    this.body = this.addNode("body");
    this.body.classList = new FakeClassList();
    const ids = [
      "exam-title", "exam-subject", "candidate-label", "timer",
      "connection-status", "save-status", "question-count", "question-list",
      "question-position", "question-type", "question-points", "question-content",
      "pdf-pane", "notes-pane", "pdf-frame", "pdf-empty", "notes",
      "language-setting", "text-size-setting", "contrast-setting", "motion-setting",
      "settings-dialog", "submit-dialog", "submit-form", "submit-summary"
    ];
    ids.forEach((id) => this.addNode(
      id.endsWith("-dialog") ? "dialog" : id === "submit-form" ? "form" :
        id === "notes" ? "textarea" : id.endsWith("-setting") ? "select" : "div", id));
    this.getElementById("timer").className = "timer";
    this.getElementById("pdf-frame").hidden = true;
    this.getElementById("pdf-empty").hidden = false;
    this.getElementById("notes-pane").hidden = true;
    this.addActionButton("previous");
    this.addActionButton("next");
    this.addActionButton("settings");
    this.addActionButton("submit");
    this.addActionButton("download");
  }

  addNode(tagName, id = "") {
    const node = new FakeNode(tagName, id);
    this.nodes.push(node);
    if (id) this.byId.set(id, node);
    return node;
  }
  addActionButton(action) {
    const node = this.addNode("button");
    node.dataset.action = action;
    return node;
  }
  getElementById(id) { return this.byId.get(id) || null; }
  createElement(tagName) { return this.addNode(tagName); }
  createTextNode(value) {
    const node = this.addNode("#text");
    node.textContent = value;
    return node;
  }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  querySelector(selector) {
    const action = /^\[data-action="([^"]+)"\]$/.exec(selector);
    if (action) return this.nodes.find((node) => node.dataset.action === action[1]) || null;
    if (selector === ".timer") return this.getElementById("timer");
    return null;
  }
  querySelectorAll(selector) {
    if (selector === "[data-action]") {
      return this.nodes.filter((node) => node.dataset.action);
    }
    if (selector === "[data-reference]") {
      return this.nodes.filter((node) => node.dataset.reference);
    }
    if (selector === "[data-i18n]") return [];
    if (selector === "[data-i18n-placeholder]") return [];
    if (selector === "button, input, textarea, select") {
      return this.nodes.filter((node) => ["BUTTON", "INPUT", "TEXTAREA", "SELECT"].includes(node.tagName));
    }
    return [];
  }
}

class FakeStorage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(String(key)) ? this.values.get(String(key)) : null; }
  setItem(key, value) { this.values.set(String(key), String(value)); }
  removeItem(key) { this.values.delete(String(key)); }
}

class FailingAnswerStorage extends FakeStorage {
  constructor() {
    super();
    this.failAnswers = false;
  }

  setItem(key, value) {
    if (this.failAnswers && String(key).endsWith(":answers")) {
      throw new Error("simulated answer-map storage failure");
    }
    super.setItem(key, value);
  }
}

class FakeWebView {
  constructor() {
    this.messages = [];
    this.listeners = Object.create(null);
  }
  postMessage(message) { this.messages.push(JSON.parse(JSON.stringify(message))); }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  emit(data) {
    for (const listener of this.listeners.message || []) listener({ data });
  }
}

class BrowserURL extends globalThis.URL {
  static createObjectURL() { return "blob:test"; }
  static revokeObjectURL() {}
}

function contextFor(candidateId, clientIdHex = HEX16, sessionIdHex = HEX16_B) {
  return {
    packageId: MANIFEST.id,
    packageDigestHex: HEX32_A,
    clientIdHex,
    sessionIdHex,
    candidateId,
    nextSequence: 1
  };
}

function contextKey(context) {
  return JSON.stringify([
    context.packageId, context.packageDigestHex, context.clientIdHex,
    context.sessionIdHex, context.candidateId
  ]);
}

function storageKey(context) {
  return `nstu-exam:${encodeURIComponent(MANIFEST.id)}:context:${encodeURIComponent(contextKey(context))}`;
}

function seedDurable(storage, context, values = {}) {
  const durable = {
    nextSequence: 1,
    previousEventHashHex: "",
    serverHighestSequence: 0,
    serverLastEventHashHex: "",
    serverStateHashHex: "",
    serverFinalized: false,
    pending: [],
    drafts: {},
    contextKey: contextKey(context),
    quarantined: [],
    finalizeRequested: false,
    ...values
  };
  storage.setItem(`${storageKey(context)}:durable-v1`, JSON.stringify(durable));
  return durable;
}

function seedAnswers(storage, context, answer, notes = "") {
  storage.setItem(`${storageKey(context)}:answers`, JSON.stringify({ q1: answer }));
  storage.setItem(`${storageKey(context)}:notes`, notes);
}

function createHarness(initialContext, prepare, localStorage = new FakeStorage(), options = {}) {
  const document = new FakeDocument();
  if (prepare) prepare(localStorage);
  const webview = new FakeWebView();
  let timerId = 0;
  const timers = new Map();
  const window = {
    NSTU_EXAM_MANIFEST: options.manifest || MANIFEST,
    NSTU_EXAM_CONTEXT: initialContext,
    chrome: { webview },
    crypto: { randomUUID: () => "00000000-0000-4000-8000-000000000000" },
    setTimeout: (callback, delay) => {
      const id = ++timerId;
      timers.set(id, { callback, delay });
      return id;
    },
    clearTimeout: (id) => { timers.delete(id); },
    setInterval: () => 0
  };
  if (Object.prototype.hasOwnProperty.call(options, "assetBase")) {
    window.NSTU_EXAM_ASSET_BASE = options.assetBase;
  }
  const context = vm.createContext({
    window,
    document,
    localStorage,
    TextEncoder,
    Uint8Array,
    Blob: class Blob {},
    URL: options.urlConstructor ? BrowserURL :
      { createObjectURL: () => "blob:test", revokeObjectURL: () => {} },
    console
  });
  vm.runInContext(APP_SOURCE, context, { filename: "exam/web/app.js" });
  return { document, localStorage, webview, window, timers };
}

function stateResponse(context, highest, lastEventHashHex, stateHashHex, finalized = false, answers = []) {
  return {
    type: "exam_state_response",
    response: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      highestContiguousSequence: highest,
      lastEventHashHex: highest ? lastEventHashHex : "",
      stateHashHex: highest ? stateHashHex : "",
      finalized,
      chunkIndex: 0,
      chunkCount: 1,
      answers
    }
  };
}

function chunkResponse(context, chunkIndex, chunkCount, answers) {
  return {
    type: "exam_state_response",
    response: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      highestContiguousSequence: 1,
      lastEventHashHex: HEX32_B,
      stateHashHex: HEX32_C,
      finalized: false,
      chunkIndex,
      chunkCount,
      answers
    }
  };
}

function runTimer(harness, predicate) {
  const entry = [...harness.timers.entries()].find(([, value]) => predicate(value));
  assert.ok(entry, "the expected browser timer should be scheduled");
  harness.timers.delete(entry[0]);
  entry[1].callback();
}

function latestAnswerInput(document) {
  const inputs = document.nodes.filter((node) => node.className === "answer-input");
  assert.ok(inputs.length > 0, "the exam should render an answer input");
  return inputs[inputs.length - 1];
}

function testStaleWatermarkIsIgnored() {
  const context = contextFor("stale-candidate");
  const harness = createHarness(context, (storage) => {
    seedDurable(storage, context, {
      nextSequence: 6,
      previousEventHashHex: HEX32_B,
      serverHighestSequence: 5,
      serverLastEventHashHex: HEX32_B,
      serverStateHashHex: HEX32_C
    });
  });
  const before = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  harness.webview.emit(stateResponse(context, 4, HEX32_D, HEX32_A));
  const after = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.deepEqual(after, before, "a lower server watermark must not rewind durable state");
  assert.equal(harness.document.getElementById("connection-status").textContent,
    "Waiting for server recovery state");
}

function testFinalizedResponseDoesNotRequeueFinalize() {
  const context = contextFor("finalize-candidate");
  const harness = createHarness(context);
  // Complete the initial authoritative-state handshake before submitting.
  harness.webview.emit(stateResponse(context, 0, "", ""));
  harness.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const events = () => harness.webview.messages.filter((message) => message.type === "exam_answer_event");
  assert.equal(events().length, 1, "submission should enqueue one finalization event");
  const finalEvent = events()[0].event;
  assert.equal(finalEvent.kind, "finalize");
  harness.webview.emit(stateResponse(context, 1, finalEvent.eventHashHex, HEX32_D, true));
  const durable = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 0, "the acknowledged finalization must leave no pending event");
  assert.equal(durable.serverFinalized, true);
  assert.equal(harness.document.getElementById("save-status").textContent, "Response package prepared.");
  harness.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  assert.equal(events().length, 1, "a finalized response must not enqueue another finalization");
}

function testContextSwitchPreservesTargetNamespace() {
  const contextA = contextFor("candidate-a");
  const contextB = contextFor("candidate-b", HEX16_B, HEX16);
  const harness = createHarness(contextA, (storage) => {
    seedAnswers(storage, contextA, "answer-a", "notes-a");
    seedAnswers(storage, contextB, "answer-b", "notes-b");
    seedDurable(storage, contextA);
    seedDurable(storage, contextB);
  });
  assert.equal(latestAnswerInput(harness.document).value, "answer-a");
  harness.webview.emit({ type: "exam_context", context: contextB });
  assert.equal(latestAnswerInput(harness.document).value, "answer-b",
    "switching context must load the target namespace");
  assert.equal(harness.localStorage.getItem(`${storageKey(contextB)}:answers`),
    JSON.stringify({ q1: "answer-b" }), "the target namespace must remain intact");
  const targetDurable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(contextB)}:durable-v1`));
  assert.equal(targetDurable.contextKey, contextKey(contextB));
  assert.equal(JSON.stringify(targetDurable).includes("answer-a"), false,
    "the active namespace must not contain prior-context answer data");
  assert.equal(harness.localStorage.getItem(`${storageKey(contextA)}:answers`),
    JSON.stringify({ q1: "answer-a" }),
    "the prior namespace must remain isolated and recoverable");
  harness.webview.emit({ type: "exam_context", context: contextA });
  assert.equal(latestAnswerInput(harness.document).value, "answer-a",
    "returning to a context must recover only that context's answer");
}

function testContextSwitchRejectsForeignPackage() {
  const context = contextFor("package-bound-candidate");
  const foreignContext = { ...context, packageId: "different-exam-package" };
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "local-answer", "local-notes");
    seedDurable(storage, context);
  });
  assert.equal(latestAnswerInput(harness.document).value, "local-answer");
  harness.webview.emit({ type: "exam_context", context: foreignContext });
  assert.equal(latestAnswerInput(harness.document).value, "local-answer",
    "a context message for another package must not replace the active context");
  assert.equal(harness.localStorage.getItem(`${storageKey(foreignContext)}:answers`),
    null, "a rejected package context must not load a foreign storage namespace");
  assert.equal(harness.document.getElementById("connection-status").textContent,
    "Exam context does not match this package");
}

function testInitialForeignPackageContextCannotLoadStorage() {
  const validContext = contextFor("initial-package-boundary");
  const foreignContext = { ...validContext, packageId: "foreign-initial-package" };
  const foreignStorageKey = storageKey(foreignContext);
  const storage = new FakeStorage();
  seedAnswers(storage, foreignContext, "must-not-load");
  seedDurable(storage, foreignContext, {
    contextKey: contextKey(foreignContext),
    finalizeRequested: true
  });
  const harness = createHarness(foreignContext, null, storage);
  assert.equal(latestAnswerInput(harness.document).value, "",
    "an initial foreign package context must not expose its answer map");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "an unbound initial context must not inherit a foreign finalization marker");
  assert.equal(harness.localStorage.getItem(`${foreignStorageKey}:answers`),
    JSON.stringify({ q1: "must-not-load" }),
    "foreign storage must remain untouched when the initial context is rejected");
}

function testLocalFinalizationLocksDynamicControls() {
  const context = contextFor("locally-finalized");
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "answer", "notes");
    seedDurable(storage, context, { finalizeRequested: true });
  });
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, true,
    "a durable finalization marker must lock the submit control after reload");
  assert.equal(latestAnswerInput(harness.document).disabled, true,
    "a durable finalization marker must lock dynamically rendered answers");
}

function testContextSwitchRestoresEditableControls() {
  const contextA = contextFor("locked-context");
  const contextB = contextFor("editable-context", HEX16_B, HEX16);
  const harness = createHarness(contextA, (storage) => {
    seedDurable(storage, contextA, { finalizeRequested: true });
    seedDurable(storage, contextB);
  });
  assert.equal(latestAnswerInput(harness.document).disabled, true,
    "the initially finalized context must be locked");
  harness.webview.emit({ type: "exam_context", context: contextB });
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "switching to an editable context must re-enable answer controls");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "switching to an editable context must re-enable static controls");
  assert.equal(harness.document.querySelector('[data-action="previous"]').disabled, true,
    "the first question must still disable previous navigation");
}

function testMismatchedPendingFinalizationDoesNotLockContext() {
  const staleContext = contextFor("stale-finalized");
  const activeContext = contextFor("active-editable", HEX16_B, HEX16);
  const source = createHarness(staleContext);
  source.webview.emit(stateResponse(staleContext, 0, "", ""));
  source.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const staleFinalization = source.webview.messages.find((message) =>
    message.type === "exam_answer_event" && message.event.kind === "finalize");
  assert.ok(staleFinalization, "the fixture must contain a valid finalization event");

  const harness = createHarness(activeContext, (storage) => {
    seedDurable(storage, activeContext, {
      contextKey: contextKey(staleContext),
      pending: [staleFinalization.event],
      finalizeRequested: true,
      serverFinalized: true,
      serverHighestSequence: staleFinalization.event.sequence,
      serverLastEventHashHex: staleFinalization.event.eventHashHex,
      serverStateHashHex: HEX32_B
    });
  });
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "a mismatched finalization must not lock the active context");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "the active context must remain submittable after quarantine");
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(activeContext)}:durable-v1`));
  assert.equal(durable.contextKey, contextKey(activeContext));
  assert.equal(durable.pending.length, 0,
    "mismatched finalization events must be quarantined");
  assert.equal(durable.finalizeRequested, false);
  assert.equal(durable.serverFinalized, false);
  assert.ok(durable.quarantined.some((entry) => entry.reason === "context-mismatch"),
    "quarantine evidence should retain bounded context-mismatch metadata");
}

function testContextSwitchQuarantinesStaleTargetNamespace() {
  const contextA = contextFor("switch-source");
  const contextB = contextFor("switch-target", HEX16_B, HEX16);
  const staleContext = contextFor("stale-target", HEX16, HEX16_B);
  const source = createHarness(staleContext);
  source.webview.emit(stateResponse(staleContext, 0, "", ""));
  source.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const staleFinalization = source.webview.messages.find((message) =>
    message.type === "exam_answer_event" && message.event.kind === "finalize");
  assert.ok(staleFinalization, "the stale namespace fixture must contain a finalization event");

  const harness = createHarness(contextA, (storage) => {
    seedDurable(storage, contextA);
    seedDurable(storage, contextB, {
      contextKey: contextKey(staleContext),
      pending: [staleFinalization.event],
      finalizeRequested: true,
      serverFinalized: true,
      serverHighestSequence: staleFinalization.event.sequence,
      serverLastEventHashHex: staleFinalization.event.eventHashHex,
      serverStateHashHex: HEX32_C
    });
  });
  harness.webview.emit({ type: "exam_context", context: contextB });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(contextB)}:durable-v1`));
  assert.equal(durable.pending.length, 0,
    "a stale target namespace must be quarantined before it can be activated");
  assert.equal(durable.finalizeRequested, false);
  assert.equal(durable.serverFinalized, false);
  assert.ok(durable.quarantined.some((entry) => entry.reason === "context-mismatch"),
    "switching into a stale namespace must retain quarantine evidence");
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "a stale target finalization must not lock the new context");
}

function testMalformedStoredClearIsQuarantined() {
  const context = contextFor("malformed-clear");
  const malformed = {
    packageId: MANIFEST.id,
    packageDigestHex: context.packageDigestHex,
    clientIdHex: context.clientIdHex,
    sessionIdHex: context.sessionIdHex,
    candidateId: context.candidateId,
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    clientTimeUnixMilliseconds: 1,
    kind: "clear",
    answer: "must-be-empty",
    eventHashHex: HEX32_A,
    previousEventHashHex: ""
  };
  const harness = createHarness(context, (storage) => {
    seedDurable(storage, context, { pending: [malformed] });
  });
  // A state checkpoint persists the in-memory quarantine result back to the
  // browser namespace; loading alone intentionally does not rewrite storage.
  harness.webview.emit(stateResponse(context, 0, "", ""));
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 0,
    "a stored clear event with answer bytes must never enter the retry queue");
  assert.ok(durable.quarantined.some((entry) => entry.reason === "invalid"),
    "malformed clear data should produce bounded quarantine evidence");
}

function testZeroWatermarkRejectsStateHash() {
  const context = contextFor("zero-watermark-state-hash");
  const harness = createHarness(context);
  const malformed = stateResponse(context, 0, "", "");
  malformed.response.stateHashHex = HEX32_A;
  harness.webview.emit(malformed);
  assert.notEqual(
    harness.document.getElementById("connection-status").textContent,
    "Server recovery state does not match pending answers.",
    "a zero watermark with a nonzero state hash must be rejected before reconciliation");
  harness.webview.emit(stateResponse(context, 0, "", ""));
  assert.equal(
    harness.document.getElementById("connection-status").textContent,
    "Server recovery ready",
    "a valid zero-watermark response must still complete reconciliation");
}

function testRejectedAckSchedulesRetry() {
  const context = contextFor("rejected-retry");
  const harness = createHarness(context);
  harness.webview.emit(stateResponse(context, 0, "", ""));
  const input = latestAnswerInput(harness.document);
  input.value = "answer";
  input.dispatchEvent({ type: "input" });
  runTimer(harness, (timer) => timer.delay === 350);
  const eventMessage = harness.webview.messages.find((message) =>
    message.type === "exam_answer_event");
  assert.ok(eventMessage, "the draft should be sent after the debounce timer");
  const event = eventMessage.event;
  harness.webview.emit({
    type: "exam_answer_ack",
    ack: {
      status: "rejected",
      sessionIdHex: context.sessionIdHex,
      sequence: event.sequence,
      eventHashHex: event.eventHashHex
    }
  });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 1,
    "a rejected event must remain durable for retry");
  assert.ok([...harness.timers.values()].some((timer) => timer.delay >= 1000),
    "a rejected ACK must schedule a bounded retry");
}

function testEquivalentChunkEncodingsAreNotConflicts() {
  const context = contextFor("chunk-normalization");
  const harness = createHarness(context);
  const answerCamel = {
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-answer",
    eventHashHex: HEX32_A
  };
  const answerSnake = {
    question_id: "q1",
    question_revision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-answer",
    event_hash_hex: HEX32_A
  };
  harness.webview.emit(chunkResponse(context, 0, 2, [answerCamel]));
  harness.webview.emit(chunkResponse(context, 0, 2, [answerSnake]));
  harness.webview.emit(chunkResponse(context, 1, 2, []));
  assert.equal(latestAnswerInput(harness.document).value, "server-answer",
    "equivalent camelCase and snake_case chunks must reassemble identically");
  assert.notEqual(harness.document.getElementById("connection-status").textContent,
    "Stale or conflicting server recovery state.");
}

function testPendingDraftWinsOverAuthoritativeCheckpoint() {
  const context = contextFor("pending-precedence");
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "local-newer");
    seedDurable(storage, context, {
      nextSequence: 2,
      drafts: {
        q1: {
          value: "local-newer",
          questionRevision: 1,
          contextKey: contextKey(context)
        }
      }
    });
  });
  const serverAnswer = {
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-checkpoint",
    eventHashHex: HEX32_B
  };
  harness.webview.emit(stateResponse(
    context, 1, HEX32_B, HEX32_C, false, [serverAnswer]));
  assert.equal(latestAnswerInput(harness.document).value, "local-newer",
    "a newer local draft must remain visible over an older server checkpoint");
}

function testAnswerMapFailureLeavesDurableDraft() {
  const context = contextFor("answer-map-failure");
  const storage = new FailingAnswerStorage();
  const harness = createHarness(context, null, storage);
  harness.webview.emit(stateResponse(context, 0, "", ""));
  storage.failAnswers = true;
  const input = latestAnswerInput(harness.document);
  input.value = "durable-draft";
  input.dispatchEvent({ type: "input" });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.drafts.q1.value, "durable-draft",
    "a failed answer-map write must retain a durable recovery draft");
  assert.equal(harness.document.getElementById("save-status").textContent,
    "Not saved", "the UI must not claim that a failed write was saved");
}

const ASSET_MANIFEST = {
  id: "asset-resolution-test",
  title: "Asset resolution test",
  subject: "Test",
  durationSeconds: 600,
  documents: [],
  questions: [
    {
      id: "audio",
      type: "listening",
      points: 1,
      prompt: "Listen",
      audio: "media/listening-01.mp3",
      options: ["A", "B"]
    },
    {
      id: "pdf",
      type: "reading",
      points: 1,
      prompt: "Read",
      pdf: "documents/reference.pdf",
      options: ["A", "B"]
    },
    {
      id: "external",
      type: "listening",
      points: 1,
      prompt: "Reject external asset",
      audio: "https://example.invalid/outside.mp3",
      options: ["A", "B"]
    },
    {
      id: "traversal",
      type: "listening",
      points: 1,
      prompt: "Reject traversal",
      audio: "../outside.mp3",
      options: ["A", "B"]
    },
    {
      id: "query",
      type: "listening",
      points: 1,
      prompt: "Reject query suffix",
      audio: "media/listening-01.mp3?download=1",
      options: ["A", "B"]
    },
    {
      id: "fragment",
      type: "listening",
      points: 1,
      prompt: "Reject fragment suffix",
      audio: "media/listening-01.mp3#fragment",
      options: ["A", "B"]
    },
    {
      id: "encoded-fragment",
      type: "listening",
      points: 1,
      prompt: "Reject encoded fragment suffix",
      audio: "media/listening-01.mp3%23fragment",
      options: ["A", "B"]
    }
  ]
};

function moveToQuestion(harness, index) {
  const next = harness.document.querySelector('[data-action="next"]');
  for (let step = 0; step < index; step += 1) next.dispatchEvent({ type: "click" });
}

function latestNode(document, tagName) {
  const nodes = document.nodes.filter((node) => node.tagName === tagName.toUpperCase());
  assert.ok(nodes.length > 0, `expected a ${tagName} element`);
  return nodes[nodes.length - 1];
}

function testInjectedAssetBaseResolvesLocalMedia() {
  const context = contextFor("asset-base");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  assert.equal(latestNode(harness.document, "audio").src,
    "https://nstu.exam/media/listening-01.mp3",
    "relative audio must resolve through the injected virtual host");
  moveToQuestion(harness, 1);
  assert.equal(harness.document.getElementById("pdf-frame").src,
    "https://nstu.exam/documents/reference.pdf",
    "relative PDF paths must resolve through the injected virtual host");
}

function testInjectedAssetBaseUsesBrowserUrlResolution() {
  const context = contextFor("asset-url-constructor");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/",
    urlConstructor: true
  });
  assert.equal(latestNode(harness.document, "audio").src,
    "https://nstu.exam/media/listening-01.mp3",
    "the browser URL branch must resolve package-relative media");
}

function testInjectedAssetBaseRejectsExternalAndTraversal() {
  const context = contextFor("asset-rejection");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  moveToQuestion(harness, 2);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "external audio must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "package traversal audio must not be loaded");
}

function testInjectedAssetBaseRejectsQueryAndFragmentSuffixes() {
  const context = contextFor("asset-suffix-rejection");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  moveToQuestion(harness, 4);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "query-bearing package assets must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "fragment-bearing package assets must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "encoded fragment package assets must not be loaded");
}

function testStaticPreviewKeepsRelativeAssetPaths() {
  const context = contextFor("asset-static-preview");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST
  });
  assert.equal(latestNode(harness.document, "audio").src, "media/listening-01.mp3",
    "static preview should retain package-relative media paths");
}

function testExpiredTimerDoesNotResetAfterReload() {
  const context = contextFor("expired-timer");
  const storage = new FakeStorage();
  storage.setItem(`${storageKey(context)}:remaining`, "0");
  const harness = createHarness(context, null, storage);
  assert.equal(harness.document.getElementById("timer").textContent, "00:00",
    "an expired persisted timer must remain expired after reload");
}

function testMalformedManifestDoesNotCrashInitialRender() {
  const context = contextFor("malformed-manifest");
  const malformed = {
    ...MANIFEST,
    questions: null,
    durationSeconds: "not-a-duration"
  };
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: malformed
  });
  assert.equal(harness.document.getElementById("timer").textContent, "45:00",
    "an invalid duration must fall back to the safe default");
  assert.equal(harness.document.getElementById("question-list").children.length, 0,
    "a malformed question list must render as an empty, usable page");
  assert.equal(harness.document.getElementById("question-content").textContent, "",
    "malformed question data must not produce a partial question render");
}

testStaleWatermarkIsIgnored();
testFinalizedResponseDoesNotRequeueFinalize();
testContextSwitchPreservesTargetNamespace();
testContextSwitchRejectsForeignPackage();
testInitialForeignPackageContextCannotLoadStorage();
testLocalFinalizationLocksDynamicControls();
testContextSwitchRestoresEditableControls();
testMismatchedPendingFinalizationDoesNotLockContext();
testContextSwitchQuarantinesStaleTargetNamespace();
testMalformedStoredClearIsQuarantined();
testZeroWatermarkRejectsStateHash();
testRejectedAckSchedulesRetry();
testEquivalentChunkEncodingsAreNotConflicts();
testPendingDraftWinsOverAuthoritativeCheckpoint();
testAnswerMapFailureLeavesDurableDraft();
testInjectedAssetBaseResolvesLocalMedia();
testInjectedAssetBaseUsesBrowserUrlResolution();
testInjectedAssetBaseRejectsExternalAndTraversal();
testInjectedAssetBaseRejectsQueryAndFragmentSuffixes();
testStaticPreviewKeepsRelativeAssetPaths();
testExpiredTimerDoesNotResetAfterReload();
testMalformedManifestDoesNotCrashInitialRender();
console.log("exam web recovery tests: 22 passed");
