/**
 * One floating scrollbar owns paint and pointer input for its scrollable host.
 * Native bars are hidden without a layout gutter; wheel and keyboard remain with the host.
 */

/** Draws one overlay and returns the detach function owned by the presentation. */
export function attachScrollbar(host, suppliedBar, onScrollInput, axis = "vertical") {
  const horizontal = axis === "horizontal";
  const bar = suppliedBar ?? document.createElement("div");
  if (!suppliedBar) {
    bar.className = "scrollbar-overlay";
    bar.setAttribute("aria-hidden", "true");
    bar.append(document.createElement("span"));
    host.parentElement.append(bar);
  }
  const hadHorizontalClass = bar.classList.contains("scrollbar-horizontal");
  if (horizontal) bar.classList.add("scrollbar-horizontal");
  const hadHostClass = host.classList.contains("scrollbar-host");
  host.classList.add("scrollbar-host");
  const controller = new AbortController();
  const thumb = bar.firstElementChild;
  const viewportSize = () => horizontal ? host.clientWidth : host.clientHeight;
  const contentSize = () => horizontal ? host.scrollWidth : host.scrollHeight;
  const position = () => horizontal ? host.scrollLeft : host.scrollTop;
  const rtl = () => horizontal && getComputedStyle(host).direction === "rtl";
  const writePosition = value => { if (horizontal) host.scrollLeft = value; else host.scrollTop = value; };
  const thumbSize = () => horizontal ? thumb.offsetWidth : thumb.offsetHeight;
  const coordinate = event => horizontal ? event.clientX : event.clientY;
  const place = () => {
    const box = host.getBoundingClientRect();
    const parentNode = bar.offsetParent;
    const parent = parentNode?.getBoundingClientRect() ?? box;
    const scaleX = parentNode?.offsetWidth ? parent.width / parentNode.offsetWidth : 1;
    const scaleY = parentNode?.offsetHeight ? parent.height / parentNode.offsetHeight : 1;
    if (horizontal) {
      bar.style.left = `${(box.left - parent.left) / (scaleX || 1)}px`;
      bar.style.width = `${box.width / (scaleX || 1)}px`;
      bar.style.top = `${(box.bottom - parent.top) / (scaleY || 1) - bar.offsetHeight}px`;
    } else {
      bar.style.top = `${(box.top - parent.top) / (scaleY || 1)}px`;
      bar.style.height = `${box.height / (scaleY || 1)}px`;
      bar.style.left = `${(box.right - parent.left) / (scaleX || 1) - bar.offsetWidth}px`;
    }
    bar.style.right = "auto";
  };
  const paint = () => {
    const viewport = viewportSize(), scrollable = contentSize() - viewport;
    bar.classList.toggle("scrollable", viewport > 0 && scrollable > 1);
    if (viewport <= 0 || scrollable <= 1) return;
    const size = Math.min(viewport, Math.max(28, viewport ** 2 / contentSize()));
    const offset = Math.max(0, Math.min(scrollable, position() + (rtl() ? scrollable : 0)));
    thumb.style[horizontal ? "left" : "top"] = `${(offset / scrollable) * (viewport - size)}px`;
    thumb.style[horizontal ? "width" : "height"] = `${size}px`;
  };
  let drag = null;
  const pointerDown = (event) => {
    if (event.button !== 0 || !bar.classList.contains("scrollable") || host.closest("[inert]")) return;
    event.preventDefault();
    event.stopPropagation();
    host.dispatchEvent(new Event("pointerdown"));
    onScrollInput?.(0);
    bar.setPointerCapture(event.pointerId);
    const box = thumb.getBoundingClientRect();
    const point = coordinate(event), start = horizontal ? box.left : box.top;
    const end = horizontal ? box.right : box.bottom;
    const onThumb = point >= start - 8 && point <= end + 8;
    if (!onThumb) {
      const travel = viewportSize() - thumbSize();
      if (travel > 0) {
        const barBox = bar.getBoundingClientRect();
        const barStart = horizontal ? barBox.left : barBox.top;
        const size = horizontal ? box.width : box.height;
        const scrollable = contentSize() - viewportSize();
        const target = ((point - barStart - size / 2) / travel) * scrollable - (rtl() ? scrollable : 0);
        onScrollInput?.(Math.sign(target - position()));
        writePosition(target);
      }
    }
    drag = { id: event.pointerId, point, position: position() };
    bar.classList.add("dragging");
  };
  const pointerMove = (event) => {
    if (drag?.id !== event.pointerId) return;
    const scrollable = contentSize() - viewportSize();
    const travel = viewportSize() - thumbSize();
    if (scrollable <= 0 || travel <= 0) return;
    const target = drag.position + ((coordinate(event) - drag.point) / travel) * scrollable;
    onScrollInput?.(Math.sign(target - position()));
    writePosition(target);
  };
  const pointerUp = (event) => {
    if (drag?.id !== event.pointerId) return;
    drag = null;
    bar.classList.remove("dragging");
    if (bar.hasPointerCapture(event.pointerId)) bar.releasePointerCapture(event.pointerId);
  };
  const entered = (event) => { if (event.pointerType === "mouse") bar.classList.add("hover"); };
  const touched = (event) => { if (event.pointerType === "touch") bar.classList.remove("hover"); };
  const left = (event) => {
    if (event.relatedTarget instanceof Node && (host.contains(event.relatedTarget) || bar.contains(event.relatedTarget))) return;
    bar.classList.remove("hover");
  };
  for (const [type, listener] of [["pointerdown", pointerDown], ["pointermove", pointerMove],
    ["pointerup", pointerUp], ["pointercancel", pointerUp], ["lostpointercapture", pointerUp],
    ["pointerenter", entered], ["pointerleave", left]]) {
    bar.addEventListener(type, listener, { signal: controller.signal });
  }
  bar.addEventListener("wheel", (event) => {
    event.preventDefault();
    const forwarded = new WheelEvent("wheel", { deltaY: event.deltaY, deltaX: event.deltaX,
      deltaMode: event.deltaMode, bubbles: true, cancelable: true });
    if (!host.dispatchEvent(forwarded)) return;
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? viewportSize() : 1;
    const delta = horizontal ? event.deltaX || event.deltaY : event.deltaY;
    writePosition(position() + delta * unit);
  }, { passive: false, signal: controller.signal });
  host.addEventListener("scroll", paint, { passive: true, signal: controller.signal });
  host.addEventListener("input", paint, { signal: controller.signal });
  host.addEventListener("pointerenter", entered, { signal: controller.signal });
  host.addEventListener("pointerdown", touched, { signal: controller.signal });
  host.addEventListener("pointerleave", left, { signal: controller.signal });
  const refresh = () => { place(); paint(); };
  document.addEventListener("scroll", refresh, { capture: true, passive: true, signal: controller.signal });
  window.addEventListener("resize", refresh, { signal: controller.signal });
  const observer = new ResizeObserver(refresh);
  observer.observe(host);
  let content = new Set();
  const watchContent = () => {
    // Horizontal transforms can change scroll overflow without resizing the track itself.
    const next = new Set(horizontal ? host.querySelectorAll("*") : host.children);
    for (const node of content) if (!next.has(node)) observer.unobserve(node);
    for (const node of next) if (!content.has(node)) observer.observe(node);
    content = next;
    refresh();
  };
  const children = new MutationObserver(watchContent);
  children.observe(host, { childList: true, subtree: true, characterData: true });
  watchContent();
  if (matchMedia("(hover: hover) and (pointer: fine)").matches && host.matches(":hover")) bar.classList.add("hover");
  return () => {
    controller.abort();
    children.disconnect();
    observer.disconnect();
    if (!hadHostClass) host.classList.remove("scrollbar-host");
    if (horizontal && !hadHorizontalClass) bar.classList.remove("scrollbar-horizontal");
    if (!suppliedBar) bar.remove();
  };
}
