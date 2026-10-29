// SPDX-License-Identifier: GPL-3.0-or-later
// An existing profile can retain the old worker after loadUnpacked updates it.
// Install this listener independently, once, in the verified native worker.
if (!chrome.webNavigation.onCreatedNavigationTarget.hasListeners()) {
    // Middle-click and noopener links have no DevTools opener or windowOpen event.
    chrome.webNavigation.onCreatedNavigationTarget.addListener(({sourceTabId, tabId, url}) => {
        if (/^https?:/.test(url) && typeof globalThis.__gnosisNavigationTargetCreated === 'function')
            globalThis.__gnosisNavigationTargetCreated(JSON.stringify({sourceTabId, tabId, url}));
    });
}
