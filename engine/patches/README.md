# CEF source fixes

The directory-picker, USER stylesheet, user-content lifecycle and certificate
overridability patches compose in that order at the exact pin below, using
`patch --fuzz=0`. Applying them in reverse order restores all 16 affected source
files byte for byte. This verifies source composition; matching SDK generation,
compilation and runtime checks are still required.

The recorded source and patch hashes live in `series.json`. Reproduce the full
forward/reverse check against a pristine pinned `src/cef` directory with:

```sh
python3 engine/tools/check-cef-patches.py /path/to/src/cef --report /tmp/cef-patch-check.json
```

The checker copies only the affected sources into a temporary directory, rejects
source or patch drift, and leaves the supplied checkout unchanged. It runs no
Git commands, downloads, SDK generators or builds. Added files must disappear on
reversal, and original files must return byte for byte. This is a source check;
it does not make the prebuilt SDK contain the patches.

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
over the current prebuilt library. This patch does not complete the user-content
manager migration.

`cef-user-content-lifecycle.patch` adds experimental renderer
`CefRenderProcessHandler::OnDocumentStart` and `OnDocumentEnd` callbacks at
Chromium's actual script injection points. Start runs after Blink updates the
current window/document bindings. Stock renderer `OnLoadStart` can still expose
an old cached JavaScript document despite a new committed URL; the retained
document-start diagnostic catches that failure. Both hooks chain Chrome's
existing implementation first, then check retained native frame references and
the document token before delivering the callback. An API-version guard
preserves older clients.

Forward/reverse dry runs pass against the same exact CEF pin. Pure pinned CEF
generators produced and checked all six callback ABI files with the stable
handler layout unchanged. Full SDK generation, hashes, C++ compilation and
runtime callback checks remain pending; this patch is also unapplied to the
prebuilt SDK. It does not supply named worlds or live per-browser configuration
snapshots for newly created renderer processes.

`cef-certificate-overridability.patch` preserves Chromium's native certificate
override policy in CEF. It uses Chromium's origin/profile policy and SSL options
helper, including strict transport enforcement and fatal errors. An immutable
request flag guards the native continuation callback, including asynchronous
calls, and an experimental `CefSSLInfo::IsCertificateErrorOverridable` getter
exposes it to the application. Unknown metadata defaults to false. Native cached
certificate decisions retain Chromium's existing behavior.

The patch includes explicit GN dependencies for the existing policy and SSL
helpers. Exact-pin forward/reverse checks and all six generated SSLInfo binding
checks pass, with the stable layout unchanged. SDK compilation, API hashes,
actual strict/HSTS/policy tests and Jupiter's acceptance-action guard still need
the matching rebuilt SDK. The current prebuilt library does not contain this fix.

`cef-extension-management.patch` adds four experimental, trusted-embedder
`CefRequestContext` operations: `GetInstalledExtensions`,
`LoadUnpackedExtension`, `SetExtensionEnabled`, and `UninstallExtension`.
`CefExtensionManagementCallback::OnExtensionManagementComplete(error, id,
extensions)` returns the actual native result and a copied registry snapshot.
The receiver's owner profile is used; off-the-record profiles fail without
redirecting to a normal/global profile. Snapshot `enabled` means actual enabled
registry membership, with terminated/blocklisted/blocked state and native
disable reasons reported separately. A missing snapshot does not roll back a
successful mutation, and cancellation does not promise rollback.

Loads use Chromium's real unpacked installer, including manifest, policy,
requirements, locale and DNR indexing checks, with file and incognito access
explicitly false. Installation/update grants manifest permissions and therefore
requires the embedder's prior install/update consent. It must never be used as a
shortcut around enable approval. Mutations are limited to ordinary unpacked or
command-line entries; components, shared modules and primary kiosk entries are
refused. The trusted Jupiter adapter must additionally protect its essential
bridge IDs, validate owned inventory paths, and supply its native install/remove
confirmation. These APIs are not a renderer or DevTools transport.

Enable accepts only unchanged permissions and the sole native USER_ACTION
disable reason. Unsupported requirements, escalation, policy, supervised
approval, terminated entries and other disable reasons are refused, rather than
silently granting permissions or repairing them. Disable goes through the native
registrar and refuses transitional enabled state containing other reasons.
Uninstall honors native user-removal policy and completes on the native cleanup
closure. UI-thread completion wrappers cover dropped initialization/native
closures; registry shutdown completes pending operations once and invalidates
their profile references. Source-file cleanup outside Chromium-owned install
directories remains the adapter's responsibility after successful removal.

Exact-pin zero-fuzz forward/reverse checks restore every source byte. Pure pinned
CEF generators checked ten C API/wrapper files for both the request context and
its new callback; all pre-existing stable request-context layouts are unchanged.
The patch adds an explicit dependency on the exact existing
`//extensions/common` target and uses the existing `//extensions/browser` target.
Full translator/API hashes, GN execution, C++ compilation, native CRUD/lifetime
runtime checks and GNOME adapter wiring still require a matching rebuilt SDK.
This patch is not part of the current prebuilt library.
