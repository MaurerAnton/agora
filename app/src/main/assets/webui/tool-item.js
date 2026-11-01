import { useLayoutEffect, useRef } from "./vendor/preact-hooks.mjs";
import { html } from "./html.js";

const DURATION = 180;

/** One capsule owns its retained paint; the track owns placement and the combined lift. */
export function ToolItem({ toolKey, identity, active, enter = true, retain = false, reduced, onExited, children }) {
  const host = useRef(null), visual = useRef(null);
  const progress = useRef(0), exited = useRef(onExited), admitted = useRef(null);
  const visible = active && (enter || retain && admitted.current === identity);
  if (visible) admitted.current = identity;
  const leaving = !visible && admitted.current === identity;
  exited.current = onExited;
  useLayoutEffect(() => {
    const node = host.current, content = visual.current;
    let frame = 0;
    const from = progress.current, to = visible ? 1 : 0, started = performance.now();
    const draw = () => {
      if (visible) delete node.dataset.toolsExiting;
      const spatial = reduced ? to : progress.current;
      node.dataset.toolsProgress = String(spatial);
      node.dataset.toolsOpacity = String(progress.current);
      node.style.height = `${content.offsetHeight * spatial}px`;
      content.style.opacity = String(progress.current);
      content.style.visibility = progress.current === 0 ? "hidden" : "";
      content.style.transform = `scale(${reduced ? 1 : .96 + .04 * progress.current})`;
    };
    const observer = new ResizeObserver(draw);
    observer.observe(content);
    function tick(now) {
      const time = Math.min(1, (now - started) / DURATION);
      progress.current = from + (to - from) * time;
      draw();
      if (time < 1 && from !== to) frame = requestAnimationFrame(tick);
      else if (leaving) {
        admitted.current = null;
        node.dataset.toolsExiting = "false";
      }
      if (!active && (time === 1 || from === to)) exited.current?.();
    }
    tick(started);
    return () => { cancelAnimationFrame(frame); observer.disconnect(); };
  }, [identity, active, visible, reduced]);
  return html`<div class="tools-item" data-tools-key=${toolKey} data-owner=${identity}
    data-tools-exiting=${leaving ? "true" : null}
    ref=${host} inert=${!visible} aria-hidden=${!visible ? "true" : null}>
    <div class="tools-item-visual" ref=${visual}>${children}</div>
  </div>`;
}

/** Keep disappearing keys until their own fade completes, without reordering retained paint. */
export function retainToolItems(previous, latest) {
  const live = new Set(latest.map(item => item.id));
  const retained = [...latest];
  previous.forEach((item, index) => {
    if (!live.has(item.id)) retained.splice(Math.min(index, retained.length), 0, item);
  });
  return retained;
}
