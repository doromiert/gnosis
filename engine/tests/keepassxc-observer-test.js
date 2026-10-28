// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = process.argv[2] || process.env.KEEPASSXC_DIR;
assert(root, 'Pass the prepared pinned KeePassXC directory');
let callback, layoutReads = 0;
let timers = [], rescans = 0, iconCleanups = 0;
const flush = () => {
    assert.equal(timers.length, 1, 'Only one trailing check may be scheduled');
    const task = timers.shift();
    assert.equal(task.delay, 1000);
    task.fn();
};
const field = (type, extra = {}) => ({
    nodeType: 1, nodeName: 'INPUT', className: '', disabled: false,
    classList: {contains: () => false}, type,
    getLowerCaseAttribute: name => name === 'type' ? type : null,
    ...extra
});
const sandbox = {
    setTimeout: (fn, delay) => { timers.push({fn, delay}); return 1; },
    window: {MutationObserver: class {constructor(fn) { callback = fn; } observe() {}}},
    document: {body: {}, visibilityState: 'visible'},
    Node: {ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
        PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_TYPE_NODE: 10, NOTATION_NODE: 12},
    kpxc: {inputs: [], settings: {useObserver: true}}, kpxcUI: {},
    kpxcIcons: {deleteAllHiddenIcons: () => { iconCleanups++; }},
    kpxcFields: {
        isVisible: input => {
            assert(!['checkbox', 'radio', 'button', 'submit', 'range', 'color', 'file'].includes(input.type),
                'Non-credential controls must not force layout');
            layoutReads++;
            return !input.hidden;
        },
        isSearchField: input => !!input.search,
        hasOverlay: () => false
    },
    kpxcSites: {exceptionFound: () => false},
    kpxcForm: {formIdentified: () => false},
    browser: {dom: {openOrClosedShadowRoot: target => target.shadowRoot}},
    matchesWithNodeName: (target, name) => target.nodeName?.toLowerCase() === name.toLowerCase(),
    logDebug() {}
};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(root, 'content/observer-helper.js'), 'utf8') +
    '\nglobalThis.helper = kpxcObserverHelper;', sandbox);
const helper = sandbox.helper;
const password = field('password'), email = field('email'), otp = field('number');
const values = ['checkbox', 'radio', 'button', 'submit', 'range', 'color', 'file'].map(t => field(t));
values.push(password, email, otp, field('text', {hidden: true}), field('text', {search: true}));
const target = {nodeType: 1, nodeName: 'FORM', className: '', childElementCount: values.length,
    querySelectorAll: () => values, getElementsByTagName: () => []};
assert.deepEqual(Array.from(helper.getInputs(target)), [password, email, otp]);
assert.equal(layoutReads, 5);
layoutReads = 0;
assert.equal(helper.getInputs(target, true).length, 4);
assert.equal(layoutReads, 0);
(async () => {
    await helper.initObserver();
    const adds = [], removes = [];
    helper.handleObserverAdd = node => adds.push(node);
    helper.handleObserverRemove = node => removes.push(node);
    helper.cacheStyle = () => {};
    callback([
        {type: 'childList', target, addedNodes: [target, target], removedNodes: [target, target]},
        {type: 'attributes', target, attributeName: 'class'},
        {type: 'attributes', target, attributeName: 'style'}
    ]);
    callback([{type: 'attributes', target, attributeName: 'class'}]);
    assert.deepEqual(adds, [], 'Mutation callbacks must not scan immediately');
    assert.deepEqual(removes, []);
    flush();
    assert.deepEqual(adds, [target, target]);
    assert.deepEqual(removes, [target]);
    assert.equal(timers.length, 0, 'Idle pages must not be polled');
    const order = [];
    helper.handleObserverAdd = node => {
        if (node === sandbox.document.body) {
            rescans++;
            assert.deepEqual(Array.from(helper.getInputs(target)), [password, email, otp]);
        } else order.push(['add', node]);
    };
    helper.handleObserverRemove = node => order.push(['remove', node]);
    callback([
        {type: 'childList', target, addedNodes: [target], removedNodes: [target]},
        {type: 'childList', target, addedNodes: [target], removedNodes: []}
    ]);
    flush();
    assert.deepEqual(order.map(([kind]) => kind), ['add', 'remove', 'add']);
    const crowded = {...target, querySelectorAll: () => [...Array(150).fill(field('checkbox')), password, email, otp]};
    assert.deepEqual(Array.from(helper.getInputs(crowded)), [password, email, otp],
        'Non-credential controls must not exhaust the upstream input limit');
    const mutation = {type: 'childList', target, addedNodes: [target], removedNodes: []};
    callback(Array(201).fill(mutation));
    callback(Array(1000).fill(mutation));
    assert.equal(rescans, 0);
    flush();
    assert.equal(rescans, 1, 'Overflow must coalesce into one credential rescan');
    assert.equal(iconCleanups, 1);
    assert.equal(timers.length, 0);
    callback([mutation]);
    sandbox.document.visibilityState = 'hidden';
    flush();
    assert.equal(rescans, 1);
    assert.equal(order.length, 3, 'Deferred checks must respect hidden pages');
    sandbox.document.visibilityState = 'visible';
    helper.handleObserverAdd(target);
    assert.equal(order.length, 4, 'Explicit checks must remain immediate');
    assert.equal(timers.length, 0);
    console.log('KEEPASSXC_OBSERVER_PASS credential/TOTP discovery, early type filtering, one-second batching, bounded overflow rescan, add/remove order, idle/hidden handling, immediate explicit checks');
})().catch(error => {console.error(error);process.exitCode = 1;});
