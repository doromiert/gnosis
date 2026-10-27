// SPDX-License-Identifier: GPL-3.0-or-later
// Installed only in Gnosis's own privileged worker through the private pipe.
globalThis.gnosisBrowserSettings = Object.freeze({
    async apply(settings) {
        const genericFonts = new Set(['serif', 'sans-serif', 'monospace', 'sans']);
        for (const [genericFamily, font] of Object.entries(settings.fonts || {})) {
            if (genericFonts.has(font) || !font)
                await chrome.fontSettings.clearFont({ genericFamily, script: 'Zyyy' });
            else
                await chrome.fontSettings.setFont({ genericFamily, script: 'Zyyy', fontId: font });
        }
        if (settings.fontSize) await chrome.fontSettings.setDefaultFontSize({ pixelSize: settings.fontSize });
        if (settings.fixedSize) await chrome.fontSettings.setDefaultFixedFontSize({ pixelSize: settings.fixedSize });
        await chrome.fontSettings.setMinimumFontSize({ pixelSize: settings.minimumSize || 0 });
        if (typeof settings.storage === 'boolean')
            await chrome.contentSettings.cookies.set({ primaryPattern: '<all_urls>', setting: settings.storage ? 'allow' : 'block' });
        if (typeof settings.popups === 'boolean')
            await chrome.contentSettings.popups.set({ primaryPattern: '<all_urls>', setting: settings.popups ? 'allow' : 'block' });
        return true;
    },
    async adblock(enabled) {
        const blockers = (await chrome.management.getAll()).filter(item => item.type === 'extension' && item.name === 'uBlock Origin');
        for (const item of blockers)
            if (item.enabled !== enabled) await chrome.management.setEnabled(item.id, enabled);
        return blockers.length;
    },
    async clearData(types) {
        const allowed = new Set(['cache', 'cacheStorage', 'cookies', 'localStorage', 'indexedDB', 'serviceWorkers']);
        if (!Array.isArray(types) || types.some(t => !allowed.has(t))) throw new Error('Unsupported website data type');
        await chrome.browsingData.remove({ since: 0, originTypes: { unprotectedWeb: true, protectedWeb: true, extension: false } },
            Object.fromEntries(types.map(t => [t, true])));
        return true;
    }
});
