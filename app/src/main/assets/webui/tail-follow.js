// The MessageList frame loop is the only scroll actor. This module owns the native tail lifecycle
// and recognizes scroll intent without treating clicks, text selection or unrelated keys as drags.
export const TailMode = Object.freeze({
  INACTIVE: "INACTIVE", ARMED: "ARMED", ATTACHED: "ATTACHED",
  SETTLING: "SETTLING", DETACHED: "DETACHED",
});

export function coalescedScrollStep(errorPx, elapsedSeconds, timeConstantSeconds, maximumVelocity, minimumStepPx) {
  if (errorPx === 0 || elapsedSeconds <= 0) return 0;
  const fraction = 1 - Math.exp(-elapsedSeconds / Math.max(timeConstantSeconds, 0.001));
  const maximumStep = Math.max(minimumStepPx, maximumVelocity * elapsedSeconds);
  return Math.min(maximumStep, Math.max(-maximumStep, errorPx * fraction));
}

export function createStreamingTailFollow() {
  let mode = TailMode.INACTIVE;
  let active = false, enabled = false, paused = false;
  let previous = null, settlingStarted = null, stable = 0;
  const attached = () => mode === TailMode.ATTACHED || mode === TailMode.SETTLING;
  return {
    get mode() { return mode; },
    get following() {
      return !paused && (active && enabled && mode === TailMode.ATTACHED ||
        !active && mode === TailMode.SETTLING);
    },
    reset() {
      mode = TailMode.INACTIVE;
      active = false; enabled = false; paused = false;
      previous = null; settlingStarted = null; stable = 0;
    },
    update({ generating, blocked, handoff, stickToBottom = true }, now) {
      const wasActive = active;
      const wasFollowing = this.following;
      active = generating;
      enabled = !blocked && stickToBottom;
      paused = !blocked && handoff;
      if (blocked) mode = active ? TailMode.DETACHED : TailMode.INACTIVE;
      else if (!active) {
        if (mode === TailMode.ATTACHED) mode = TailMode.SETTLING;
        else if (mode !== TailMode.SETTLING) mode = TailMode.INACTIVE;
      } else if (!paused) {
        if (!enabled) mode = TailMode.DETACHED;
        else if (mode === TailMode.SETTLING) {
          mode = TailMode.ATTACHED;
          settlingStarted = null; stable = 0;
        } else if (mode === TailMode.INACTIVE) mode = TailMode.ARMED;
      }
      if (mode === TailMode.SETTLING && wasActive) {
        settlingStarted = now; stable = 0;
      }
      if (this.following && !wasFollowing) previous = now;
      if (!this.following) previous = null;
    },
    userDrag() {
      if (mode !== TailMode.INACTIVE) mode = TailMode.DETACHED;
      previous = null; settlingStarted = null; stable = 0;
    },
    proximity(remaining, scrolling, now) {
      if (active && enabled && !paused && !scrolling && !attached() && remaining <= 48) {
        mode = TailMode.ATTACHED;
        previous = now;
      }
    },
    frame(remaining, now) {
      if (!this.following) return 0;
      const elapsed = Math.max(0, Math.min(0.05, (now - (previous ?? now)) / 1000));
      previous = now;
      const step = remaining > 0.5 ? coalescedScrollStep(remaining, elapsed, 0.055, 2800, 2) : 0;
      if (mode === TailMode.SETTLING) {
        settlingStarted ??= now;
        stable = remaining <= 2 ? stable + 1 : 0;
        const duration = now - settlingStarted;
        if (duration >= 1600 || duration >= 700 && stable >= 8) {
          mode = TailMode.INACTIVE;
          previous = null; settlingStarted = null; stable = 0;
        }
      }
      return step;
    },
  };
}

export function bindTailScrollInput(root, { start, stop }, scrollbar = null) {
  let pointer = null, scrollbarHeld = false, moving = false, timer = 0;
  const finish = () => {
    clearTimeout(timer);
    if (pointer?.dragging || scrollbarHeld) return;
    if (moving) { moving = false; stop(); }
  };
  const deferFinish = () => { clearTimeout(timer); timer = setTimeout(finish, 120); };
  const begin = () => { moving = true; start(); deferFinish(); };
  const consumes = (target, direction) => {
    for (let node = target; node && node !== root; node = node.parentElement) {
      if (node.scrollHeight <= node.clientHeight + 1) continue;
      const overflow = getComputedStyle(node).overflowY;
      if (!/auto|scroll/.test(overflow)) continue;
      if (direction < 0 && node.scrollTop > 0 || direction > 0 &&
          node.scrollTop < node.scrollHeight - node.clientHeight - 1) return true;
    }
    return false;
  };
  const canMove = direction => direction < 0 ? root.scrollTop > 0
    : root.scrollTop < root.scrollHeight - root.clientHeight - 0.5;
  const wheel = event => {
    if (event.ctrlKey || !event.deltaY || Math.abs(event.deltaX) > Math.abs(event.deltaY) ||
        !canMove(Math.sign(event.deltaY)) || consumes(event.target, Math.sign(event.deltaY))) return;
    begin();
  };
  const key = event => {
    const target = event.target;
    if (target.closest?.("input,textarea,select,button,a,[contenteditable]:not([contenteditable=false])")) return;
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    const direction = ["ArrowUp", "PageUp", "Home"].includes(event.key) || event.key === " " && event.shiftKey ? -1
      : ["ArrowDown", "PageDown", "End", " "].includes(event.key) ? 1 : 0;
    if (direction && canMove(direction) && !consumes(target, direction)) begin();
  };
  const down = event => {
    if (!["touch", "pen"].includes(event.pointerType)) return;
    pointer = { id: event.pointerId, x: event.clientX, y: event.clientY, dragging: false, target: event.target };
  };
  const move = event => {
    if (pointer?.id !== event.pointerId) return;
    const dx = event.clientX - pointer.x, dy = event.clientY - pointer.y;
    if (!pointer.dragging && Math.abs(dy) > 6 && Math.abs(dy) > Math.abs(dx) &&
        !consumes(pointer.target, -Math.sign(dy))) {
      pointer.dragging = true;
      begin();
    }
  };
  const up = event => {
    if (scrollbarHeld) { scrollbarHeld = false; deferFinish(); }
    if (pointer?.id !== event.pointerId) return;
    const dragging = pointer.dragging;
    pointer = null;
    if (dragging) deferFinish();
  };
  const scrollbarDown = event => {
    if (event.button !== 0 || !scrollbar.classList.contains("scrollable") || root.closest("[inert]")) return;
    scrollbarHeld = true;
    begin();
  };
  scrollbar?.addEventListener("pointerdown", scrollbarDown, { capture: true, passive: true });
  const scroll = () => { if (moving) deferFinish(); };
  const listeners = [["wheel", wheel], ["keydown", key], ["pointerdown", down],
    ["pointermove", move], ["scroll", scroll], ["scrollend", finish]];
  for (const [type, listener] of listeners) root.addEventListener(type, listener, { passive: true });
  const view = root.ownerDocument.defaultView;
  view.addEventListener("pointerup", up, { passive: true });
  view.addEventListener("pointercancel", up, { passive: true });
  return {
    get scrolling() { return moving; },
    scrollbarInput() { begin(); },
    dispose() {
      clearTimeout(timer);
      scrollbar?.removeEventListener("pointerdown", scrollbarDown, true);
      for (const [type, listener] of listeners) root.removeEventListener(type, listener);
      view.removeEventListener("pointerup", up);
      view.removeEventListener("pointercancel", up);
    },
  };
}
