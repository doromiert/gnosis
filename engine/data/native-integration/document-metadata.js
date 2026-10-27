// SPDX-License-Identifier: GPL-3.0-or-later
// Top-frame metadata and trusted interaction invalidation. Installed in its own isolated world; pages
// cannot call the private binding or replace this world's DOM prototypes.
(() => {
    const send = globalThis.__gnosisDocumentMetadata;
    if (typeof send !== 'function') return;
    for (const type of ['pointerdown', 'keydown', 'input'])
        addEventListener(type, event => {
            if (event.isTrusted) send(JSON.stringify({kind:'activity'}));
        }, {capture:true});
    if (window !== top) return;
    let previous = '', timer = 0;
    const update = () => {
        timer = 0;
        const message = JSON.stringify({ url: location.href, title: document.title,
            icon: document.querySelector('link[rel~=icon]')?.href || '' });
        if (message !== previous) { previous = message; send(message); }
    };
    const install = () => {
        update();
        if (!document.head) return;
        new MutationObserver(() => {
            if (!timer) timer = setTimeout(update, 50);
        }).observe(document.head, { childList: true, subtree: true,
            characterData: true, attributes: true, attributeFilter: ['href', 'rel'] });
    };
    if (document.readyState === 'loading')
        addEventListener('DOMContentLoaded', install, { once: true });
    else install();
})();
