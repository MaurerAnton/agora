import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture({ horizontal = true, rtl = false, content = 800 } = {}) {
  class Element {
    constructor() {
      this.style = {}; this.listeners = new Map(); this.children = []; this.captured = new Set();
      const classes = new Set();
      this.classList = { contains: key => classes.has(key), add: key => classes.add(key),
        remove: key => classes.delete(key), toggle(key, value) { if (value) classes.add(key); else classes.delete(key); } };
    }
    addEventListener(type, callback, options) {
      const callbacks = this.listeners.get(type) || new Set();
      callbacks.add(callback); this.listeners.set(type, callbacks);
      options?.signal?.addEventListener("abort", () => callbacks.delete(callback));
    }
    dispatchEvent(event) { for (const callback of this.listeners.get(event.type) || []) callback(event); return true; }
    querySelectorAll() { return this.children.flatMap(node => [node, ...node.querySelectorAll()]); }
    closest() { return null; }
    contains(node) { return node === this || this.children.includes(node); }
    matches() { return false; }
    setPointerCapture(id) { this.captured.add(id); }
    hasPointerCapture(id) { return this.captured.has(id); }
    releasePointerCapture(id) { this.captured.delete(id); }
  }
  const host = new Element(), bar = new Element(), thumb = new Element();
  host.clientWidth = 200; host.clientHeight = 200;
  host.scrollWidth = horizontal ? content : 200; host.scrollHeight = horizontal ? 200 : content;
  host.scrollLeft = 0; host.scrollTop = 0;
  host.getBoundingClientRect = () => ({ left: 10, top: 20, right: 210, bottom: 220, width: 200, height: 200 });
  bar.offsetWidth = horizontal ? 200 : 12; bar.offsetHeight = horizontal ? 12 : 200;
  bar.getBoundingClientRect = () => ({ left: horizontal ? 10 : 198, top: horizontal ? 208 : 20 });
  bar.firstElementChild = thumb; bar.children = [thumb];
  Object.defineProperties(thumb, {
    offsetWidth: { get: () => parseFloat(thumb.style.width) || 4 },
    offsetHeight: { get: () => parseFloat(thumb.style.height) || 4 },
  });
  thumb.getBoundingClientRect = () => {
    const left = 10 + (parseFloat(thumb.style.left) || 0), top = 20 + (parseFloat(thumb.style.top) || 0);
    return { left, top, right: left + thumb.offsetWidth, bottom: top + thumb.offsetHeight,
      width: thumb.offsetWidth, height: thumb.offsetHeight };
  };
  const track = new Element(), item = new Element();
  host.children = [track]; track.children = [item];
  const document = new Element(), window = new Element();
  const observed = new Set();
  let refresh;
  const code = fs.readFileSync(new URL("../../main/assets/webui/material/scrollbar.js", import.meta.url), "utf8");
  const context = vm.createContext({ AbortController, Event, Node: Element, document, window,
    getComputedStyle: () => ({ direction: rtl ? "rtl" : "ltr" }), matchMedia: () => ({ matches: false }),
    ResizeObserver: class { constructor(callback) { refresh = callback; } observe(node) { observed.add(node); } unobserve(node) { observed.delete(node); } disconnect() {} },
    MutationObserver: class { observe() {} disconnect() {} },
    WheelEvent: class { constructor(type, options) { this.type = type; Object.assign(this, options); } },
  });
  vm.runInContext(code.replace(/export /g, ""), context);
  const input = [];
  let downs = 0;
  host.addEventListener("pointerdown", () => downs++);
  const dispose = horizontal ? context.attachScrollbar(host, bar, direction => input.push(direction), "horizontal")
    : context.attachScrollbar(host, bar, direction => input.push(direction));
  const emit = (node, type, values = {}) => node.dispatchEvent({ type, preventDefault() {}, stopPropagation() {},
    button: 0, pointerId: 1, pointerType: "mouse", ...values });
  return { host, bar, thumb, item, observed, emit, input, dispose, refresh, downs: () => downs };
}

test("horizontal scrollbar paints proportional thumb and shares hover with its host without a gutter", () => {
  const f = fixture();
  assert.equal(f.bar.classList.contains("scrollable"), true);
  assert.equal(f.bar.classList.contains("scrollbar-horizontal"), true);
  assert.equal(f.observed.has(f.item), true, "nested item resizes refresh transformed horizontal overflow");
  assert.equal(f.thumb.style.width, "50px");
  assert.equal(f.thumb.style.left, "0px");
  assert.equal(f.host.clientWidth, 200);
  assert.equal(f.host.style.width, undefined);
  assert.equal(f.bar.classList.contains("hover"), false);
  f.emit(f.host, "pointerenter");
  assert.equal(f.bar.classList.contains("hover"), true);
  f.emit(f.host, "pointerleave", { relatedTarget: f.bar });
  assert.equal(f.bar.classList.contains("hover"), true);
  f.emit(f.bar, "pointerleave", { relatedTarget: null });
  assert.equal(f.bar.classList.contains("hover"), false);
  f.host.scrollLeft = 300; f.emit(f.host, "scroll");
  assert.equal(f.thumb.style.left, "75px");
  f.host.scrollWidth = 200; f.refresh();
  assert.equal(f.bar.classList.contains("scrollable"), false);
  f.dispose();
  assert.equal(f.host.classList.contains("scrollbar-host"), false);
});

test("horizontal thumb drag interrupts the host owner and scrolls only its existing horizontal axis", () => {
  const f = fixture();
  f.emit(f.bar, "pointerdown", { clientX: 30 });
  assert.equal(f.downs(), 1);
  assert.equal(f.bar.classList.contains("dragging"), true);
  f.emit(f.bar, "pointermove", { clientX: 105 });
  assert.equal(f.host.scrollLeft, 300);
  assert.equal(f.host.scrollTop, 0);
  assert.equal(f.input[0], 0);
  f.emit(f.bar, "pointerup");
  assert.equal(f.bar.classList.contains("dragging"), false);
  assert.equal(f.bar.hasPointerCapture(1), false);
  f.dispose();
});

test("RTL horizontal thumb starts at physical right and drag follows the same negative scroll range", () => {
  const f = fixture({ rtl: true });
  assert.equal(f.thumb.style.left, "150px");
  f.emit(f.bar, "pointerdown", { clientX: 180 });
  f.emit(f.bar, "pointermove", { clientX: 105 });
  assert.equal(f.host.scrollLeft, -300);
  f.emit(f.host, "scroll");
  assert.equal(f.thumb.style.left, "75px");
  f.emit(f.bar, "pointercancel");
  assert.equal(f.bar.classList.contains("dragging"), false);
  f.dispose();
});

test("default vertical consumers retain their dimensions, scrolling and cleanup", () => {
  const f = fixture({ horizontal: false });
  assert.equal(f.bar.classList.contains("scrollbar-horizontal"), false);
  assert.equal(f.thumb.style.height, "50px");
  f.host.scrollTop = 300; f.emit(f.host, "scroll");
  assert.equal(f.thumb.style.top, "75px");
  assert.equal(f.host.scrollLeft, 0);
  f.dispose();
  f.host.scrollTop = 600; f.emit(f.host, "scroll");
  assert.equal(f.thumb.style.top, "75px");
});
