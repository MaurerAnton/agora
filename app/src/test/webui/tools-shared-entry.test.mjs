import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

function row(reduced = false) {
  let next = 0, now = 0;
  const frames = new Map(), settled = [];
  const range = { style: { width: "0px" }, hasAttribute: () => false };
  const style = () => ({ setProperty(key, value) { this[key] = value; },
    getPropertyValue(key) { return this[key] || ""; }, removeProperty(key) { delete this[key]; } });
  const nodes = [{ dataset: { toolsKey: "todo", owner: "owner", toolsProgress: "0", toolsOpacity: "0" },
    style: { width: "", translate: "" }, offsetWidth: 100, offsetLeft: 0,
    hasAttribute: key => key === "data-tools-key", firstElementChild: { offsetHeight: 48 },
    getBoundingClientRect: () => ({ left: 12, right: 112 }) }];
  const track = { style: { width: "", minWidth: "" }, offsetWidth: 216,
    get children() { return [...nodes, range]; }, lastElementChild: range };
  const chat = { style: style() };
  const viewport = { firstElementChild: track, dataset: { toolsOwner: "owner", toolsFamily: "family",
    toolsEnter: "false", toolsOpening: "true" }, style: style(), clientWidth: 240, scrollWidth: 240, scrollLeft: 0,
    closest: () => chat, getBoundingClientRect: () => ({ left: 0 }), dispatchEvent() {},
    addEventListener() {}, removeEventListener() {} };
  let mutate;
  const code = fs.readFileSync(new URL("../../main/assets/webui/tools-row.js", import.meta.url), "utf8")
    .replace(/^import .*;\r?\n/gm, "").replace(/export /g, "");
  const context = vm.createContext({ performance: { now: () => now }, tweenProgress: value => value,
    requestAnimationFrame: callback => { frames.set(++next, callback); return next; },
    cancelAnimationFrame: key => frames.delete(key),
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
    MutationObserver: class { constructor(callback) { mutate = callback; } observe() {} disconnect() {} },
    getComputedStyle: () => ({ direction: "ltr" }), Event: class {},
    window: { addEventListener() {}, removeEventListener() {} } });
  vm.runInContext(code, context);
  const dispose = context.attachToolsRow(viewport, reduced, (...values) => settled.push(values));
  return { viewport, track, chat, nodes, settled, dispose,
    step(ms = 16) { now += ms; const pending = [...frames.values()]; frames.clear(); pending.forEach(callback => callback(now)); },
    change() { mutate([]); }, pending: () => frames.size,
    entrances: () => settled.filter(values => values[2] === true) };
}

test("opening remains hidden until data, destination geometry and every child fade settle", () => {
  const f = row();
  assert.equal(f.viewport.style.opacity, "0");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), "0px");
  f.step();
  assert.deepEqual(f.settled, [], "a partial initial snapshot does not complete replacement geometry");
  f.viewport.dataset.toolsRequired = JSON.stringify([["todo", "owner"]]);
  f.change(); f.step(); f.step();
  assert.ok(f.settled.some(values => values[1] === f.viewport.dataset.toolsRequired));
  assert.equal(f.entrances().length, 0, "mounted width alone does not begin first paint");
  f.nodes[0].dataset.toolsProgress = ".5";
  f.change(); f.step();
  assert.equal(f.entrances().length, 0);
  f.nodes[0].dataset.toolsProgress = "1";
  f.nodes[0].dataset.toolsOpacity = ".5";
  f.change(); f.step();
  assert.equal(f.entrances().length, 0, "spatial readiness does not complete a child's opacity fade");
  f.nodes[0].dataset.toolsOpacity = "1";
  f.change(); f.step();
  assert.equal(f.entrances().length, 1);
  assert.equal(f.viewport.style.opacity, "0");
  assert.equal(f.track.style.transformOrigin, "50% 100%");
  assert.equal(f.track.style.scale, "0.96");
  f.viewport.dataset.toolsOpening = "false";
  f.step(90);
  assert.equal(f.viewport.style.opacity, "0.5");
  assert.equal(f.track.style.scale, "0.98");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), "24px");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-message-inset"), "28px");
  f.change(); f.step();
  assert.equal(f.entrances().length, 1, "measurement cannot restart the row entrance");
  f.dispose();
  assert.equal(f.pending(), 0, "disposal cancels the shared animation frame");
  assert.equal(f.track.style.scale, "");
});

