// SPDX-License-Identifier: GPL-3.0-or-later
// Only installed for an extension action popup, in the private metadata world.
(() => {
    if (window !== top || location.protocol !== 'chrome-extension:') return;
    const send = globalThis.__jupiterDocumentMetadata;
    if (typeof send !== 'function') return;
    let timer = 0, previous = '';
    const update = () => {
        timer = 0;
        if (!document.body) return;
        const root = document.documentElement.getBoundingClientRect();
        const body = document.body.getBoundingClientRect();
        const css = getComputedStyle(document.body);
        const number = value => parseFloat(value) || 0;
        const width = Math.ceil(Math.max(root.width,
            body.width + number(css.marginLeft) + number(css.marginRight)));
        const height = Math.ceil(body.height + number(css.marginTop) + number(css.marginBottom));
        const message = JSON.stringify({kind: 'popup-size', url: location.href, width, height});
        if (width > 0 && height > 0 && message !== previous) {
            previous = message;
            send(message);
        }
    };
    const schedule = () => { if (!timer) timer = setTimeout(update, 32); };
    const install = () => {
        const observer = new ResizeObserver(schedule);
        observer.observe(document.documentElement);
        observer.observe(document.body);
        new MutationObserver(schedule).observe(document.body,
            {subtree: true, childList: true, attributes: true, characterData: true});
        document.fonts?.ready.then(schedule);
        addEventListener('load', schedule, true);
        schedule();
    };
    if (document.readyState === 'loading')
        addEventListener('DOMContentLoaded', install, {once: true});
    else install();
})();
