// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
function worker() {
    const listeners = [], messages = [];
    const context = vm.createContext({
        // Existing profiles already export these from the previous adapter.
        gnosisExtensions: {}, gnosisNativeReady: Promise.resolve(true),
        chrome: {webNavigation: {onCreatedNavigationTarget: {
            hasListeners: () => listeners.length > 0,
            addListener: listener => listeners.push(listener)
        }}},
        __gnosisNavigationTargetCreated: payload => messages.push(JSON.parse(payload))
    });
    return {context, listeners, messages};
}
const regular = worker(), privateWorker = worker();
for (const state of [regular, privateWorker]) {
    // A fresh background import and repeated native bootstrap must not duplicate it.
    vm.runInContext(source, state.context);
    vm.runInContext(source, state.context);
    assert.equal(state.listeners.length, 1);
}
const link = {sourceTabId: 12, tabId: 13, url: 'https://github.com/'};
regular.listeners[0](link);
assert.deepEqual(regular.messages, [link]);
assert.deepEqual(privateWorker.messages, [], 'Regular links must stay in their worker');
for (const url of ['file:///tmp/private', 'chrome://settings', 'javascript:alert(1)'])
    regular.listeners[0]({...link, url});
assert.equal(regular.messages.length, 1, 'Only permitted HTTP(S) hosts are forwarded');
delete regular.context.__gnosisNavigationTargetCreated;
regular.listeners[0](link); // A worker without the private native binding stays inert.
assert.equal(regular.messages.length, 1);
privateWorker.listeners[0]({...link, url: 'http://localhost/'});
assert.equal(privateWorker.messages.length, 1);
console.log('NAVIGATION_TARGET_PASS cached worker upgrade, idempotence, permitted schemes and private isolation');
