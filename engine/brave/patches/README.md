# Native Brave integration patches

Keep this series small: add an opt-in hook at a stable browser component,
leave GTK UI and policy in Jupiter, and preserve ordinary Brave behavior when
the switch is absent. Do not copy Views layouts or patch binary offsets.

`series.json` records the Brave/Chromium release pair, pristine source hashes,
and ordered patch hashes. The first patch changes one component file to honor
`--jupiter-native-ui` when deciding whether to show zoom feedback. Native zoom,
layout, events, and per-tab settings remain intact. The mode-transition path
also respects the visibility flag; suppressing extension zoom alone missed it.

This is a source patch. The stock prebuilt Brave package does not contain it;
its zoom popup is not fixed until a patched browser is built and tested.

Check the pinned release without fetching a full browser checkout:

```sh
nix run .#check-brave-patches
```

Or check an initialized Brave checkout after Brave applies its own patches:

```sh
python3 engine/tools/check-brave-patches.py /path/to/brave-browser/src
```

Probe a release upgrade before changing the pin:

```sh
nix run .#check-brave-patches -- --fetch --brave-version 1.100.31 --upgrade \
  --report /tmp/jupiter-brave-upgrade.json \
  --candidate-pin /tmp/jupiter-brave-series-next.json
```

The checker resolves Chromium from that Brave release's own `package.json`.
It fetches only the touched sources and refuses to simulate a release where
Brave adds an override to those files: use an initialized checkout for that
case. Neither fetch mode nor local mode changes the supplied checkout or pin.
Candidate mode permits source/version drift, but still enforces patch hashes,
exact hunk context (zero fuzz), forward application, and byte-identical reversal.
Line offsets are allowed so unrelated upstream insertions do not need a rebase.
Failures identify the changed patch, source, release pair, or rejected hunk.

After source checks, review the proposed pin and build the patched Brave release.
Apply from its Chromium `src` root, after Brave initialization:

```sh
patch --batch --fuzz=0 --forward -p1 < /path/to/jupiter/engine/brave/patches/0001-native-zoom-feedback.patch
```

Required runtime checks before accepting an upgrade: GTK zoom buttons,
Ctrl+plus/minus/reset, Ctrl+wheel, normal/private profiles, reload/navigation,
actual page reflow, and absence of the native popup. Also launch Brave without
the Jupiter switch to verify its normal zoom feedback remains available.
Source checks never establish compilation or runtime compatibility.

The Nix package accepts a `brave` package argument for the production build.
For a local patched executable, the launcher also honors explicit overrides:

```sh
JUPITER_BRAVE_EXECUTABLE=/path/to/patched/brave \
JUPITER_BRAVE_VERSION=1.97.56 nix run .
```

This does not build or patch Brave for you. The executable must already contain
the checked source patch and have its runtime dependencies available.

`nix flake check` includes the pinned source check and detects when nixpkgs'
Brave version drifts from this pin. The default app still uses stock Brave;
this series is not silently applied to a prebuilt executable.
