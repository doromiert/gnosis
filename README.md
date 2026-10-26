# Gnosis

Gnosis (formerly Jupiter) uses GNOME Web's GTK/libadwaita shell with an embedded
Brave backend. Tabs, bookmarks, address entry and application controls are native
GTK. The supplied `gnosis.svg` is the application artwork.
Source and issues: [doromiert/gnosis](https://github.com/doromiert/gnosis).

Run `nix run .` or `nix run .#gnosis`. The primary executable is `gnosis`;
`jupiter` remains a compatibility alias. Existing profile directories, GSettings
schemas, application identifiers and KeePassXC identity are retained so the
rename preserves browsing data, settings and database pairing.

Enable **Vim Mode** in **Preferences → General → Browsing** for keyboard navigation.
It is off by default and takes effect immediately in open tabs.

- `h/j/k/l` scroll, `d/u` move half a page, `gg`/`G` go to the top/bottom.
  Prefix scrolling commands with a count, such as `3j`.
- `f` shows link/control hints; type a hint to activate it. `F` opens a hinted
  HTTP(S) link in a new tab. Escape cancels hints.
- `/` opens native find; `n`/`N` select the next/previous match.
- `H`/`L` go back/forward; `J`/`K` cycle tabs; `o` focuses the address bar;
  `t` opens a tab, `r` reloads and `x` closes through the normal close safeguards.
- `gi` focuses a text field. Fields, editors and composing text keep normal
  input. `i` temporarily passes all keys to a website; Escape returns to normal mode.

F3 and Shift+F3 use the native GTK find bar, including when Vim mode is off.

The engine migration is **in progress**. Brave provides the active browser
backend, extensions and native engine services. WebKitGTK/JavaScriptCore and the
CEF compatibility layer remain linked for services still being migrated. See
[the current migration notes](BRAVE-MIGRATION.md) and
[the live checklist](JUPITER-TODO.md) for verified coverage and remaining gaps.

The following sections retain notes from the earlier CEF implementation.

JavaScript evaluation runs in Chromium's main context or a native named isolated
world, using unique context identities across navigation. Isolated worlds share
the page DOM without sharing globals or gaining cross-origin access. Cancellation,
tab closure, and moving a view between windows resolve pending evaluations.
Chromium document/context destruction cancels them without awaiting a late
DevTools reply, and context removal checks the unique identity before numeric IDs.
Hiding a tab preserves its scripts. Declarative user scripts and private-world
messages still need porting. Internal top-level pages use native CEF messages
with JSON payloads: Reload, certificate-action callbacks, and web-app controls no
longer use WebKit/JSC message objects. The adapter rejects websites, subframes,
forged alternate-page URLs, and unregistered handlers, and keeps deliveries local
to each regular/private view. Certificate-error handling itself still needs its
native service migration. Returned values are native JSON descriptions from
Chromium, preserving nested values, Unicode, null/undefined, BigInt, and non-finite
numbers without constructing a JavaScriptCore context. Existing consumers use
JSON-GLib directly; remaining JavaScriptCore use belongs to legacy services and
the private page-message bridge.

Main-document navigation initializes native resources separately from overall
loading activity. Fragment changes, same-document Back, and subframe loads keep
the current resource, response metadata, and pending isolated-world scripts.
Same-document history refreshes without emitting false document-load events;
cached Back navigations retain the original response metadata.

Chromium load failures now reach Epiphany's actual error-page template, whose
Reload button navigates to the original destination. Internal SVG and CSS replies
infer their resource MIME types, so Chromium renders their icons and styles;
the templates use the standard system font family. Installed web-app icons load
through per-view native resource URLs. Only bounded decoded image data is served;
malformed or missing files use GNOME's fallback, and icon URLs expire on reload.

Permission requests now use native CEF objects and Epiphany's actual dialogs,
including microphone/camera requests and general permission prompts. The requesting
origin is kept separate from the top-level page. Navigation, subframe removal, and tab closure
withdraw pending requests; Ask Later dismisses them without saving a choice.
Notification, location, microphone, and camera choices now use Chromium's content
settings. Existing policy-file choices are imported once before navigation;
GNOME's website controls update the same store as JavaScript permission queries,
and revocation survives restart. Clipboard choices apply to each request. Private
choices remain in their own memory-only context and ignore the regular policy
file. Media checks use Chromium's diagnostic fake devices, not a claim of
physical webcam coverage. The old WebKit query hook is removed. Location/Bluetooth
services, native WebAuthn UI, and screen-capture source selection still need work.
Password, advertisement, and autoplay policies retain their existing backend;
the legacy page-script permission export still needs migration.

Clipboard completions return to the requesting V8 world, including concurrent
isolated worlds and replacement documents after reload. Text reads first pass
Chromium's original native permission and frame-policy checks, then read GTK's
actual clipboard; this fixes empty reads from the offscreen Wayland backend.
Native focus checks reject background operations. Main/private-world custom
Copy handlers retain their formats, and Cut deletes the selection once.
Rich `navigator.clipboard.read()` also reads GTK formats after Chromium grants
the request. It supports text, HTML/SVG, encoded PNG and native GTK textures,
and Figma's binary format. PNG bytes retain their encoding and metadata; decoding
stays in the renderer. Chromium's safe HTML API sanitizes markup in an inert
document; inline formatting survives, and explicit unsanitized HTML remains
available. Selecting formats avoids reading unwanted data, and a clipboard
change during a pending read cancels it. Generic web-custom-format interoperability
and Chromium's full HTML style-inlining behavior still need work.

Reader Mode extracts the current Chromium document before navigating, then
serves the prepared HTML through Epiphany's existing internal scheme. Live DOM
edits survive extraction. Explicit reader URLs use a separate native source view
in the same regular/private session; normal quit also works during a pending
source request. The actual Find entry receives selected page text from Chromium.

Session placeholders and alternate error pages retain their intended address;
their internal data URLs are excluded from the address bar and saved session.
Selecting a restored placeholder or reloading an alternate page loads its real URL.

Full uBlock Origin MV2 runs in Chromium's real extension runtime. Alloy tab APIs
have a native adapter; this is not uBlock Lite or a replacement URL blocker.
The compatibility check covers network filtering, cosmetic filtering in the main
page and an iframe, and a scriptlet. It is not a claim of complete extension API
coverage. Ordinary browsing keeps Chromium's sandbox enabled.
The unused WebKit JSON-filter manager and its startup downloads have been removed;
filtering is provided by the actual uBlock extension.

## Run

```sh
nix run .
```

The Nix package pins the CEF SDK and official full uBlock Origin. Gnosis uses
its own application ID and profile, separate from Epiphany and Select.

## Develop and check

```sh
nix develop
meson setup build-cef -Ddeveloper_mode=true -Dman-pages=disabled \
  -Dcef_root="$CEF_ROOT" -Dublock_dir="$UBLOCK_DIR"
meson compile -C build-cef -j$(nproc)
meson test -C build-cef --print-errorlogs 'Engine favicon cache' 'Engine Chromium origins'

cmake -S engine -B engine/build -DCEF_ROOT="$CEF_ROOT" -DUBLOCK_DIR="$UBLOCK_DIR"
cmake --build engine/build -j$(nproc)
python3 engine/tools/check-native.py engine/build/engine-view-test
python3 engine/tools/check-native.py engine/build/script-test
python3 engine/tools/check-native.py --desktop engine/build/capabilities-test
python3 engine/tools/check-native.py --desktop engine/build/capabilities-test --gpu-only
python3 engine/tools/check-extensions.py engine/build/extension-probe
python3 engine/tools/check-native.py --portals engine/build/portal-test --files-only
python3 engine/tools/check-native.py "$(command -v python3)" engine/tools/check-cache-ui.py \
  build-cef/src/gnosis --schemas build-cef/data
python3 engine/tools/check-native.py "$(command -v python3)" engine/tools/check-cookies-ui.py \
  build-cef/src/gnosis --schemas build-cef/data
```

The native checks use a local HTTP fixture, Xvfb, and disposable profiles.
The desktop capability check waits for its test window to be active before
exercising focus-protected clipboard operations. The GTK Find/Reader workflow
can be checked in regular and private mode with:

```sh
python3 engine/tools/check-native.py --timeout 90 "$(command -v python3)" \
  engine/tools/check-script-ui.py build-cef/src/gnosis --schemas build-cef/data
python3 engine/tools/check-native.py --timeout 90 "$(command -v python3)" \
  engine/tools/check-script-ui.py build-cef/src/gnosis --schemas build-cef/data --incognito
```
 The
isolated CEF diagnostic processes disable their sandbox; the application does
not. The inherited upstream test suite still needs its engine-dependent fixtures
ported to CEF; a successful compile does not mean that whole suite passes.
The portal check saves through Chromium's real writable file handle and verifies
the exact binary bytes and Unicode filename on disk. It also exercises uploads,
cancellation, hiding the page, and closing the window. Folder uploads remain excluded because of a known Chromium dialog
crash in the current offscreen embedding.

The original project's documentation is preserved in
[README.epiphany.md](README.epiphany.md). Epiphany's original license and
copyright notices remain in place. The package includes CEF's license and
Chromium third-party credits.
