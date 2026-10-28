// SPDX-License-Identifier: GPL-3.0-or-later
(args => {
  'use strict';
  const state = globalThis.__gnosisMcp ??= {
    document: `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`,
    elements: new Map(),
    ids: new WeakMap(),
    next: 0,
  };
  const viewport = () => ({width: innerWidth, height: innerHeight});
  const visible = element => {
    const rect = element.getBoundingClientRect();
    const style = getComputedStyle(element);
    return rect.width > 0 && rect.height > 0 && rect.bottom > 0 && rect.right > 0 &&
      rect.top < innerHeight && rect.left < innerWidth && style.visibility !== 'hidden' && style.display !== 'none';
  };
  const resolve = ref => {
    const element = state.elements.get(ref)?.deref();
    if (!element?.isConnected || element.ownerDocument !== document)
      throw new Error('Stale element reference. Take a new snapshot.');
    return element;
  };
  if (args.operation === 'snapshot') {
    const elements = [];
    const current = new Map();
    let visited = 0;
    const scan = root => {
      for (const element of root.querySelectorAll('*')) {
        if (++visited > 10000 || elements.length >= 300) break;
        if (element.shadowRoot) scan(element.shadowRoot);
        if (!element.matches('a[href],button,input:not([type=hidden]),textarea,select,summary,iframe,[role],[contenteditable=true],[tabindex]') || !visible(element)) continue;
        let ref = state.ids.get(element);
        if (!ref) {
          ref = `${state.document}-${++state.next}`;
          state.ids.set(element, ref);
        }
        current.set(ref, new WeakRef(element));
        const rect = element.getBoundingClientRect();
        const labelled = (element.getAttribute('aria-labelledby') || '').split(/\s+/)
          .map(id => element.getRootNode().getElementById?.(id)?.textContent || '').join(' ');
        const name = element.getAttribute('aria-label') || labelled.trim() ||
          Array.from(element.labels || []).map(label => label.textContent).join(' ') ||
          element.getAttribute('alt') || element.getAttribute('title') ||
          element.getAttribute('placeholder') || element.innerText || '';
        elements.push({ref, tag: element.localName, role: element.getAttribute('role') || '',
          name: name.trim().slice(0, 200), type: element.getAttribute('type') || '',
          disabled: Boolean(element.disabled || element.getAttribute('aria-disabled') === 'true'),
          bounds: {x: rect.x, y: rect.y, width: rect.width, height: rect.height}});
      }
    };
    scan(document);
    state.elements = current;
    // shortcut: DOM references cover the top document and open shadow roots;
    // use screenshot coordinates for frames until frame-scoped references are added.
    return {url: location.href, title: document.title, ready: document.readyState,
      viewport: viewport(), text: (document.body?.innerText || '').slice(0, 40000),
      truncated: visited > 10000 || elements.length >= 300, elements};
  }
  if (args.operation === 'point') {
    let x = args.x, y = args.y;
    if (args.ref) {
      const element = resolve(args.ref);
      if (element.disabled || element.getAttribute('aria-disabled') === 'true')
        throw new Error('The element is disabled.');
      element.scrollIntoView({block: 'center', inline: 'center', behavior: 'instant'});
      if (!visible(element)) throw new Error('The element is not visible.');
      const rect = element.getBoundingClientRect();
      x = Math.max(0, rect.left) + (Math.min(innerWidth, rect.right) - Math.max(0, rect.left)) / 2;
      y = Math.max(0, rect.top) + (Math.min(innerHeight, rect.bottom) - Math.max(0, rect.top)) / 2;
      let hit = document.elementFromPoint(x, y);
      while (hit?.shadowRoot) {
        const inner = hit.shadowRoot.elementFromPoint(x, y);
        if (!inner || inner === hit) break;
        hit = inner;
      }
      if (hit !== element && !element.contains(hit)) throw new Error('Another element covers this target.');
    }
    if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || y < 0 || x >= innerWidth || y >= innerHeight)
      throw new Error('Coordinates must be inside the current viewport.');
    return {x, y, viewport: viewport()};
  }
  if (args.operation === 'editable') {
    let element = document.activeElement;
    while (element?.shadowRoot?.activeElement) element = element.shadowRoot.activeElement;
    if (!element || element.disabled || element.readOnly ||
        !(element.isContentEditable || element.localName === 'textarea' || element.localName === 'iframe' ||
          (element.localName === 'input' && !['button','submit','reset','checkbox','radio','file','range','color','hidden'].includes(element.type))))
      throw new Error('Focus an editable field before typing.');
    return {editable: true};
  }
  throw new Error('Unknown document operation.');
})
