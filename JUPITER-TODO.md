# Jupiter progress

Updated: 2026-10-16. This checklist tracks the CEF port on `main` and the
features requested for Jupiter. Checked items describe verified behavior,
not complete coverage of the surrounding feature. Development checkpoints live
on `main`; nothing here implies a published release.

Status: `[ ]` planned, `[o]` in progress, `[x]` verified complete.

Build preference: use all available CPU cores (currently 24 logical CPUs),
including `nix build --cores 0` and native builds with `-j$(nproc)`.
Keep Chromium memory core dumps disabled.

## Current work

- [x] Fix About/app metadata: Jupiter 0.1.0, actual CEF/Chromium diagnostics,
  upstream credits preserved.
- [x] Rebuild the Nix package and inspect the corrected About dialog; verified
  Jupiter branding and retained GNOME icon with development library paths unset.
- [x] Replace the null main-resource response stub with actual Chromium metadata;
  verify redirects, error responses, Unicode filenames, subframes, history
  restoration, and resource lifetime. Remove the old WebKit response-policy code.
- [x] Rebuild and check the Nix package with the response migration included:
  version output, About dialog, concurrent downloads, and normal shutdown pass.
- [x] Replace WebKitSettings/WebKitFeature with native Chromium preferences:
  fonts, languages, HTTP/JavaScript identity, spellcheck/caret preferences,
  storage policy, and restart feature switches. Verify live changes in regular
  and private tabs and background downloads; rebuild and check the Nix app.
- [x] Fix preferences dialog ownership so Reset All and normal shutdown do not
  leave a dangling dialog pointer. Eight affected checks, real full uBlock,
  MV2/MV3 fixtures, and desktop clipboard/WebGPU pass after the migration.
- [x] Cancel file choosers when their page is hidden, breaking the parent-window
  lifetime cycle on close. Real GTK portal checks verify save-path selection,
  single/multiple uploads, native cancellation, hiding the page, and closing the
  window; D-Bus confirms six requests and both pending-request Close calls.
- [x] Rebuild the Nix package with the chooser lifetime fix; verify the real
  packaged browser reads a portal-granted file with sandboxed renderers, closes
  a pending portal on quit, and still passes the preferences UI check.
- [x] Enforce each session's storage policy on CEF's intercepted cookie path.
  Verify HTTP and JavaScript cookie blocking, rejected Set-Cookie responses,
  regular/private downloads, restored cookie access, persistent cookies, and
  explicit private-session overrides after live preference changes.
- [x] Rebuild and verify the package after the intercepted-cookie fix: actual
  sandboxed portal uploads, pending-chooser shutdown, preferences Reset All,
  concurrent downloads, full uBlock, MV2/MV3, and desktop clipboard/WebGPU pass.
- [ ] Finish migrating site-data management to Chromium's actual stores; keep
  enumeration and clearing accurate for persistent and private contexts.
- [x] Route HTTP cache clearing from the existing Website Data dialog to CEF.
  Verify actual eviction, cancellation, regular/private isolation, preservation
  of cookies, checkbox persistence, filtering, and normal/incognito dialog use.
  Wait for operations before showing success; keep hidden selections out of
  clearing. Nine native checks pass. Packaged normal/incognito clearing passes.
  One earlier normal-mode quit timed out; subsequent packaged and development
  runs quit normally. Preserve screenshots/window details on future failures.
- [x] Move Website Data's cookie list and deletion to the native CEF session.
  Verify per-domain clearing and all-cookie clearing, HttpOnly and CHIPS cookies,
  cancellation, invalid domains, other-domain preservation, and private-session
  isolation. Actual regular/incognito dialogs preserve localStorage and quit
  normally. Background legacy queries no longer reset native row selections;
  asynchronous fetch callbacks hold weak references to the dialog.
  Keep the GTK filter predicate free of row-expansion side effects and make
  category visibility independent of child insertion during asynchronous refresh.
  Rebuilt the Nix app; regular/incognito packaged workflows and normal quit pass
  without development library paths. Nine native checks and real full uBlock
  pass. GUI checks explicitly focus the search entry before typing or editing.
