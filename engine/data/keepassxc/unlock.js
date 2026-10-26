// SPDX-License-Identifier: GPL-3.0-or-later
// Ask KeePassXC itself to unlock when the user enters a detected login form.
// Never handle the database password here. Synthetic page events and background
// frames cannot cause unlock prompts. A cancelled prompt has a short cooldown.
(() => {
    let pending = false;
    let lastPrompt = 0;
    const rescanned = new WeakSet();
    document.addEventListener('focusin', async event => {
        if (kpxc.settings?.jupiterAutoUnlock === false || !event.isTrusted || !navigator.userActivation.isActive ||
            window !== window.top || pending || Date.now() - lastPrompt < 30000)
            return;
        const field = event.composedPath()[0];
        if (!(field instanceof HTMLInputElement)) return;
        pending = true;
        try {
            // Staged sign-ins can replace the username form without a document
            // navigation. Follow upstream redetection when a real user reaches
            // a newly introduced password input; never prompt from DOM mutations.
            if (field.type === 'password' && !rescanned.has(field) &&
                !kpxc.combinations?.some(c => c.password === field)) {
                rescanned.add(field);
                kpxc.inputs = [];
                kpxc.combinations = [];
                await kpxc.initCredentialFields();
            }
            if (!kpxc.combinations?.some(c => c.username === field || c.password === field)) return;
            if (await kpxc.reconnect() && kpxc.databaseState === DatabaseState.LOCKED) {
                lastPrompt = Date.now();
                await sendMessage('get_database_hash', [false, true]);
            }
        } catch {
            // KeePassXC can be closed or disconnected during the request.
        } finally {
            pending = false;
        }
    }, true);
})();
