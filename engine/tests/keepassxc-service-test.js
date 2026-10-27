// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
(async () => {
    const calls = [];
    let privateTab = false, url = 'https://example.org/login';
    const tab = () => ({ id: 7, incognito: privateTab, url });
    const sandbox = {
        setTimeout, clearTimeout, URL, crypto: require('node:crypto'),
        browser: {
            tabs: { get: async id => { assert.equal(id, 7); return tab(); }, sendMessage: async (...args) => calls.push(args) },
            storage: { local: { set: async () => {} } }
        },
        page: { settings: { passkeys: true }, getLoginList: async () => [{ uuid: '123', text: 'Test Login', password: 'must never leave the worker', totp: '000000' }] },
        tabs: { getTabFromId: () => ({}) },
        keepass: { isConnected: true, isKeePassXCAvailable: true, isDatabaseClosed: false, isAssociated: () => true, currentKeePassXC: '2.7.12' },
        kpxcEvent: {messageHandlers: {}}
    };
    vm.runInNewContext(fs.readFileSync(process.argv[2] || 'engine/data/keepassxc/service.js', 'utf8'), sandbox);
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
    const prompts = [], upstream = [];
    sandbox.__gnosisPasskeyProvider = payload => prompts.push(JSON.parse(payload));
    const startPasskey = () => sandbox.kpxcEvent.messageHandlers.passkeys_register(tab(), [{timeout: 15000}, 'https://example.org']);
    let pending = startPasskey(); await new Promise(setImmediate);
    let prompt = prompts.find(p => !p.closed);
    assert.equal(prompt.tabId, 7); assert.equal(prompt.create, true);
    assert.deepEqual(Object.keys(prompt).sort(), ['create', 'id', 'tabId', 'timeout', 'url']);
    api.resolvePasskey(prompt.id, 'device');
    assert.equal((await pending).gnosisProvider, 'device');
    assert.equal(sandbox.gnosisPasskeyRequests.size, 0);
    prompts.length = 0;
    pending = startPasskey(); await new Promise(setImmediate);
    sandbox.kpxcEvent.messageHandlers.gnosis_passkeys_cancel({id: 8});
    assert.equal(sandbox.gnosisPasskeyRequests.size, 1); // another tab cannot cancel it
    sandbox.kpxcEvent.messageHandlers.gnosis_passkeys_cancel(tab());
    assert.equal((await pending).gnosisProvider, 'cancel');
    sandbox.keepass.isDatabaseClosed = true;
    let associated = false;
    sandbox.keepass.isAssociated = () => associated;
    sandbox.keepass.getDatabaseHash = async (_tab, flags) => {
        assert.equal(flags[1], true); upstream.push('unlock'); sandbox.keepass.isDatabaseClosed = false;
    };
    sandbox.keepass.associate = async () => { upstream.push('associate'); associated = true; };
    sandbox.keepass.passkeysRegister = async (_tab, args) => { upstream.push('register'); return {response: {publicKey: args[0]}}; };
    prompts.length = 0;
    pending = startPasskey(); await new Promise(setImmediate);
    prompt = prompts.find(p => !p.closed);
    api.resolvePasskey(prompt.id, 'keepassxc');
    assert.equal((await pending).gnosisProvider, 'keepassxc');
    assert.deepEqual(upstream, ['unlock', 'associate', 'register']);
    sandbox.page.settings.passkeys = false;
    assert.equal((await startPasskey()).gnosisProvider, 'device');
    privateTab = true;
    assert.equal((await startPasskey()).gnosisProvider, 'cancel');
    console.log('KEEPASSXC_SERVICE_PASS private isolation, website checks, sanitized labels, entry validation, top-frame fill, settings validation');
    console.log('KEEPASSXC_PROVIDER_PASS native provider selection, explicit hardware fallback, cancellation, unlock, pairing, private isolation');
})().catch(error => { console.error(error); process.exitCode = 1; });