- [ ] Use a service-level site-data API for private contexts. A Chromium
  Settings helper successfully enumerated real regular-profile storage, but
  the pinned SDK crashes in its sign-in localization for off-record profiles.
  Keep that probe out of the app and do not fall back to the regular profile.
- [x] Verify Chromium quota enumeration without the Settings helper: real
  IndexedDB buckets appear in regular/private contexts with their own storage
  keys and usage; private enumeration excludes regular-profile buckets. This
  remains an isolated diagnostic probe, not a production site-data adapter.
- [x] Evaluate native scripts in actual Chromium named isolated worlds. Verify
  shared DOM, isolated globals, cross-origin boundaries, private/tab isolation,
  source URL escaping, cancellation from another thread, real navigation,
  hiding, moving, and normal window closure. Pin evaluations to unique contexts
  and actual document epochs, so starting a download does not cancel them.
  Ten native checks pass, including downloads with and without an evaluation
  callback; reachable pending-promise fixtures also pass three repeated runs.
  Real uBlock and MV2/MV3 permission/isolation fixtures pass.
- [x] Stop exposing internal alternate-page data URLs in the address bar,
  history, resource address, and saved session. Retain opaque page origins;
  load actual destinations when restoring/selecting placeholders or reloading.
  Native regression and repeated real-browser restart/deferred-tab workflows
  pass, including the Nix package without development library paths.
- [ ] Resolve intermittent packaged shutdown assertion (`!ephy_shell`): one
  restored-session quit failed; subsequent repeated restarts, preferences, and
  cookie clearing quit normally. Reference tracing found obsolete WebKit filter
  downloads holding the app. Remove that pipeline and recheck the package;
  do not claim the assertion's root cause is proven from successful repeats.
- [x] Remove the unused WebKit JSON-filter manager, startup downloads, and
  filter-initialization navigation queue. Full uBlock still passes its real
  filtering checks. The rebuilt package passes repeated restore/deferred-tab
  workflows and normal quit, including a fixture that detects any legacy filter
  download. This removes a WebKit service; other WebKit dependencies remain.
  Actual packaged preferences and normal/private cookie clearing also pass.
  Cookie UI automation retries the clear action during the documented disabled
  period while CEF flushes the store; screenshots captured this timing failure.
- [x] Replace WebKit permission-request objects with actual CEF prompt/media
  callbacks and reuse Epiphany's dialogs. Verify allow/deny, Ask Later, canonical
  requesting/top-level origins, opaque-origin denial, private scope, single-use
  decisions, navigation, iframe removal/navigation, tab closure, and late replies.
  Real Chromium media tests use diagnostic fake devices; physical webcam/mic
  behavior is not claimed. Eleven native checks pass. The actual app's dialogs
  allow/deny/dismiss correctly; clipboard denial leaves camera/microphone records
  unchanged, and private requests leave the regular permission file unchanged.
  Remove the unused WebKit permission-state query signal and handler as well.
  The Nix package passes actual permission dialogs, repeated session restoration,
  preferences, and normal quit without development library paths. Full uBlock,
  MV2/MV3, desktop clipboard/WebGPU, and six portal requests pass. The extension
  probe now waits for all result markers instead of closing before the delayed
  blocking check; the three-launch permission workflow has an explicit deadline.
