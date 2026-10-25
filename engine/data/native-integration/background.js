// SPDX-License-Identifier: GPL-3.0-or-later
// The GTK shell owns downloads. Use Chromium's real privileged API to suppress
// its shelf/bubble and download-start overlay; no page scripts or toolbar action.
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
