// SPDX-License-Identifier: GPL-3.0-or-later
(() => {
    if (!page.settings || !Object.hasOwn(page.settings, 'jupiterAutoUnlock')) return;
    if (!Object.hasOwn(page.settings, 'gnosisAutoUnlock'))
        page.settings.gnosisAutoUnlock = page.settings.jupiterAutoUnlock;
    delete page.settings.jupiterAutoUnlock;
    void browser.storage.local.set({settings: page.settings});
})();