- [o] Investigate intermittent CEF integration timeouts. A parallel suite timed out
  in script/permission lifetimes and downloads; isolated checks and the same full
  suite run sequentially pass all eleven checks. Keep the native browser tests
  sequential for normal verification and preserve operation-specific diagnostics.
  A later serialized script check also timed out during a concurrent Nix build;
  three immediate isolated repeats passed. Script waits now identify the caller
  and pending expression instead of only reporting a generic assertion.
  A second suite during compilation also timed out in permission and session
  checks. After compilation stopped, all twelve checks and the full runtime/UI
  gates passed. New navigation regression coverage below fixes a separate
  lifecycle error; do not claim it proves the under-load timeout's root cause.
  The rich-clipboard regression suite also hit clipboard startup, session and
  download timeouts after compilation had finished (10/13 passed). Sanitizer
  documents are now created only on the first rich markup read, rather than
  every renderer context. The subsequent thirteen-check suite, actual Wayland,
  AMD WebGPU, full uBlock/MV2/MV3 and file portals pass; this does not establish
  the intermittent startup timeout's full cause.
  The underlying timing issue is not proven fixed. Builds still use all cores.
  The final uBlock gate initially failed dashboard initialization or actual
  blocking after filters reported ready. Source review and runtime traces found
  a fixture race: fresh uBlock reloads itself, disconnecting messages during the
  fixed wait; disconnected messages resolve undefined. The probe now waits for
  a real getLists response (after native network observers start), reads back the
  exact enabled filters, and avoids reloading an initialized dashboard. All four
  genuine uBlock checks and a fresh-profile repeat pass. The rebuilt Nix app
  passes regular/private icons, real clipboard permission dialogs and Find/Reader
  workflows without development library paths. The separate CEF startup timing
  issue above is still open; this fixes the observed uBlock fixture races.

## Verified foundation

- [x] Use Epiphany's actual GTK/libadwaita shell with CEF rendering pages.
- [x] Package CEF libraries so the built Nix app starts without development paths.
- [x] Keep ordinary browsing sandboxed; verify a packaged renderer has
  `Seccomp=2` and `NoNewPrivs=1`.
- [x] Verify native copy, web clipboard writes, and GTK paste into Chromium inputs.
- [x] Verify WebGPU on the desktop AMD GPU with an actual compute operation.
- [x] Verify hardware WebGPU separately on the actual Wayland backend. The
  `--gpu-only` capability test computes [2,4,6,8] on the actual AMD RDNA 2 adapter
  and cleanly shuts down Chromium. The Vulkan presentation warning does not
  prevent this compute operation; clipboard focus checks remain unchanged.
- [x] Run full uBlock Origin MV2: network filtering, cosmetic filtering,
  scriptlets, and iframe filtering.
- [x] Pass MV2/MV3 extension fixtures, including host permission checks.
- [x] Verify native MV3 USER-origin CSS overrides inline author `!important` and
  native removal restores the author style. Real MV3 runtime, blocking and host
  isolation gates pass. Log: `build-cef/verification/rich-clipboard/png-final/mv3-user-origin.log`.
- [x] Use native CEF request contexts for cookies and isolated private sessions;
  verify cookie persistence and private-session cleanup.
- [x] Route downloads through CEF into Epiphany's existing download UI;
  verify redirects, concurrent filenames, cancellation, private cookies,
  tab closure, and normal application shutdown.
- [x] Verify file and multiple-file selection through real GTK portals into
  actual Chromium file inputs.
- [x] Verify a UTF-8 save-dialog path, user cancellation, and pending file-chooser
  cancellation when hiding a page or closing its window through real portals.
- [x] Prevent large Chromium memory core dumps while preserving process
  identification needed by desktop portals.

## Finish the engine swap

- [ ] Fix folder uploads: Chromium currently crashes in its upload confirmation
  dialog with the GTK/offscreen embedding. Avoid repeating the known crash
  without a relevant fix.
- [ ] Fix File System Access directory selection separately from folder uploads.
  A real `showDirectoryPicker()` gesture reaches `Browser::OnFileDialog` as
  `FILE_DIALOG_OPEN` (0), confirmed under GDB; the resulting GTK chooser lists
  files. CEF's `SelectFileToFileChooserParams` omits the non-upload folder modes.
  Correct this in CEF rather than guessing from dialog titles or replacing
  the web API with a JavaScript mock. The source patch in
  `engine/patches/cef-directory-picker.patch` dry-runs against pinned commit
  `c1a097e`; it is not applied to the prebuilt SDK, so the runtime bug remains.
- [x] Verify actual saved-document contents through Chromium's writable file
  handles and the GTK SaveFile portal. A real user gesture creates and closes
  a writable handle; Python independently verifies all nine binary bytes and the
  Unicode filename. All six supported chooser cases and their D-Bus requests
  pass, including upload, cancellation, hiding and closing the pending window.
  Log: `build-cef/verification/rich-clipboard/png-final/portal-save-bytes.log`.
