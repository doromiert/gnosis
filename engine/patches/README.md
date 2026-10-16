# CEF source fixes

`cef-directory-picker.patch` targets CEF commit `c1a097e`, used by the pinned
150.0.22 SDK. It preserves folder selection when Chromium's File System Access
API reaches CEF's native dialog handler. The original conversion handles upload
folders but drops both ordinary folder modes, reporting an open-file request.

The patch applies to the pinned source. A real `showDirectoryPicker()` gesture
was observed arriving in `Browser::OnFileDialog` as `FILE_DIALOG_OPEN` (0),
and opening a GTK file chooser. This is separate from the upload confirmation
crash triggered by `<input webkitdirectory>`.

This source patch is **not applied to the prebuilt SDK by package.nix**. It needs
a rebuilt CEF binary and an end-to-end directory-handle/portal regression before
the directory-picker issue can be marked fixed. The running app still has it.

`cef-user-styles.patch` targets the same pinned commit. It adds experimental,
renderer-only `CefFrame::InsertUserStyleSheet` and `RemoveUserStyleSheet` APIs
backed by Blink's real USER cascade. Keys belong to their frame's current
document; stale, foreign and already removed keys are rejected. This is a source
patch, not a replacement using author styles or JavaScript DOM elements.

The patch passes forward/reverse apply checks at the exact pin. The pinned CEF
translator generated and checked both C API layouts and both C++ wrapper
directions; the existing stable frame layout remains unchanged. Full C++
compilation and runtime stylesheet tests require a rebuilt SDK and are pending.

After applying it in a complete pinned Chromium checkout at `src/cef`, regenerate
the bindings and API hashes with CEF's own tools before building the SDK:

```sh
python3 tools/translator.py --root-dir .
python3 tools/version_manager.py --update
python3 tools/version_manager.py --check --fast-check
```

Ship matching headers, library, resources and wrapper, and rebuild Jupiter with
the experimental API. Do not change the stable API hashes or install new headers
over the current prebuilt library. Named-world lifecycle and document-end hooks
still need separate native integration; this patch does not complete the
user-content manager migration.
