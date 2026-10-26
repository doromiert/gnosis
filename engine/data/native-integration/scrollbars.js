/* SPDX-License-Identifier: GPL-3.0-or-later
 * Animate the real Blink scrollbar, not a replacement DOM scroller. Blink's
 * scrollbar pseudo-elements do not run CSS transitions themselves. This isolated
 * world updates only their style source during a short state transition; no
 * scroll interception, idle animation loop, or privileged page binding exists.
 */
(() => {
  if (globalThis.__jupiterScrollbarsInstalled) return;
  globalThis.__jupiterScrollbarsInstalled = true;
  const expansion = '--jupiter-scrollbar-expansion';
  const thumbHover = '--jupiter-scrollbar-thumb-hover';
  const pressed = '--jupiter-scrollbar-pressed';
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  // Non-auto scrollbar-width bypasses Blink's custom scrollbar painting.
  // Normalize thin only; retain none and restore author styles on changes.
  const normalized = new WeakMap(), pending = new Set(), lastScrollbarStyle = new WeakMap();
  let normalizeFrame = 0;
  function normalize(element) {
    const style = element.style;
    if (!style) return;
    const original = normalized.get(element);
    if (original) {
      for (const [property, saved] of Object.entries(original)) {
        if (style.getPropertyValue(property) !== 'auto' || style.getPropertyPriority(property) !== 'important') continue;
        if (saved.value) style.setProperty(property, saved.value, saved.priority);
        else style.removeProperty(property);
      }
      normalized.delete(element);
      lastScrollbarStyle.set(element, element.getAttribute('style'));
    }
    if (getComputedStyle(element).scrollbarWidth !== 'thin') return;
    const saved = {};
    for (const property of ['scrollbar-width','scrollbar-color']) {
      saved[property] = {value:style.getPropertyValue(property), priority:style.getPropertyPriority(property)};
      style.setProperty(property, 'auto', 'important');
    }
    normalized.set(element, saved);
    lastScrollbarStyle.set(element, element.getAttribute('style'));
  }
  function scan(element) {
    if (!(element instanceof Element)) return;
    pending.add(element);
    if (normalizeFrame) return;
    normalizeFrame = requestAnimationFrame(() => {
      normalizeFrame = 0;
      const roots = [...pending]; pending.clear();
      for (const root of roots) {
        if (!root.isConnected || roots.some(other => other !== root && other.contains(root))) continue;
        normalize(root);
        for (const child of root.querySelectorAll('*')) normalize(child);
      }
    });
  }
  new MutationObserver(records => {
    for (const record of records) {
      if (record.type === 'attributes') {
        // Animation and our own inline override must not trigger tree scans.
        if (record.attributeName === 'style' && lastScrollbarStyle.has(record.target) &&
            record.target.getAttribute('style') === lastScrollbarStyle.get(record.target)) continue;
        scan(record.target);
      } else if (record.target.nodeName === 'STYLE') scan(document.documentElement);
      else for (const node of record.addedNodes) scan(node);
    }
  }).observe(document, {subtree:true, childList:true, attributes:true, attributeFilter:['class','style']});
  document.addEventListener('DOMContentLoaded', () => scan(document.documentElement), {once:true});
  document.addEventListener('load', event => {
    if (event.target instanceof HTMLLinkElement && event.target.relList.contains('stylesheet')) scan(document.documentElement);
  }, true);
  scan(document.documentElement);
  const states = new WeakMap();
  const animating = new Set();
  let hovered = null, captured = null, hoveredAxis = 'vertical', capturedAxis = 'vertical', capturedThumb = false, lastPointer = null, frame = 0;

  function writeStyle(element, property, value) {
    if (value === null) element.style.removeProperty(property);
    else element.style.setProperty(property, value, 'important');
    lastScrollbarStyle.set(element, element.getAttribute('style'));
  }

  function state(element, property) {
    let properties = states.get(element);
    if (!properties) { properties = new Map(); states.set(element, properties); }
    let value = properties.get(property);
    if (!value) {
      value = {element, property, value: 0, from: 0, to: 0, start: 0};
      properties.set(property, value);
    }
    return value;
  }
  function sources(element) {
    // Blink's ScrollbarStyleSource prefers body for the viewport whenever body
    // has custom scrollbar rules; document.scrollingElement is not its style
    // source. Mirror viewport state onto both possible sources.
    return element === document.documentElement && document.body ? [element, document.body] : [element];
  }
  function tick(now) {
    frame = 0;
    for (const item of animating) {
      const progress = reduced.matches ? 1 : Math.max(0, Math.min(1, (now - item.start) / 200));
      item.value = item.from + (item.to - item.from) * progress;
      for (const source of sources(item.element)) writeStyle(source, item.property, String(item.value));
      if (progress === 1 || !item.element.isConnected) {
        animating.delete(item);
        if (!item.to || !item.element.isConnected)
          for (const source of sources(item.element)) writeStyle(source, item.property, null);
      }
    }
    if (animating.size) frame = requestAnimationFrame(tick);
  }
  function transition(element, to, property = expansion) {
    if (!element) return;
    const item = state(element, property);
    if (item.to === to) return;
    item.from = item.value; item.to = to; item.start = performance.now();
    animating.add(item);
    if (!frame) frame = requestAnimationFrame(tick);
  }
  const property = (name, axis) => axis === 'horizontal' ? name + '-horizontal' : name;
  function hover(element, axis = 'vertical') {
    if (element === hovered && axis === hoveredAxis) return;
    transition(hovered, 0, property(expansion, hoveredAxis));
    transition(hovered, 0, property(thumbHover, hoveredAxis));
    hovered = element; hoveredAxis = axis;
    transition(hovered, 1, property(expansion, axis));
  }
  function axisAt(element, event) {
    if (captured) return capturedAxis;
    if (!element) return 'vertical';
    const root = element === document.documentElement;
    const scroller = root ? document.scrollingElement || element : element;
    const rect = root ? {left:0,right:innerWidth,width:innerWidth} : element.getBoundingClientRect();
    const scale = root ? 1 : rect.width / (element.offsetWidth || rect.width);
    const style = getComputedStyle(element);
    const vertical = scroller.scrollHeight > (root ? innerHeight : element.clientHeight);
    if (vertical && (style.direction === 'rtl' ? event.clientX <= rect.left + 24*scale : event.clientX >= rect.right - 24*scale)) return 'vertical';
    return 'horizontal';
  }
  function hit(event) {
    if (document.pointerLockElement) return null;
    const root = document.documentElement;
    const scroller = document.scrollingElement || root;
    const path = event.composedPath().filter(element => element instanceof Element);
    if (root && !path.includes(root)) path.push(root);
    for (const element of path) {
      if (element.clientWidth <= 0 || element.clientHeight <= 0) continue;
      const style = getComputedStyle(element);
      if (style.scrollbarWidth === 'none') continue;
      const isRoot = element === root;
      const rect = isRoot ? {left: 0, top: 0, right: innerWidth, bottom: innerHeight, width: innerWidth, height: innerHeight} : element.getBoundingClientRect();
      const sx = isRoot ? 1 : rect.width / (element.offsetWidth || rect.width);
      const sy = isRoot ? 1 : rect.height / (element.offsetHeight || rect.height);
      const left = rect.left + (parseFloat(style.borderLeftWidth) || 0) * sx;
      const right = rect.right - (parseFloat(style.borderRightWidth) || 0) * sx;
      const top = rect.top + (parseFloat(style.borderTopWidth) || 0) * sy;
      const bottom = rect.bottom - (parseFloat(style.borderBottomWidth) || 0) * sy;
      const x = event.clientX, y = event.clientY;
      if (x < left || x > right || y < top || y > bottom) continue;
      const vertical = (isRoot ? scroller.scrollHeight > innerHeight : element.scrollHeight > element.clientHeight) &&
        (isRoot || /^(auto|scroll|overlay)$/.test(style.overflowY));
      const horizontal = (isRoot ? scroller.scrollWidth > innerWidth : element.scrollWidth > element.clientWidth) &&
        (isRoot || /^(auto|scroll|overlay)$/.test(style.overflowX));
      if ((vertical && (style.direction === 'rtl' ? x <= left + 24 * sx : x >= right - 24 * sx)) ||
          (horizontal && y >= bottom - 24 * sy)) return element;
    }
    return null;
  }
  function onThumb(element, event) {
    if (!element || !event) return false;
    if (capturedThumb && element === captured) return true;
    const root = element === document.documentElement;
    const scroller = root ? document.scrollingElement || element : element;
    const rect = root ? {left:0,top:0,width:innerWidth,height:innerHeight} : element.getBoundingClientRect();
    const sx = root ? 1 : rect.width / (element.offsetWidth || rect.width);
    const sy = root ? 1 : rect.height / (element.offsetHeight || rect.height);
    const style = getComputedStyle(element);
    const left = root ? 0 : (parseFloat(style.borderLeftWidth) || 0);
    const top = root ? 0 : (parseFloat(style.borderTopWidth) || 0);
    const width = root ? innerWidth : element.clientWidth;
    const height = root ? innerHeight : element.clientHeight;
    const x = (event.clientX - rect.left) / sx - left;
    const y = (event.clientY - rect.top) / sy - top;
    const vertical = scroller.scrollHeight > height;
    const horizontal = scroller.scrollWidth > width;
    function contains(position, visible, total, offset, coordinate) {
      const length = Math.max(0, position - 18);
      const thumb = Math.min(length, Math.max(42, Math.round(length * visible / total)));
      const start = 9 + (total > visible ? offset / (total - visible) * (length - thumb) : 0);
      return coordinate >= start && coordinate <= start + thumb;
    }
    if (vertical && (style.direction === 'rtl' ? x <= 24 : x >= width - 24))
      return contains(height - (horizontal ? 24 : 0), height, scroller.scrollHeight, scroller.scrollTop, y);
    if (horizontal && y >= height - 24)
      return contains(width - (vertical ? 24 : 0), width, scroller.scrollWidth, Math.abs(scroller.scrollLeft), x);
    return false;
  }
  function update(event) {
    lastPointer = {clientX:event.clientX, clientY:event.clientY};
    const element = captured || hit(event);
    const axis = axisAt(element, event);
    hover(element, axis); transition(element, onThumb(element, event) ? 1 : 0, property(thumbHover, axis));
  }
  document.addEventListener('pointermove', update, {capture: true, passive: true});
  document.addEventListener('pointerdown', event => {
    const element = hit(event); capturedAxis = axisAt(element, event);
    captured = element; capturedThumb = onThumb(captured, event);
    if (captured) {
      hover(captured, capturedAxis); transition(captured, capturedThumb ? 1 : 0, property(thumbHover, capturedAxis));
      transition(captured, capturedThumb ? 1 : 0, property(pressed, capturedAxis));
    }
  }, {capture: true, passive: true});
  function release(event) {
    transition(captured, 0, property(pressed, capturedAxis));
    captured = null; capturedThumb = false;
    if (event) update(event); else hover(null);
  }
  document.addEventListener('pointerup', release, {capture: true, passive: true});
  document.addEventListener('pointercancel', () => release(), {capture: true, passive: true});
  document.addEventListener('pointerout', event => {
    // A native scrollbar is not a DOM relatedTarget. Entering it can emit
    // pointerout(null) while the pointer remains inside its viewport hit area.
    // Do not start collapsing the control underneath the pointer.
    if (!event.relatedTarget && !captured) update(event);
  }, {capture: true, passive: true});
  document.addEventListener('scroll', () => {
    if (hovered && lastPointer) transition(hovered, onThumb(hovered, lastPointer) ? 1 : 0, property(thumbHover, hoveredAxis));
  }, {capture:true, passive:true});
  window.addEventListener('blur', () => release());
  document.addEventListener('pointerlockchange', () => { if (document.pointerLockElement) release(); });
  window.addEventListener('pagehide', () => {
    release();
    if (frame) cancelAnimationFrame(frame);
    frame = 0;
    for (const item of animating) {
      for (const source of sources(item.element)) writeStyle(source, item.property, null);
      item.value = item.from = item.to = 0;
    }
    animating.clear();
  });
})();
