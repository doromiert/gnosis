// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const hash = 'a'.repeat(64), other = 'b'.repeat(64);
const entry = id => ({hash, id, key: Buffer.alloc(32, id.length).toString('base64')});
const clone = value => JSON.parse(JSON.stringify(value));
function worker(initial = {}) {
    const data = clone(initial), listeners = [], packets = [];
    const sandbox = vm.createContext({
        keepass: {keyRing: data.keyRing || {}, associated: {value: true, hash}},
        page: {clearAllLogins: () => {data.cachesCleared = (data.cachesCleared || 0)+1;}},
        browser: {storage: {
            local: {get: async () => clone(data), set: async values => {
                if (values.gnosisInheritedAssociations)
                    assert.deepEqual(clone(sandbox.keepass.keyRing), clone(values.keyRing),
                        'Upstream must use the imported ring during asynchronous storage writes');
                Object.assign(data, clone(values));
                for (const listener of listeners) listener(Object.fromEntries(Object.keys(values).map(key => [key, {}])), 'local');
            }},
            onChanged: {addListener: listener => listeners.push(listener)}
        }},
        __gnosisKeePassXCPairingChanged: payload => packets.push(JSON.parse(payload))
    });
    vm.runInContext(source, sandbox);vm.runInContext(source, sandbox);
    assert.equal(listeners.length, 1, 'Cached workers must register one storage listener');
    return {data, sandbox, packets, api: sandbox.gnosisKeePassXCPairing};
}
(async () => {
    const main = worker({keyRing: {[hash]: {...entry('main'), lastUsed: 123, unexpected: 'omit'}}, settings: {passkeys: true}});
    await main.api.publish();
    assert.deepEqual(main.packets, [{[hash]: entry('main')}]);
    await assert.rejects(main.api.apply({}), /main profile/);
    const app = worker({keyRing: {[hash]: entry('app'), [other]: {...entry('other'), hash: other}}, settings: {passkeys: false}});
    const another = worker();
    await app.api.apply(main.packets[0]);await another.api.apply(main.packets[0]);
    assert.equal(app.data.keyRing[hash].id, 'main');
    assert.equal(another.data.keyRing[hash].id, 'main');
    assert.equal(app.data.keyRing[other].id, 'other');
    assert.deepEqual(app.data.settings, {passkeys: false});
    assert.equal(app.sandbox.keepass.associated.value, false);
    assert.equal(app.data.cachesCleared, 1, 'Inherited trust changes invalidate upstream login caches');
    assert.deepEqual(app.packets, [], 'Consumers cannot publish into the main profile');
    assert.equal(await app.api.apply(main.packets[0]), false, 'Reimport is idempotent');
    await Promise.all([app.api.apply({[hash]: entry('updated')}), app.api.apply({[hash]: entry('latest')})]);
    assert.equal(app.data.keyRing[hash].id, 'latest', 'Concurrent updates remain ordered');
    await app.api.apply({});await another.api.apply({});
    assert.equal(app.data.keyRing[hash].id, 'app', 'Revocation restores an existing app pairing');
    assert.equal(another.data.keyRing[hash], undefined, 'Revocation removes inherited-only associations');
    const invalid = {[hash]: {...entry('bad'), password: 'must not be imported'}};
    await assert.rejects(app.api.apply(invalid), /Invalid shared/);
    await assert.rejects(app.api.apply({bad: entry('bad')}), /Invalid shared/);
    await app.api.apply(main.packets[0]); // Invalid input must not poison the import queue.
    assert.equal(app.data.keyRing[hash].id, 'main');
    assert.equal(main.data.keyRing[hash].id, 'main');
    const migrated = worker();await migrated.api.apply(main.packets[0]);
    const newHash = 'c'.repeat(64), moved = {...entry('main'), hash: newHash};
    migrated.data.keyRing = {[newHash]: moved};migrated.sandbox.keepass.keyRing = migrated.data.keyRing;
    await migrated.api.apply({[newHash]: moved});await migrated.api.apply({});
    assert.equal(migrated.data.keyRing[newHash], undefined, 'An upstream hash migration must retain inherited provenance');
    console.log('KEEPASSXC_PAIRING_PASS shared association, app data preservation, revocation, idempotence, validation and ordered updates');
})().catch(error => {console.error(error);process.exitCode = 1;});