test("initial interaction reveal completion wakes the shared entrance without another mutation", () => {
  for (const reduced of [false, true]) {
    const f = row(reduced), node = f.nodes[0];
    node.dataset.toolsKey = "interaction";
    node.dataset.toolsProgress = "1";
    node.dataset.toolsOpacity = "0";
    node.firstElementChild.getBoundingClientRect = node.getBoundingClientRect;
    f.viewport.dataset.toolsRequired = JSON.stringify([["interaction", "owner"]]);
    f.change(); f.step(); f.step();
    assert.equal(f.entrances().length, 0);
    node.dataset.toolsOpacity = "1";
    node.dataset.toolsReveal = "owner";
    f.change(); f.step();
    if (!reduced) {
      assert.equal(f.entrances().length, 0);
      f.step(349);
      assert.equal(f.entrances().length, 0);
      f.step(1);
    }
    assert.equal(f.entrances().length, 1, "finishing reveal must recheck entrance after it clears its own state");
    f.viewport.dataset.toolsOpening = "false";
    f.step(180);
    assert.equal(f.viewport.style.opacity, "1");
    assert.equal(f.pending(), 0);
    f.dispose();
  }
});

test("Reduced Motion keeps one opacity entrance while snapping scale and clearance", () => {
  const f = row(true);
  f.viewport.dataset.toolsRequired = JSON.stringify([["todo", "owner"]]);
  f.nodes[0].dataset.toolsProgress = "1";
  f.nodes[0].dataset.toolsOpacity = ".5";
  f.change(); f.step(); f.step(); f.change(); f.step();
  assert.equal(f.entrances().length, 0);
  f.nodes[0].dataset.toolsOpacity = "1";
  f.change(); f.step();
  assert.equal(f.entrances().length, 1);
  f.viewport.dataset.toolsOpening = "false";
  f.step(90);
  assert.equal(f.viewport.style.opacity, "0.5");
  assert.equal(f.track.style.scale, "1");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), "48px");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-message-inset"), "56px");
  f.step(90);
  assert.equal(f.viewport.style.opacity, "1");
  assert.equal(f.track.style.scale, "");
  assert.equal(f.pending(), 0);
  f.dispose();
});

test("loaded-empty opening completes immediately and leaves live arrival to its item", () => {
  const f = row();
  const todo = f.nodes[0];
  f.nodes.splice(0);
  f.viewport.dataset.toolsRequired = "[]";
  f.change(); f.step(); f.step();
  assert.equal(f.entrances().length, 1, "an empty row completes without a later child mutation");
  assert.equal(f.settled.at(-1)[1], "[]");
  assert.equal(f.viewport.style.opacity, "1", "an empty row has no shared fade to run");
  assert.equal(f.track.style.scale || "1", "1");
  assert.equal(f.pending(), 0, "loaded-empty must not reserve a 180ms entrance");
  f.viewport.dataset.toolsOpening = "false";
  todo.dataset.toolsProgress = ".5";
  todo.dataset.toolsOpacity = ".5";
  f.nodes.push(todo);
  f.viewport.dataset.toolsRequired = JSON.stringify([["todo", "owner"]]);
  f.change(); f.step();
  assert.equal(f.viewport.style.opacity, "1", "live item opacity is not multiplied by an empty row fade");
  assert.equal(f.track.style.scale || "1", "1");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), "24px");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-message-inset"), "28px");
  assert.equal(f.entrances().length, 1);
  f.dispose();
});

for (const partial of [false, true]) test(`unrelated selection preserves ${partial ? "partial" : "completed"} outgoing row paint and clearance`, () => {
  const f = row();
  f.viewport.dataset.toolsRequired = JSON.stringify([["todo", "owner"]]);
  f.nodes[0].dataset.toolsProgress = "1";
  f.nodes[0].dataset.toolsOpacity = "1";
  f.change(); f.step(); f.step(); f.change(); f.step();
  f.viewport.dataset.toolsOpening = "false";
  f.step(partial ? 90 : 180);
  const opacity = f.viewport.style.opacity, scale = f.track.style.scale;
  const lift = f.chat.style.getPropertyValue("--chat-tools-lift");
  const inset = f.chat.style.getPropertyValue("--chat-tools-message-inset");
  f.viewport.dataset.toolsOwner = "destination";
  f.viewport.dataset.toolsFamily = "unrelated";
  f.viewport.dataset.toolsOpening = "true";
  delete f.viewport.dataset.toolsRequired;
  f.nodes[0].dataset.toolsExiting = "true";
  f.change(); f.step(0);
  assert.equal(f.viewport.style.opacity, opacity);
  assert.equal(f.track.style.scale, scale, "an unrelated selection cannot snap the painted outgoing scale");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), lift);
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-message-inset"), inset);
  f.nodes[0].dataset.toolsProgress = ".5";
  f.nodes[0].dataset.toolsOpacity = ".5";
  f.change(); f.step(16);
  assert.equal(f.viewport.style.opacity, opacity, "outgoing paint freezes the shared entrance progress");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), `${parseFloat(lift) / 2}px`);
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-message-inset"), `${parseFloat(inset) / 2}px`);
  f.nodes.splice(0);
  f.change(); f.step();
  assert.equal(f.viewport.style.opacity, "0", "reset only after outgoing paint is gone");
  assert.equal(f.track.style.scale, "");
  assert.equal(f.chat.style.getPropertyValue("--chat-tools-lift"), "0px");
  f.dispose();
});
