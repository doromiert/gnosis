#!/usr/bin/env python3
"""Small checked adaptation of the pinned official KeePassXC-Browser release.

Keep encryption, association, URL matching, credentials, TOTP and WebAuthn in
upstream code. This identity/storage is independent of a separately installed
KeePassXC-Browser. No remotely connectable extension APIs are added.
"""
import json
import pathlib
import sys

root, assets = map(pathlib.Path, sys.argv[1:])
manifest_path = root / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
assert manifest['version'] == '1.10.4.1', 'Review the integration when upgrading'
# Separate integration revision forces Chromium to retire cached worker code.
manifest['version'] = '1.10.4.106'
manifest['version_name'] = '1.10.4.1 (Gnosis integration 6)'
manifest['permissions'].append('scripting')
manifest['key'] = (assets / 'public-key.txt').read_text().strip()
manifest.pop('update_url', None)
# Existing users retain their settings. Enable upstream WebAuthn on first use.
page = root / 'background/page.js'
text = page.read_text()
assert text.count('    passkeys: false,') == 1
page.write_text(text.replace('    passkeys: false,', '    passkeys: true,'))
(root / 'content/jupiter-unlock.js').write_text((assets / 'unlock.js').read_text())
manifest['content_scripts'][0]['js'].append('content/jupiter-unlock.js')
# The GTK shell owns the integration UI; retain upstream as a private service.
manifest.pop('action', None)
manifest.pop('options_ui', None)
(root / 'background/jupiter.js').write_text((assets / 'service.js').read_text())
service = root / 'background/background_service.js'
text = service.read_text()
assert text.count("        'init.js'\n") == 1
service.write_text(text.replace("        'init.js'\n", "        'init.js',\n        'jupiter.js'\n"))
# Reject non-credential inputs before any visibility/layout reads.
observer = root / 'content/observer-helper.js'
text = observer.read_text()
before = """    const inputAllowed = (elem) => !elem.disabled
        && elem.getLowerCaseAttribute('type') !== 'hidden'"""
after = """    const inputAllowed = (elem) => !elem.disabled
        && kpxcObserverHelper.inputTypes.includes(elem.getLowerCaseAttribute('type'))"""
assert text.count(before) == 1, 'Review upstream input collection'
text = text.replace(before, after)
before = '''    for (const field of inputFields) {
        if ((!ignoreVisibility && !kpxcFields.isVisible(field))
            || kpxcFields.isSearchField(field)) {
            continue;
        }

        const type = field.getLowerCaseAttribute('type');
        if (kpxcObserverHelper.inputTypes.includes(type)) {
            inputs.push(field);
        }
    }'''
after = '''    for (const field of inputFields) {
        const type = field.getLowerCaseAttribute('type');
        if (!kpxcObserverHelper.inputTypes.includes(type)
            || (!ignoreVisibility && !kpxcFields.isVisible(field))
            || kpxcFields.isSearchField(field)) {
            continue;
        }
        inputs.push(field);
    }'''
assert text.count(before) == 1, 'Review upstream field filtering'
text = text.replace(before, after)
# Collapse repeated checks in a batch, preserving add/remove/add ordering.
start = text.index('kpxcObserverHelper.initObserver = async function() {')
end = text.index('// Stores mutation style', start)
handler = text[start:end]
anchor = '        const styleMutations = [];'
assert handler.count(anchor) == 1
handler = handler.replace(anchor, '''        const pending = [], operations = new Map();
        const queue = (target, removal = false) => {
            if (operations.get(target) === removal) return;
            operations.set(target, removal);
            pending.push([target, removal]);
        };
''' + anchor)
for before, after in [
    ('kpxcObserverHelper.handleObserverAdd(node);', 'queue(node);'),
    ('kpxcObserverHelper.handleObserverRemove(node);', 'queue(node, true);'),
    ('kpxcObserverHelper.handleObserverAdd(mut.target);', 'queue(mut.target);'),
    ('kpxcObserverHelper.handleObserverAdd(styleMut.target);', 'queue(styleMut.target);'),
    ('kpxcObserverHelper.handleObserverRemove(styleMut.target);', 'queue(styleMut.target, true);'),
]:
    assert handler.count(before) == 1, 'Review upstream mutation handling'
    handler = handler.replace(before, after)
anchor = '    });\n\n    if (document.body)'
assert handler.count(anchor) == 1
handler = handler.replace(anchor, '''        for (const [target, removal] of pending) {
            if (removal) kpxcObserverHelper.handleObserverRemove(target);
            else kpxcObserverHelper.handleObserverAdd(target);
        }
''' + anchor)
# Schedule mutation work once per second without polling or starving busy pages.
anchor = '    kpxc.observer = new MutationObserver(function(mutations, obs) {'
assert handler.count(anchor) == 1
handler = handler.replace(anchor, '    const processMutations = function(mutations) {')
anchor = '    });\n\n    if (document.body)'
assert handler.count(anchor) == 1
handler = handler.replace(anchor, '''    };
    let queued = [], overflow = false, timer = 0;
    kpxc.observer = new MutationObserver(function(mutations) {
        if (document.visibilityState === 'hidden' || kpxcUI.mouseDown) return;
        if (!overflow) {
            if (queued.length + mutations.length <= MAX_MUTATIONS) queued.push(...mutations);
            else { queued = []; overflow = true; }
        }
        if (timer) return;
        timer = setTimeout(function() {
            timer = 0;
            const batch = queued, rescan = overflow;
            queued = []; overflow = false;
            if (document.visibilityState === 'hidden' || kpxcUI.mouseDown) return;
            if (rescan) {
                // Bound retained records without missing fields on busy pages.
                kpxcObserverHelper.handleObserverAdd(document.body);
                kpxcIcons.deleteAllHiddenIcons();
            } else processMutations(batch);
        }, 1000);
    });

    if (document.body)''')
observer.write_text(text[:start] + handler + text[end:])
# Keep the upstream closed Shadow DOM and trusted input handlers. Only append
# scoped presentation rules and replace colored logos with GNOME symbolic icons.
overlays = (assets / 'overlays.css').read_text()
for css in (root / 'css').glob('*.css'):
    css.write_text(css.read_text() + '\n' + overlays)
for name, source in {'keepassxc.svg': 'password.svg', 'key.svg': 'password.svg',
                     'locked.svg': 'locked.svg', 'otp.svg': 'otp.svg',
                     'disconnected.svg': 'disconnected.svg'}.items():
    (root / 'icons' / name).write_bytes((assets / 'icons' / source).read_bytes())
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
