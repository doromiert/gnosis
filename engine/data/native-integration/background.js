// SPDX-License-Identifier: GPL-3.0-or-later
// The GTK shell owns downloads. Use Chromium's real privileged API to suppress
// its shelf/bubble and download-start overlay; no page scripts or toolbar action.
const managedExtension = item => item.id !== chrome.runtime.id &&
    item.id !== 'giohadoodmdcadnkcpdmhmmdnaidnckm' &&
    item.type === 'extension' && item.installType !== 'other';
globalThis.jupiterExtensions = Object.freeze({
    async inventory() {
        return (await chrome.management.getAll()).filter(managedExtension);
    },
    async setEnabled(id, enabled) {
        const item = await chrome.management.get(id);
        if (!managedExtension(item) || !item.mayDisable ||
            (enabled && item.mayEnable === false))
            throw new Error('This extension cannot be changed by Jupiter');
        await chrome.management.setEnabled(id, enabled);
        const actual = await chrome.management.get(id);
        if (actual.enabled !== enabled) throw new Error('Extension state did not change');
        return this.inventory();
    }
});

function handOffDownloads() {
    globalThis.jupiterNativeReady = Promise.resolve(chrome.downloads.setUiOptions({enabled: false}))
        .then(() => true);
}
chrome.runtime.onStartup.addListener(handOffDownloads);
chrome.runtime.onInstalled.addListener(handOffDownloads);
handOffDownloads();
// Only the inherited private protocol pipe can install this worker binding.
// Webpages have no access to it or to this extension's browser APIs.
chrome.tabs.onZoomChange.addListener(change => {
    const send = globalThis.__jupiterZoomChanged;
    if (typeof send === 'function')
        send(JSON.stringify({tabId: change.tabId, factor: change.newZoomFactor, scope: change.zoomSettings.scope}));
});

// This API is reachable only through the shell's private worker session. No
// content script, externally_connectable endpoint or runtime message handler.
for (const event of [chrome.management.onInstalled, chrome.management.onUninstalled,
                     chrome.management.onEnabled, chrome.management.onDisabled]) {
    event.addListener(() => {
        if (typeof globalThis.__jupiterExtensionsChanged === 'function')
            globalThis.__jupiterExtensionsChanged('changed');
    });
}
