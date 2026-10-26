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
manifest['version'] = '1.10.4.104'
manifest['version_name'] = '1.10.4.1 (Gnosis integration 4)'
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
