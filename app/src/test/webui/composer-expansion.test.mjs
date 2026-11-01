import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/assets/webui/composer.js", import.meta.url), "utf8");
const start = source.indexOf("  // Native keeps the tools overlay anchored to the last measured non-expanded bar.");
const end = source.indexOf("  const composing = useRef(false);", start);
assert.ok(start >= 0 && end > start);

function retainedAnchor(expanded = false) {
  let height = 139, enlarged = expanded, resize, cleanup, disconnected = false;
  const styles = new Map();
  const node = {
    classList: { contains: () => enlarged },
    getBoundingClientRect: () => ({ height }),
    style: { setProperty: (key, value) => styles.set(key, value) },
  };
  vm.runInNewContext(source.slice(start, end), {
    host: { current: node }, expanded,
    useLayoutEffect: effect => { cleanup = effect(); },
    ResizeObserver: class {
      constructor(callback) { resize = callback; }
      observe(target) { assert.equal(target, node); }
      disconnect() { disconnected = true; }
    },
  });
  return { styles, resize: () => resize?.(), cleanup: () => cleanup?.(),
    size: value => { height = value; }, expanded: value => { enlarged = value; },
    get observed() { return !!resize; }, get disconnected() { return disconnected; } };
}

test("Composer retains the ordinary bar anchor instead of publishing the expanded Surface height", () => {
  const anchor = retainedAnchor();
  assert.equal(anchor.styles.get("--composer-collapsed-height"), "139px");
  anchor.size(185);
  anchor.resize();
  assert.equal(anchor.styles.get("--composer-collapsed-height"), "185px");
  // A queued observer notification must not move tools to the expanded host's top.
  anchor.expanded(true);
  anchor.size(732);
  anchor.resize();
  assert.equal(anchor.styles.get("--composer-collapsed-height"), "185px");
  anchor.cleanup();
  assert.equal(anchor.disconnected, true);
});

test("an initially expanded Composer does not create a second active bar measurement", () => {
  const anchor = retainedAnchor(true);
  assert.equal(anchor.observed, false);
  assert.equal(anchor.styles.size, 0);
});

function measuredField({ expanded = false, naturalHeight = 70, overflowY = "" } = {}) {
  const style = { height: "47px", overflowY };
  const widths = [];
  const node = { style, get scrollHeight() {
    // A visible native scrollbar during height:auto would narrow the field and add a line.
    widths.push(style.overflowY);
    return naturalHeight + (style.overflowY === "hidden" ? 0 : 23);
  } };
  const fitStart = source.indexOf("  function fitField() {");
  const fitEnd = source.indexOf("  // Only the input/attachment column owns", fitStart);
  assert.ok(fitStart >= 0 && fitEnd > fitStart);
  vm.runInNewContext(source.slice(fitStart, fitEnd) + ";fitField();", {
    field: { current: node }, expanded, FIELD_MAX_HEIGHT: 162,
  });
  return { style, widths };
}

test("collapsed Composer measures full text width before restoring native overflow", () => {
  for (const overflowY of ["", "auto", "scroll"]) {
    const field = measuredField({ overflowY });
    assert.deepEqual(field.widths, ["hidden"]);
    assert.equal(field.style.height, "70px");
    assert.equal(field.style.overflowY, overflowY);
  }
  const capped = measuredField({ naturalHeight: 300, overflowY: "auto" });
  assert.equal(capped.style.height, "162px");
  assert.equal(capped.style.overflowY, "auto");
});

test("expanded Composer leaves exact constraints and overflow to the existing layout", () => {
  const field = measuredField({ expanded: true, overflowY: "auto" });
  assert.equal(field.style.height, "");
  assert.equal(field.style.overflowY, "auto");
  assert.deepEqual(field.widths, []);
});
