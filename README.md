# Jupiter

Jupiter is a port of the actual GNOME Web (Epiphany 51.1) GTK/libadwaita shell to
Chromium Embedded Framework. Work happens on `main`. The native tab bar,
bookmarks sidebar, address bar, icons, and UI templates come from Epiphany.

The engine replacement is **in progress**. Pages render through CEF, including
internal URI handlers, JavaScript evaluation, navigation history, snapshots,
printing, and favicons. WebKitGTK/JavaScriptCore are still linked for remaining
service/model APIs. Network storage, settings, native permission/download
integration, the page-script bridge, and the extension-manager UI still need
migration. Tab groups are not implemented in this fork yet.

Full uBlock Origin MV2 runs in Chromium's real extension runtime. Alloy tab APIs
have a native adapter; this is not uBlock Lite or a replacement URL blocker.
The compatibility check covers network filtering, cosmetic filtering in the main
page and an iframe, and a scriptlet. It is not a claim of complete extension API
coverage. Ordinary browsing keeps Chromium's sandbox enabled.

## Run

```sh
nix run .
```

The Nix package pins the CEF SDK and official full uBlock Origin. Jupiter uses
its own application ID and profile, separate from Epiphany and Select.

## Develop and check

```sh
nix develop
meson setup build-cef -Ddeveloper_mode=true -Dman-pages=disabled \
  -Dcef_root="$CEF_ROOT" -Dublock_dir="$UBLOCK_DIR"
meson compile -C build-cef
meson test -C build-cef --print-errorlogs 'Engine favicon cache' 'Engine Chromium origins'

cmake -S engine -B engine/build -DCEF_ROOT="$CEF_ROOT" -DUBLOCK_DIR="$UBLOCK_DIR"
cmake --build engine/build
python3 engine/tools/check-native.py engine/build/engine-view-test
python3 engine/tools/check-extensions.py engine/build/extension-probe
```

The native checks use a local HTTP fixture, Xvfb, and disposable profiles. The
isolated CEF diagnostic processes disable their sandbox; the application does
not. The inherited upstream test suite still needs its engine-dependent fixtures
ported to CEF; a successful compile does not mean that whole suite passes.

The original project's documentation is preserved in
[README.epiphany.md](README.epiphany.md). Epiphany's original license and
copyright notices remain in place. The package includes CEF's license and
Chromium third-party credits.
