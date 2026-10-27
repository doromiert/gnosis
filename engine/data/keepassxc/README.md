# Bundled KeePassXC integration

Gnosis packages the official KeePassXC-Browser **1.10.4.1** Chromium release,
pinned by SHA-256 in `package.nix`. Password saving/filling, TOTP, encryption,
database association, origin checks and WebAuthn/passkeys remain upstream code.
The package is independent of a separately installed KeePassXC extension:
`public-key.txt` fixes its identity to `giohadoodmdcadnkcpdmhmmdnaidnckm`.

`engine/tools/prepare-keepassxc.py` checks the pinned version and exact source
anchors before making these adaptations:

- Enable upstream passkey support by default; preserve saved user settings.
- Request KeePassXC's own unlock prompt on a user gesture in a detected top-level
  login field. Ignore synthetic events/background frames and coalesce prompts,
  with a 30-second cooldown after cancellation. No master password enters Gnosis.
- Remove the extension action/options UI and exclude the backend from Gnosis's
  extension list. All Gnosis controls live in the native libadwaita panel at
  **hamburger menu → KeePassXC**: pairing, unlock/lock, login selection, TOTP,
  passkeys and automatic-unlock settings. Only labels and entry IDs leave the
  backend; password/code values are filled by upstream content scripts.
- Style the closed-shadow page overlays using libadwaita popover/list geometry,
  live GTK palette/font variables and GNOME symbolic icons. Preserve upstream
  keyboard, origin, positioning and filling behavior.
- Rescan a newly focused password field once on trusted interaction. The native
  panel also offers **Redetect Login Fields** for staged forms. If this page
  predates the integration's startup, recover only manifest-declared scripts in
  its top frame, respecting URL exclusions and Chromium's host grants. This is
  why the adapter requests the `scripting` permission.
- Disable Brave's competing password-save and automatic-sign-in preferences.
  Existing native vault data is preserved.

The integration loads in the normal profile only. Private profiles do not
load it or receive a native host registration. Enable KeePassXC's Browser
Integration and choose **KeePassXC → Connect Database…** once to authorize
Gnosis. Unlocking an already paired database is separate from association;
KeePassXC must permit browser unlock requests for its password prompt to open.
KeePassXC itself must be installed, with `keepassxc-proxy` in PATH.

The bundled manifest uses an integration revision (`1.10.4.106`) and retains the
upstream version in `version_name`. Existing Chromium profiles can retain older
worker scripts after unpacked-resource changes. Gnosis resolves only the exact
internal worker URL, waits for upstream initialization and installs its trusted
GTK adapter from the pinned package through the private CDP pipe. It does not
clear worker caches, extension storage, association keys, or browsing data.

Brave searches a fixed default native-host path even with `--user-data-dir`.
Gnosis gives only its Brave process a profile-owned `native-config` directory.
Its host manifest permits only this bundled extension's origin. The generated
0700 proxy launcher restores the original desktop config directory and clears
Brave's foreign library/toolkit paths before executing the installed proxy. This
avoids loading a newer Nix GLib into the user's older KeePassXC runtime. No other
browser's host registration or KeePassXC settings are modified.

Upgrade by updating the release URL/hash/version assertion and reviewing the
small checked adaptations. Retain the public key to preserve association/storage.
Run `node engine/tests/keepassxc-unlock-test.js` and
`node engine/tests/keepassxc-service-test.js`, plus the actual native proxy/worker,
GTK-panel, closed-shadow overlay and full native uBlock integration probes. Live database pairing and actual
password/TOTP/passkey operations require a user-authorized test database; a
successful host handshake alone does not establish those round trips.

The user confirmed passkey use on GitHub. Discord did not respond in their test;
its cause is unconfirmed. A real host handshake alone does not validate every
credential, TOTP and passkey operation. Developer builds can use
`-Dkeepassxc_dir=$KEEPASSXC_DIR` from the Nix development shell.

Integration 5 rejects unsupported input types before visibility, hit testing and
layout reads. Credential and TOTP input types, visibility/search filtering and
closed-shadow discovery stay upstream. Repeated mutation checks for the same
node/action are collapsed within a callback; add/remove/add ordering is retained.
`keepassxc-observer-test.js` checks these contracts against the prepared release
and runs in the Nix package's check phase. Service and unlock tests also pass.

A controlled Chromium 155 test alternated old/new filters over five rounds of 200
scans, with 97 checkboxes and three credential fields plus a layout-invalidating
style change each iteration. Median time fell from 240.4ms to 26.1ms; visibility
checks fell from 20,000 to 600. Both versions returned exactly the same username,
password and numeric TOTP fields. This measures this scan path, not total
Speedometer throughput. Results: `/tmp/gnosis-keepassxc-filter-benchmark.json`.

Integration 6 batches mutation-driven field detection into one trailing check per
second. The timer is scheduled by the first change and is not reset by later
changes, so continuously animated pages cannot starve detection. Idle pages have
no timer. At most 200 mutation records are retained; overflow uses upstream mutation-based
discovery on the document body and stale-icon cleanup once, rather than dropping
new fields or restarting autofill. Non-credential controls are rejected before
the upstream input-count limit, so they cannot crowd out newly added login fields. Initial discovery, explicit Redetect Fields and WebAuthn remain
immediate. Hidden-page and mouse-down guards are checked again when work runs.
Dynamic forms may take up to one second to acquire their field icons.

User Speedometer 3.1 baseline with integration 5: 27.0 ±1.6 with KeePassXC,
34.8 ±1.5 without it. A CPU-instrumented integration-5 run scored 25.1, with
4.528 seconds inclusive in KeePassXC call stacks, chiefly the mutation observer.
The observer, service and unlock regression checks cover integration 6.