- [ ] Finish portal coverage for printing, OpenURI and screen sharing/PipeWire.
  Folder uploads remain tracked separately above.
- [x] Make the migrated browser preferences control Chromium's actual settings
  and behavior; replace the feature panel's WebKit catalog with Chromium switches.
- [ ] Make clearing site data and cache operate on Chromium's stores, including
  private contexts and asynchronous error reporting. The pinned SDK needs a native
  storage-enumeration adapter: quota buckets omit local-storage-only sites and
  cannot identify every category/partition by themselves. Chromium Settings can
  enumerate regular data but crashes in isolated private profiles because its
  signin client is absent. Quota Internals works with a hidden Chrome-runtime
  view in both contexts; a windowless Alloy probe returns ERR_ABORTED. Do not
  infer the site list from history, inspect live database files, or mark private
  clearing complete using the regular context.
- [ ] Replace remaining WebKit storage/ITP services; ensure privacy reporting
  describes protection actually provided by this browser.
- [ ] Finish native permissions, authentication, TLS errors/exceptions, and
  navigation/dialog lifecycle behavior.
  User desktop checks: clipboard read/write works; `showDirectoryPicker()` opens
  a file chooser, WebAuthn still shows Chromium UI, and Bluetooth/location requests
  appear to do nothing. Trace these individually; camera remains unverified
  because the user has no webcam.
- [o] Connect CEF certificate failures to Epiphany's existing TLS error page and
  scoped exception flow (delegated in an isolated worktree; main stays on main).
  Map actual certificate errors separately from informational status, retain the
  issuer chain and verify host/certificate/session isolation and navigation cleanup.
- [x] Synchronize notifications, location, microphone, and camera choices with
  Chromium content settings. Import existing policy-file choices once before
  navigation; persist a migration marker alongside native content settings.
  Native checks cover JavaScript query results, allow/deny, repeated requests,
  revocation, malformed origins, and separate private contexts. The actual GTK
  app imports a notification denial before its first query, changes Allow/Deny/Ask
  through GNOME's website controls, preserves those choices across restart, and
  does not resurrect revoked legacy records. Private choices disappear on close.
  Clipboard denial prompts again on the next request and does not alter media
  settings. The Nix package passes the same permission workflow, repeated session
  restoration, preferences, and normal quit without development library paths.
  All eleven native checks, actual uBlock/MV2/MV3, desktop clipboard/WebGPU,
  and the six supported file-portal requests pass.
  Existing password/ad/autoplay policies keep their original backend.
- [ ] Finish remaining permission-service integration and persistence rules,
  including storage-access choices and the legacy page-script permission export.
- [x] Return native JSON results from Chromium evaluation instead of creating a
  JavaScriptCore context per result. Port all evaluation callers, including Find,
  reader metadata, OpenSearch arrays, web-app metadata, and legacy extension
  replies. Native checks verify nested data, Unicode, distinct null/undefined,
  BigInt/non-finite numbers, script/serialization errors, isolated worlds, and
  cancellation/navigation/closure. All eleven native regressions pass.
- [x] Fix Reader Mode's pending-navigation deadlock: extract the current DOM
  before navigating, consume prepared HTML once for its matching reader URI,
  and register scheme requests before a synchronous completion can free them.
  Real GTK checks verify Find selection through native clipboard, live DOM edits
  in Reader content, direct reader URLs, normal/private cookie contexts, and quit
  during a stalled source request. Cold reader URLs release their hidden source
  view after parsing. Remove the reader source view's unused WebKit context.
  The Reader keyboard shortcut and menu both pass. Actual uBlock/MV2/MV3 and
  desktop copy/write/paste plus AMD WebGPU compute pass after the result migration.
  The desktop probe explicitly presents its window and waits for active focus
  before exercising the unchanged clipboard security gate. All six supported
  file-portal requests pass with the ported result consumers. The final Nix
  package passes normal/private Find/Reader workflows, pending-reader quit,
  repeated session restoration, preferences, and permission persistence without
  development library paths.
