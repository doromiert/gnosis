// SPDX-License-Identifier: GPL-3.0-or-later
// The GTK shell owns downloads. Use Chromium's real privileged API to suppress
// its shelf/bubble and download-start overlay; no page scripts or toolbar action.
const managedExtension = item => item.id !== chrome.runtime.id &&
    item.id !== 'giohadoodmdcadnkcpdmhmmdnaidnckm' &&
    item.type === 'extension' && item.installType !== 'other';
globalThis.gnosisExtensions = Object.freeze({
    async inventory() {
        return (await chrome.management.getAll()).filter(managedExtension);
    },
    async setEnabled(id, enabled) {
        const item = await chrome.management.get(id);
        if (!managedExtension(item) || !item.mayDisable ||
            (enabled && item.mayEnable === false))
            throw new Error('This extension cannot be changed by Gnosis');
        await chrome.management.setEnabled(id, enabled);
        const actual = await chrome.management.get(id);
        if (actual.enabled !== enabled) throw new Error('Extension state did not change');
        return this.inventory();
    }
});

function handOffDownloads() {
    globalThis.gnosisNativeReady = Promise.resolve(chrome.downloads.setUiOptions({enabled: false}))
        .then(() => true);
}
chrome.runtime.onStartup.addListener(handOffDownloads);
chrome.runtime.onInstalled.addListener(handOffDownloads);
handOffDownloads();
// Only the inherited private protocol pipe can install this worker binding.
// Webpages have no access to it or to this extension's browser APIs.
chrome.tabs.onZoomChange.addListener(change => {
    const send = globalThis.__gnosisZoomChanged;
    if (typeof send === 'function')
        send(JSON.stringify({tabId: change.tabId, factor: change.newZoomFactor, scope: change.zoomSettings.scope}));
});

// This API is reachable only through the shell's private worker session. No
// content script, externally_connectable endpoint or runtime message handler.
for (const event of [chrome.management.onInstalled, chrome.management.onUninstalled,
                     chrome.management.onEnabled, chrome.management.onDisabled]) {
    event.addListener(() => {
        if (typeof globalThis.__gnosisExtensionsChanged === 'function')
            globalThis.__gnosisExtensionsChanged('changed');
    });
}

// A browser navigation event, once per document; no page scripts, DOM observers,
// or persistent CSS inspector. USER origin also respects named author layers.
chrome.webNavigation.onCommitted.addListener(({tabId, frameId, url}) => {
    if (!globalThis.gnosisControlThemeEnabled || !/^(https?|file):/.test(url)) return;
    chrome.scripting.insertCSS({target: {tabId, frameIds: [frameId]},
        files: ['select.css'], origin: 'USER'}).catch(() => {}); // A frame may navigate away.
});
