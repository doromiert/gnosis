// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

class Element {
    constructor() { this.isConnected = true; }
    closest() { return null; }
    getBoundingClientRect() { return {x: 10, y: 20}; }
}
class HTMLMediaElement extends Element {
    constructor() {
        super(); this.paused = true; this.muted = false; this.loop = false;
        this.controls = false; this.currentSrc = 'https://media.invalid/clip.webm';
    }
    async play() { this.paused = false; }
    pause() { this.paused = true; }
}
class HTMLVideoElement extends HTMLMediaElement {
    async requestPictureInPicture() { document.pictureInPictureElement = this; }
    async requestFullscreen() { document.fullscreenElement = this; }
}
class HTMLAudioElement extends HTMLMediaElement {}
const document = {
    pictureInPictureEnabled: true, fullscreenEnabled: true,
    async exitPictureInPicture() { this.pictureInPictureElement = null; },
    async exitFullscreen() { this.fullscreenElement = null; }
};
let onMenu;
const messages = [];
const sandbox = {
    Element, HTMLMediaElement, HTMLVideoElement, HTMLAudioElement,
    HTMLAnchorElement: class extends Element {}, HTMLAreaElement: class extends Element {},
    HTMLImageElement: class extends Element {}, HTMLInputElement: class extends Element {},
    HTMLTextAreaElement: class extends Element {}, WeakRef, document,
    getSelection: () => '',
    window: {addEventListener(name, callback) { assert.equal(name, 'contextmenu'); onMenu = callback; }},
    __gnosisContextMenu: message => messages.push(JSON.parse(message))
};
vm.runInNewContext(fs.readFileSync('engine/data/native-integration/context-menu.js', 'utf8'), sandbox);
assert.equal(sandbox.__gnosisContextMenu, undefined, 'raw native binding must be removed');
const action = sandbox.__gnosisContextMenuAction;
function menu(element, extra = {}) {
    let prevented = false;
    onMenu({isTrusted: true, defaultPrevented: false, composedPath: () => [element],
        button: 2, clientX: 0, clientY: 0, preventDefault() { prevented = true; }, ...extra});
    return prevented ? messages.at(-1) : null;
}
(async () => {
    const first = new HTMLVideoElement();
    assert.equal(menu(first, {isTrusted: false}), null, 'synthetic events cannot create targets');
    assert.equal(menu(first, {defaultPrevented: true}), null, 'website-owned context menus remain respected');
    assert.equal(messages.length, 0);
    const hit = menu(first);
    assert.deepEqual([hit.x, hit.y], [0, 0], 'viewport-edge coordinates retain zero');
    assert.equal(hit.media.kind, 'video');
    assert.equal(hit.media.src, first.currentSrc);
    assert.equal(hit.media.paused, true);
    assert.equal(hit.media.pictureInPicture, true);
    assert.equal(hit.media.fullscreen, true);
    assert.equal(await action('forged', 'play'), false, 'wrong target rejected');
    assert.equal(await action(hit.target, 'play'), true);
    assert.equal(first.paused, false);
    assert.equal(await action(hit.target, 'pause'), true);
    assert.equal(first.paused, true);
    for (const [operation, field, value] of [
        ['mute', 'muted', true], ['unmute', 'muted', false],
        ['loop', 'loop', true], ['unloop', 'loop', false],
        ['show-controls', 'controls', true], ['hide-controls', 'controls', false]
    ]) {
        assert.equal(await action(hit.target, operation), true);
        assert.equal(first[field], value);
    }
    await action(hit.target, 'picture-in-picture');
    assert.equal(document.pictureInPictureElement, first);
    assert.equal(menu(first).media.inPictureInPicture, true);
    const current = messages.at(-1);
    assert.equal(await action(hit.target, 'pause'), false, 'superseded menu token rejected');
    await action(current.target, 'picture-in-picture');
    assert.equal(document.pictureInPictureElement, null);
    await action(current.target, 'fullscreen');
    assert.equal(document.fullscreenElement, first);
    await action(current.target, 'fullscreen');
    assert.equal(document.fullscreenElement, null);
    assert.equal(await action(current.target, 'arbitrary-script'), false, 'unknown operation rejected');
    const second = new HTMLVideoElement();
    const next = menu(second);
    assert.equal(await action(current.target, 'play'), false, 'old menu cannot control a different media element');
    second.isConnected = false;
    assert.equal(await action(next.target, 'play'), false, 'detached media rejected');
    const audio = new HTMLAudioElement();
    const audioHit = menu(audio);
    assert.equal(audioHit.media.kind, 'audio');
    assert.equal(audioHit.media.pictureInPicture, false);
    assert.equal(audioHit.media.fullscreen, false);
    assert.equal(await action(audioHit.target, 'fullscreen'), false);
    assert.equal(await action(audioHit.target, 'picture-in-picture'), false);
    assert.equal(await action(audioHit.target, 'play'), true);
    const plain = menu(new Element());
    assert.equal(plain.media, null);
    assert.equal(await action(audioHit.target, 'pause'), false, 'a later non-media menu retires the media target');
    assert.equal(await action(plain.target, 'play'), false);
    console.log('CONTEXT_MEDIA_PASS trusted events, real target operations, video/audio capabilities, stale and detached targets');
})().catch(error => { console.error(error); process.exitCode = 1; });