- [x] Replace internal top-level page messaging with actual CEF renderer hooks
  and process messages carrying JSON, without a WebKit JavaScript API shim.
  Port Reload, certificate-action callbacks, and web-app controls; remove JSC
  use from EphyWebView. Restrict delivery to registered handlers on this view's
  own internal documents; reject websites, subframes, forged alternate-page
  markers, oversized messages, and wrong worlds. Native checks cover parser-time
  delivery, JSON/Unicode, live registration changes, history, private contexts,
  normal close, and a callback closing its own window. Chromium network errors now reach the actual Epiphany error
  template. Its real Reload action recovers a failed HTTP request; the actual
  web-app Delete control opens GNOME's confirmation, and cancellation preserves
  the disposable app fixture. Certificate-error service handling remains below.
- [x] Infer internal resource MIME types when a scheme callback leaves them
  unspecified. Real Chromium checks render an alternate-page SVG and apply an
  internal CSS stylesheet; the application error icon and page styles now render.
  Replace the WebKit-only system-font name in imported templates with system-ui.
- [x] Cancel outstanding native evaluations directly from Chromium document and
  context destruction, independently of a late DevTools method reply. Match a
  context's unique identity before removing a reused numeric ID. Retain script
  promises in hidden tabs and during downloads; navigation and closure checks
  exercise the affected lifetime rules. Keep the intermittent timeout above
  open rather than attributing it to this lifetime gap without a specific trace.
- [x] Port local web-app icon loading from blocked file URLs to per-view native
  resource URLs. Actual regular/private application pages render exact fixture
  pixels; malformed, oversized, missing and nonregular icons use GNOME's fallback.
  Unknown/path/expired/cross-view icon requests fail and disposable apps survive.
  Both workflows close normally. The empty-list illustration branch remains
  source-reviewed only. Logs: `build-cef/verification/rich-clipboard/png-final/`.
- [x] Separate main-document starts from the aggregate loading indicator.
  Use actual main-navigation callbacks, including cached-history commit fallback;
  fragment changes and subframe loads preserve the main resource, native
  response, pending isolated-world promises, and document-event counts.
  Initialize the root frame from Chromium's real frame tree; refresh same-
  document history outside DevTools event dispatch to avoid reentrancy.
  Actual mouse-gesture fragment navigation and native Back pass, as do cached
  response restoration and the real Reload/Delete UI. Keep Chrome's normal
  suppression of non-user-activated script history entries intact.
  The final Nix package passes all twelve native checks, real full uBlock,
  MV2/MV3, desktop copy/write/paste and AMD WebGPU compute, and six actual file
  portal requests. Packaged regular/private Reload and web-app cancellation,
  Find/Reader (including stalled-source quit), repeated deferred-tab restoration,
  preferences Reset All, and the eight-launch permission workflow pass with
  development library paths unset. No compilation ran alongside this final suite;
  under-load stability remains unverified after the lifecycle changes.
- [ ] Port declarative user scripts and private-world page messages plus their
  remaining JavaScriptCore values. Preserve top/all-frame selection, timing,
  globals, CSP behavior, URL scopes, and isolated worlds. A native runtime probe
  showed root-only DevTools scripts miss cross-process frames and delayed main-
  world JavaScript eval fails strict CSP; do not use those shortcuts as the port.
  Exact Chromium 150 source research rules out native chrome.userScripts as a
  complete substitute: per-extension consent, restricted internal/data schemes,
  a 100-world limit and shared incognito world configuration leave manager gaps.
  Extend native per-view configuration and the existing DevTools observer with
  named-world bootstraps and paused cross-process iframe session routing instead.
  Native extension USER-origin injection/removal now passes its real runtime
  check; per-manager stylesheets, internal schemes and early timing still need
  their complete native integration.
- [x] Bind clipboard replies to the actual requesting V8 context, including
  isolated worlds and replacement render views after reload. Verify concurrent
  overlapping request IDs, Unicode, rejected formats, custom Copy handlers in
  main/private worlds, Cut deleting once, and native background-focus denial.
