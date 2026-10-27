// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = process.argv[2] || process.env.KEEPASSXC_DIR;
assert(root, 'Pass the prepared pinned KeePassXC directory');
let callback, layoutReads = 0;
const field = (type, extra = {}) => ({
    nodeType: 1, nodeName: 'INPUT', className: '', disabled: false,
    classList: {contains: () => false}, type,
    getLowerCaseAttribute: name => name === 'type' ? type : null,
    ...extra
});
const sandbox = {
    window: {MutationObserver: class {constructor(fn) { callback = fn; } observe() {}}},
    document: {body: {}, visibilityState: 'visible'},
    Node: {ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
        PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_TYPE_NODE: 10, NOTATION_NODE: 12},
    kpxc: {inputs: [], settings: {useObserver: true}}, kpxcUI: {},
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
    assert.deepEqual(adds, [target, target]);
    assert.deepEqual(removes, [target]);
    const order = [];
    helper.handleObserverAdd = node => order.push(['add', node]);
    helper.handleObserverRemove = node => order.push(['remove', node]);
    callback([
        {type: 'childList', target, addedNodes: [target], removedNodes: [target]},
        {type: 'childList', target, addedNodes: [target], removedNodes: []}
    ]);
    assert.deepEqual(order.map(([kind]) => kind), ['add', 'remove', 'add']);
    console.log('KEEPASSXC_OBSERVER_PASS credential and OTP discovery, search/visibility filtering, zero checkbox layout reads, deduplicated mutations with add/remove/add order preserved');
})().catch(error => {console.error(error);process.exitCode = 1;});
