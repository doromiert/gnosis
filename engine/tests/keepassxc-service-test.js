// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
(async () => {
    const calls = [];
    let privateTab = false, url = 'https://example.org/login';
    const tab = () => ({ id: 7, incognito: privateTab, url });
    const sandbox = {
        setTimeout, URL,
        browser: {
            tabs: { get: async id => { assert.equal(id, 7); return tab(); }, sendMessage: async (...args) => calls.push(args) },
            storage: { local: { set: async () => {} } }
        },
        page: { settings: { passkeys: true }, getLoginList: async () => [{ uuid: '123', text: 'Test Login', password: 'must never leave the worker', totp: '000000' }] },
        tabs: { getTabFromId: () => ({}) },
        keepass: { isConnected: true, isKeePassXCAvailable: true, isDatabaseClosed: false, isAssociated: () => true, currentKeePassXC: '2.7.12' },
        kpxcEvent: {}
    };
    vm.runInNewContext(fs.readFileSync('engine/data/keepassxc/service.js', 'utf8'), sandbox);
    const api = sandbox.jupiterKeePassXC;
    const entries = JSON.parse(JSON.stringify(await api.invoke(7, 'entries')));
    assert.deepEqual(entries, { entries: [{ id: '123', label: 'Test Login' }] });
    await assert.rejects(api.invoke(7, 'fill', 'stale'), /no longer available/);
    calls.length = 0;
    await api.invoke(7, 'redetect');
    assert.equal(calls[0][0], 7);
    assert.equal(calls[0][1].action, 'redetect_fields');
    assert.equal(calls[0][2].frameId, 0);
    calls.length = 0;
    await api.invoke(7, 'fill', '123');
    assert.equal(calls[0][0], 7); assert.equal(calls[0][1].uuid, '123'); assert.equal(calls[0][2].frameId, 0);
    url = 'chrome://settings'; await assert.rejects(api.invoke(7, 'entries'), /Open a website/);
    await assert.rejects(api.invoke(7, 'redetect'), /Open a website/);
    url = 'https://example.org'; privateTab = true;
    await assert.rejects(api.invoke(7, 'status'), /private tabs/); privateTab = false;
    privateTab = true; await assert.rejects(api.invoke(7, 'redetect'), /private tabs/); privateTab = false;
    await assert.rejects(api.invoke(7, 'passkeys', 'true'), /Invalid setting/);
    await api.invoke(7, 'auto-unlock', false); assert.equal(sandbox.page.settings.jupiterAutoUnlock, false);
    const injections = [];
    let missing = true;
    sandbox.browser.runtime = { getManifest: () => ({content_scripts: [
        {matches: ['<all_urls>'], js: ['trusted-content.js']},
        {matches: ['<all_urls>'], exclude_matches: ['*://*/*.xml*'], js: ['trusted-passkeys.js']}
    ]}) };
    sandbox.browser.scripting = {executeScript: async args => {
        assert.deepEqual(JSON.parse(JSON.stringify(args.target)), {tabId: 7, frameIds: [0]});
        injections.push(args);
        if (args.files) missing = false;
        return [{result: false}];
    }};
    sandbox.browser.tabs.sendMessage = async () => {
        if (missing) throw new Error('Could not establish connection. Receiving end does not exist.');
    };
    url = 'https://example.org/login';
    await api.invoke(7, 'redetect');
    assert.equal(injections.length, 3);
    injections.length = 0; missing = true; url = 'https://example.org/document.xml';
    await api.invoke(7, 'redetect');
    assert.equal(injections.length, 2); // honor upstream XML exclusions
    await assert.rejects(api.invoke(7, 'dump-passwords'), /Unknown/);
    console.log('KEEPASSXC_SERVICE_PASS private isolation, website checks, sanitized labels, entry validation, top-frame fill, settings validation');
})().catch(error => { console.error(error); process.exitCode = 1; });