- [x] Fix empty Wayland clipboard text reads: retain Blink's original native
  readText permission/policy checks, then read the actual GTK clipboard. Capture
  the original Promise.then before website scripts so they cannot invoke the
  native completion without permission. Real X11 and desktop Wayland checks
  verify exact main/private-world text, GTK-owned and empty clipboards, per-read
  permission prompts, denial without disclosure, and fresh bindings after reload.
  AMD WebGPU still computes [2,4,6,8]. All thirteen native checks, real full
  uBlock/MV2/MV3 and six file portal requests pass. The rebuilt Nix app passes
  the actual GTK Allow/Deny dialogs (exact text and a fresh prompt on each read)
  and regular/private Find/Reader checks with development library paths unset.
  Verification logs and the package path are saved under
  `build-cef/verification/clipboard/`; the rest of the CEF port remains unfinished.
- [x] Port the supported rich navigator.clipboard.read() formats to GTK,
  preserving Chromium permissions, HTML sanitization and binary image data.
  Native X11 and actual Wayland checks verify text, exact encoded PNG metadata
  and pixels, native GTK textures, safe HTML/SVG, Figma binary data, selective
  formats, explicit unsanitized HTML, iframe policy, and cancellation when the
  clipboard owner changes. Embedded image/CSS resources are not fetched.
  PNG bytes pass unchanged to the renderer for decoding; GTK encodes only local
  textures when no PNG bytes are offered. Invalid image data is rejected by the
  renderer decoder. Twelve other native checks, AMD WebGPU, MV2/MV3, exact portal
  saves and full uBlock in two fresh profiles pass. The rebuilt package passes
  regular/private icons, clipboard Allow/Deny and Find/Reader workflows with
  development library paths unset. Logs:
  `build-cef/verification/rich-clipboard/png-final/`. Package:
  `/nix/store/iwsw7dwhs0h4cczvq0fdplsz4qcrhddk-jupiter-0.1.0`.
  The initial broad run hit clipboard startup/reload and uBlock initialization
  failures; the targeted native/desktop runs and corrected uBlock probe pass.
  Keep the independent CEF startup issue above, generic web-custom-format
  interoperability and HTML style-inlining differences open before calling
  clipboard coverage complete.
- [ ] Implement Chromium's web-custom-format clipboard map for interoperability
  beyond Figma's legacy raw MIME. The pinned Chromium 150 backend publishes JSON
  under `application/web;type="custom/formatmap"`, mapping web MIME names to
  indexed `application/web;type="custom/format0"` payloads. Preserve the `web `
  namespace separately from native clipboard formats, bounded reads/writes,
  format filtering, and exact binary bytes in cross-application checks.
- [ ] Port password/autofill integration and content script management.
- [ ] Replace remaining WebKit request/resource model APIs and report subresource
  lifecycle events. Main-response metadata is native now; source reads still use
  Chromium's document source and need original response-body support.
- [ ] Connect extension management, enable/disable, settings, and toolbar actions
  to the native CEF extension runtime; test real extension workflows. Only considered done when both manifest v3 and v2 extensions work.
- [ ] Update the pinned CEF/Chromium SDK from Chromium 150 while preserving
  full MV2/uBlock support and rerunning compatibility checks.
- [ ] Remove WebKitGTK/JavaScriptCore dependencies entirely once their remaining
  callers are migrated.
- [ ] Run a final packaged browser smoke check covering the migrated services;
  port affected upstream tests rather than treating compilation as completion.

## Requested Jupiter features

These are not implemented in the Epiphany-based fork yet. Shell work can start
before every service migration is finished, with the engine work still tracked
above.

- [ ] Add tab groups using native Epiphany/libadwaita tab behavior and styling.
- [ ] Add Ctrl+Tab with a thumbnail grid showing only tab names as text.
- [ ] Offer tab order versus most recently viewed order for Ctrl+Tab, including
  Home where applicable.
- [ ] Check tab insertion animation and overflow against vanilla libadwaita;
  keep single-tab visibility and group navigation consistent with the requested UI.
- [ ] Replace the temporary GNOME icon when the user's Jupiter artwork is ready.
- [ ] Add built-in KeePassXC integration for saving and filling credentials,
  using KeePassXC's browser integration protocol and database authorization.

## Completion rule

Keep this file updated at each meaningful milestone. Do not mark the engine
replacement complete while WebKit/JSC remain required, or call extension/portal
support complete based only on the passing subsets above.
