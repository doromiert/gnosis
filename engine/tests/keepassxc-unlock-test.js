// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('engine/data/keepassxc/unlock.js', 'utf8');
const run = async () => {
    let handler, calls = [], now = 100000, state = 'locked', resolve;
    class Input {}
    const field = new Input();
    const window = {}; window.top = window;
    const sandbox = {
        window, HTMLInputElement: Input, navigator: { userActivation: { isActive: true } },
        Date: { now: () => now },
        document: { addEventListener: (type, fn) => { assert.equal(type, 'focusin'); handler = fn; } },
        DatabaseState: { LOCKED: 'locked' },
        kpxc: { settings: {}, combinations: [{ username: field }], reconnect: async () => true, get databaseState() { return state; } },
        sendMessage: async (...args) => { calls.push(args); if (resolve) await new Promise(resolve); }
    };
    vm.runInNewContext(source, sandbox);
    const event = { isTrusted: true, composedPath: () => [field] };
    await handler({ ...event, isTrusted: false }); assert.equal(calls.length, 0);
    sandbox.navigator.userActivation.isActive = false;
    await handler(event); assert.equal(calls.length, 0);
    sandbox.navigator.userActivation.isActive = true;
    await handler({ ...event, composedPath: () => [new Input()] }); assert.equal(calls.length, 0);
    window.top = {}; await handler(event); assert.equal(calls.length, 0); window.top = window;
    state = 'unlocked'; await handler(event); assert.equal(calls.length, 0);
    state = 'locked'; sandbox.kpxc.settings.jupiterAutoUnlock = false;
    await handler(event); assert.equal(calls.length, 0);
    sandbox.kpxc.settings.jupiterAutoUnlock = true;
    await handler(event); assert.equal(calls.length, 1);
    assert.equal(calls[0][0], 'get_database_hash');
    assert.equal(calls[0][1][0], false); assert.equal(calls[0][1][1], true);
    await handler(event); assert.equal(calls.length, 1); // cancelled-prompt cooldown
    now += 30001; await handler(event); assert.equal(calls.length, 2);
    now += 30001; resolve = r => { sandbox.finish = r; };
    const pending = handler(event); await Promise.resolve(); await Promise.resolve();
    await handler(event); assert.equal(calls.length, 3); // no simultaneous prompts
    sandbox.finish(); await pending;
    resolve = null;
    now += 30001;
    const password = new Input(); password.type = 'password';
    let rescans = 0;
    sandbox.kpxc.initCredentialFields = async () => {
        rescans++;
        sandbox.kpxc.combinations = [{ password }];
    };
    const staged = { isTrusted: true, composedPath: () => [password] };
    await handler({ ...staged, isTrusted: false }); assert.equal(rescans, 0);
    await handler(staged); assert.equal(rescans, 1); assert.equal(calls.length, 4);
    now += 30001;
    await handler(staged); assert.equal(rescans, 1); // no repeated scan of the same input
    console.log('KEEPASSXC_UNLOCK_PASS user gesture, matching field, top frame, lock state, cooldown, coalescing');
};
run().catch(error => { console.error(error); process.exitCode = 1; });
