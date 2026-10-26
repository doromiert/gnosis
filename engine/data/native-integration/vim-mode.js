// SPDX-License-Identifier: GPL-3.0-or-later
// Runs only in the browser-owned document world. Never trust page-made keys.
(() => {
    const send = globalThis.__jupiterDocumentMetadata;
    if (typeof send !== 'function' || globalThis.gnosisVim) return;
    let enabled = false, insert = false, prefix = '', prefixAt = 0;
    let overlay = null, hintRoot = null, hints = [], typed = '', pointerTarget = null;
    const alphabet = 'asdfghjkl';
    const action = name => send(JSON.stringify({kind: 'vim-action', action: name}));
    const clear = () => {
        overlay?.remove(); overlay = hintRoot = null; hints = []; typed = '';
    };
    const reset = () => { clear(); insert = false; prefix = ''; };
    const active = () => {
        let node = document.activeElement;
        while (node?.shadowRoot?.activeElement) node = node.shadowRoot.activeElement;
        return node;
    };
    const editable = event => {
        const focused = active();
        // Closed shadow roots retarget keyboard events to their host. Treat
        // opaque, non-control focus as input rather than stealing typed keys.
        if (focused instanceof Element && focused !== document.body && focused !== document.documentElement &&
            !focused.matches('a[href],area[href],button,summary,input,textarea,select')) {
            const style = getComputedStyle(focused);
            if (!(focused.scrollHeight > focused.clientHeight && /(auto|scroll)/.test(style.overflowY)) &&
                !(focused.scrollWidth > focused.clientWidth && /(auto|scroll)/.test(style.overflowX))) return true;
        }
        return document.designMode === 'on' || event.composedPath().some(node =>
        node instanceof Element && (node.isContentEditable || node.matches(
            'input,textarea,select,[role="textbox"],[role="searchbox"],[role="combobox"],[role="spinbutton"],[role="application"]') ||
            (node === focused && node.localName.includes('-'))));
    };
    const visible = node => {
        const rect = node.getBoundingClientRect(), style = getComputedStyle(node);
        return rect.width > 0 && rect.height > 0 && rect.bottom > 0 && rect.right > 0 &&
            rect.top < innerHeight && rect.left < innerWidth && style.visibility === 'visible' &&
            style.display !== 'none' && style.opacity !== '0';
    };
    const layer = () => {
        clear();
        overlay = document.createElement('div');
        overlay.style.cssText = 'all:initial!important;position:fixed!important;inset:0!important;pointer-events:none!important;z-index:2147483647!important';
        hintRoot = overlay.attachShadow({mode: 'closed'});
        const style = document.createElement('style');
        style.textContent = '.hint,.mode{position:fixed;box-sizing:border-box;font:700 13px system-ui;color:#fff;background:#383838;border:1px solid #555;border-radius:6px;padding:3px 6px;box-shadow:0 2px 6px #0005}.mode{bottom:18px;right:18px;font-weight:500;padding:10px 16px;border-radius:12px}';
        hintRoot.append(style);
        document.documentElement.append(overlay);
    };
    const hint = newTab => {
        layer();
        const selector = newTab ? 'a[href],area[href]' :
            'a[href],button,summary,input,textarea,select,[role="button"],[role="link"]';
        const candidates = [];
        let visited = 0;
        const visit = root => {
            for (const node of root.querySelectorAll('*')) {
                if (candidates.length >= 400 || ++visited > 10000) break;
                if (node.matches(selector) && !node.disabled && !node.closest('[inert]') && visible(node)) candidates.push(node);
                if (node.shadowRoot) visit(node.shadowRoot);
            }
        };
        visit(document);
        if (!candidates.length) { clear(); return; }
        const width = Math.max(1, Math.ceil(Math.log(candidates.length) / Math.log(alphabet.length)));
        hints = candidates.map((node, index) => {
            let code = '';
            for (let i = 0; i < width; i++) { code = alphabet[index % alphabet.length] + code; index = Math.floor(index / alphabet.length); }
            const label = document.createElement('span'), rect = node.getBoundingClientRect();
            label.className = 'hint'; label.textContent = code.toUpperCase();
            label.style.left = Math.max(2, Math.min(innerWidth - width * 12 - 16, rect.left)) + 'px';
            label.style.top = Math.max(2, Math.min(innerHeight - 30, rect.top)) + 'px';
            hintRoot.append(label);
            return {node, code, label, newTab};
        });
    };
    const choose = key => {
        if (key === 'Backspace') typed = typed.slice(0, -1);
        else if (key.length === 1 && alphabet.includes(key.toLowerCase())) typed += key.toLowerCase();
        else return;
        const matches = hints.filter(item => item.code.startsWith(typed));
        if (!matches.length) { clear(); return; }
        if (matches.length === 1 && matches[0].code === typed) {
            const {node, newTab} = matches[0]; clear();
            if (!node.isConnected || !visible(node) || node.disabled) return;
            if (newTab) {
                try {
                    const url = new URL(node.href, location.href);
                    if (!['http:', 'https:'].includes(url.protocol)) return;
                    const link = document.createElement('a'); link.href = url.href;
                    link.target = '_blank'; link.rel = 'noopener'; link.click();
                } catch {}
            } else {
                node.focus({preventScroll: true});
                node.click();
            }
            return;
        }
        for (const item of hints) {
            item.label.hidden = !item.code.startsWith(typed);
            item.label.textContent = item.code.slice(typed.length).toUpperCase();
        }
    };
    const scroller = event => {
        let node = event.composedPath().find(item => item instanceof Element && item !== document.body && item !== document.documentElement);
        node ||= pointerTarget;
        while (node instanceof Element && node !== document.documentElement) {
            const style = getComputedStyle(node);
            if ((node.scrollHeight > node.clientHeight && /(auto|scroll)/.test(style.overflowY)) ||
                (node.scrollWidth > node.clientWidth && /(auto|scroll)/.test(style.overflowX))) return node;
            node = node.parentElement || node.getRootNode()?.host;
        }
        return document.scrollingElement;
    };
    addEventListener('pointerdown', event => { if (event.isTrusted) pointerTarget = event.composedPath().find(node => node instanceof Element); }, {capture: true});
    addEventListener('scroll', () => { if (hints.length) clear(); }, {capture: true, passive: true});
    addEventListener('resize', () => { if (hints.length) clear(); });
    addEventListener('blur', reset);
    addEventListener('pagehide', reset);
    addEventListener('keydown', event => {
        if (!enabled || !event.isTrusted || event.isComposing || event.key === 'Process' || event.key === 'Dead') return;
        if (event.key === 'Escape') {
            if (insert || hints.length) { reset(); event.preventDefault(); event.stopImmediatePropagation(); }
            else if (editable(event)) { active()?.blur(); }
            prefix = ''; return;
        }
        if (insert || editable(event) || event.ctrlKey || event.altKey || event.metaKey) { prefix = ''; return; }
        if (hints.length) { event.preventDefault(); event.stopImmediatePropagation(); choose(event.key); return; }
        if (performance.now() - prefixAt > 1000) prefix = '';
        const key = event.key, previous = prefix;
        if (/^[1-9]$/.test(key) || (key === '0' && /^[1-9]/.test(prefix))) {
            prefix = (prefix + key).slice(0, 3); prefixAt = performance.now();
            event.preventDefault(); event.stopImmediatePropagation(); return;
        }
        const count = Math.min(999, parseInt(prefix, 10) || 1); prefix = '';
        const commands = {'/':'win.find',n:'win.find-next',N:'win.find-prev',o:'win.location',t:'win.new-tab',
            H:'toolbar.navigation-back',L:'toolbar.navigation-forward',r:'toolbar.reload',x:'tab.close',J:'win.vim-tab-next',K:'win.vim-tab-previous'};
        if ('hjkludG'.includes(key)) {
            const target = scroller(event); if (!target) return;
            const distance = Math.max(1, target.clientHeight || innerHeight) / 2;
            const delta = {h:[-60,0],j:[0,60],k:[0,-60],l:[60,0],u:[0,-distance],d:[0,distance]};
            if (key === 'G') target.scrollTo({top:target.scrollHeight,behavior:'instant'});
            else target.scrollBy({left:delta[key][0]*count,top:delta[key][1]*count,behavior:'instant'});
        } else if (key === 'g') {
            if (previous === 'g') scroller(event)?.scrollTo({top:0,behavior:'instant'});
            else { prefix = 'g'; prefixAt = performance.now(); }
        } else if (key === 'i' && previous === 'g') {
            [...document.querySelectorAll('input:not([type=hidden]),textarea,[contenteditable="true"]')].find(node => !node.disabled && !node.readOnly && visible(node))?.focus();
        } else if (key === 'f' || key === 'F') hint(key === 'F');
        else if (key === 'i') {
            insert = true; layer();
            const badge = document.createElement('span'); badge.className = 'mode';
            badge.textContent = 'Insert mode · Esc to return'; badge.setAttribute('role','status'); hintRoot.append(badge);
        } else if (commands[key]) { if (!event.repeat) action(commands[key]); }
        else return;
        event.preventDefault(); event.stopImmediatePropagation();
    }, {capture: true});
    Object.defineProperty(globalThis, 'gnosisVim', {value:Object.freeze({setEnabled(value) {
        enabled = value === true; if (!enabled) reset();
    }}), configurable:false, writable:false});
    send(JSON.stringify({kind:'vim-ready'}));
})();
