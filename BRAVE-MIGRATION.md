# Brave backend migration

Objective: take the working Brave PoC, embed it in Epiphany's actual GTK shell,
remove WebKit and the CEF compatibility dependencies, and connect shell services
to the real Brave browser. This replaces the previous CEF engine direction.

User constraints: work solo, prioritize implementation, use `nix run` to build and
launch, keep the Chromium sandbox enabled and core dumps disabled, no interface-
polish skills. The original X11 PoC can be closed; preserve useful test windows
while replacing their backend. Brave supplies the web-content backend; Gnosis
owns all browser UI and user-facing browser behavior.

Launch preference: reuse the normal ~/.local/share/gnosis profile for user test
builds. Earlier per-build /tmp profiles made history appear to disappear. Their
databases remain intact; the default history had 43 saved URLs when checked.
Use a fresh isolated profile only for a specific isolation test, not each build.

## Native find, close protection and runtime updates — 2026-10-26

Native Ctrl+F now calls the pinned FindTabHelper and consumes merged renderer
results through a private inherited channel. It retains actual matching and
highlights across same-process and cross-process frames. The GTK toolbar uses
actual count/ordinal results and hides unsupported whole-word matching. Tests
cover wrapping, case, Unicode, hidden text, query replacement/stop, independent
and closed targets, bounded requests and the actual GTK menu/counter lifecycle.
Closing targets can remain resolvable without ever replying; those requests are
retired rather than retaining stale helper identities indefinitely.

Native beforeunload preflight dispatches real frame handlers, then suppresses
only the subsequent unload for its own armed check. Tabs/windows remain intact
until the complete GTK close transaction is approved. Cancel, navigation and
trusted document activity invalidate approvals; a final validation precedes
window/application destruction. Real GTK last-tab Keep Open, two-tab window and
application-wide two-window Cancel/Leave, form heuristics and actual DevTools
activation pass. A missing
reply defaults to Keep Open and offers explicit Close Anyway; actual GTK fault
injection verifies both choices. Cross-process-frame interaction invalidation
and broader live-editor coverage remain follow-up work.

Default browsing no longer passes --disable-frame-rate-limit. Composited Brave
uses WaylandExternalBeginFrameSource; explicit GNOSIS_UNCAP_FRAMES=1 remains a
diagnostic override. Steady-state packaged checks measured 59.79 fps at 60 Hz
and 119.55 fps at 120 Hz, versus 1698.37 fps in the earlier uncapped 60 Hz sample.
Startup/fullscreen monitor transitions affected one earlier sample; mixed
monitors and the separate GPU SIGTRAP are not declared fixed.

Nix uses cef_runtime=linked, preserving SDK file modes, sandbox helper, ICU,
locales and actual ELF loading while avoiding repeated CEF copying/stripping.
Portable installations retain copy mode. WebKitNetworkSession is lazy; normal
and incognito GTK checks verify identity, ITP changes and disabled credential
storage. CEF remains required by real CefValue/JSON and other compatibility APIs,
and WebKit/JSC still serve remaining consumers. This is not full engine removal.

Bundled full uBlock refresh uses the stable profile-owned directory, validates
ownership/identity, atomically swaps a complete tree and keeps a rollback until
native reload/version/enabled-state verification. Existing storage is retained;
newer installed versions are never downgraded automatically. External providers
are respected. Filesystem recovery tests, actual same-path native reload/storage
checks, full MV2/network/main-and-frame cosmetic/scriptlet checks and actual GTK
GSettings disable/re-enable filtering pass.

Private profiles now have explicit owned leases spanning intentional restart,
browser exit and background extension filesystem work. Last private tab and
application shutdown stop writers before unlinking owned data. Cleanup does not
follow symlinks or remove a replaced root. Actual incognito cleanup passed; an
abrupt host crash can still leave a temporary directory.

Media context menus operate on the actual trusted captured video/audio element
and reject stale documents or detached elements. Real Brave audio and JS checks
pass. PiP is not advertised while it would open an unhosted Chromium window.
Cross-process iframe menus, native downloads and Inspect Element are still open.
PIN/account/GPM/conditional-passkey and tab-sharing GTK adapters also remain open;
the source/ABI audit did not enable unverified native model offsets.

See engine/tests/NATIVE-GTK-TESTS.md for repeatable probe commands. Earlier dated
investigation notes below are historical where this milestone supersedes them.

## Custom tabs archived on 2026-10-25

The Select custom tab strip, overview and group implementation is preserved on
`archive/select-custom-tabs`. The active shell uses its previous AdwTabBar and
AdwTabOverview. Independent restored-title, native favicon and active-tab signal
cleanup fixes remain in the active tree, along with the earlier settings work.

## Settings bridge verified on 2026-10-25

Font preferences use the privileged fontSettings API; popup/storage policy uses
contentSettings. Custom CSS joins the native document stylesheet, and custom JS
uses removable Page scripts gated to HTTP(S)/file pages. Reader font changes and
per-site/default zoom stay in the existing shell policy. Disabling JS removes
future-document scripts; already executed script effects require a page reload.

Cookies use Storage/Network CDP methods within the matching normal/private
process. Site-storage clearing uses browsingData with extension storage excluded.
Website Data shows supported global storage categories and actual cookie domains;
WebKit HSTS/ITP/device-ID categories are hidden in Brave mode.

The advertisement switch controls actual full uBlock Origin through management.
If absent, enabling it loads the official packaged build from a stable profile-local
path. Chromium developer mode is enabled for the bundled/validated unpacked
extensions, avoiding DISABLE_UNSUPPORTED_DEVELOPER_EXTENSION on uBlock self-reload.
Brave's independent global ad/cosmetic filters are handed off through its pinned
settings WebUI handlers, in an unembedded transient target. Other Shields settings
are retained. The bundled copy's automatic version refresh remains follow-up work.

Target.openDevTools supplies real DevTools in a transient GTK compositor view.
Inspector windows do not join GtkApplication's browser-window list, and close with
the inspected tab. F12, Ctrl+Shift+I and the main-menu action share this path.

Native GTK checks covered custom homepage, fonts/CSS/JS enable/disable, reader
font changes, default zoom, DevTools, cookie path/domain deletion, private cookie
isolation, Website Data categories and Ctrl+Tab contrast. The online-store submenu
and actual new-tab activation passed; the Chrome store may redirect to Google consent.
Full official uBlock passed network, main/frame cosmetic, scriptlet and toggling
checks. The store submenu opens Chrome Web Store and Microsoft Edge Add-ons;
Edge-catalog installation has not been independently verified.

Ordinary browsing keeps Blink's native webdriver getter false through
AutomationControlled's runtime feature setting; explicit automation mode retains
the automation marker. The private CDP pipe and Chromium sandbox remain enabled.
The user confirmed Google sign-in now proceeds. The exact-binary overlay suppresses the shared infobar presentation through
its preferred-size calculation. Underlying flags, native delegates and sandbox
settings are unchanged. The CAPTCHA screenshot reports unusual network traffic and does not
establish the cause of an individual challenge.

## Current work

- [o] Replace the native-child X11 rendering boundary with an embedding Wayland
  compositor. The standalone GTK widget in engine/brave/compositor.c has visibly
  rendered Brave GPU buffers via DMA-BUF; the user confirmed the run worked.
  Route a shared compositor's surfaces to individual Epiphany tabs next. Keep
  GTK responsible for overlays, corner clipping, and outer window focus.
- [o] Extract the proven native X11/XWayland embedding into reusable page/process
  classes; share one Brave process/profile across normal tabs and isolate private
  sessions. Private inherited CDP pipes only; no public debugging ports.
- [o] Replace Epiphany page views/navigation with that backend. Keep the actual
  header, tab bar, overview, bookmarks/preferences widgets and their native style.
- [o] Fix native keyboard focus ownership; inspect pointer lock intermittency
  separately from NZP game behavior. Native child clicks must hand focus to Brave;
  GTK header/shortcuts must regain it. Don't emulate held mouse buttons.
- [o] Replace Chromium's right-click presentation with actual GTK menu actions.
- [o] Internal pages/resources and origin-bound messages: preserve existing GTK
  shell services without exposing privileged bridges to arbitrary websites.
- [ ] History, title, favicon, progress, navigation entries, restore and thumbnails.
- [o] Find, editing/clipboard, zoom, source/reader, saving and print. GTK editing
  actions must reach Brave's native Wayland seat rather than the old CEF path.
- [o] Follow the host monitor's refresh rate and verify actual frame pacing.
- [x] Repair webpage paste through native GTK editing actions and Wayland clipboard.
- [x] Stop history/view-transition commits from restarting an already finished load.
- [ ] Native extension inventory/actions/settings and real Store install; actual
  full uBlock Origin plus MV2/MV3. Keep all extension actions in the menu.
- [o] Downloads, cookies/site-data, permissions, notifications, portals, auth/TLS,
  client certificates, WebAuthn and real file/directory chooser integration.
- [ ] Select's complete tab/group/overview behavior, beyond the Ctrl+Tab grid.
- [ ] Remove all remaining WebKit/JSC and CEF objects, imports and build inputs.
- [ ] Actual packaged GTK acceptance, including normal/private/multiple windows,
  iframe behavior, close lifetime, keyboard and real games' pointer lock.

The standalone PoC in ../gnosis-brave-poc is already visibly verified: GTK native
surface -> native child container -> actual Brave app window. Its normal-window
coordinates add GTK's CSD shadow offset; native window extents are cleared and
Mutter withdrawal completes before reparenting. F12 opens GTK Inspector and F9
shows the native content boundary. This is external-window X11 embedding, not a
source fork of Brave or native Wayland integration; don't claim otherwise.

Integration evidence (2026-10-24): the first extraction incorrectly matched a
Mutter decoration frame by its copied page title. Restoring the PoC's exact
Brave WM_CLASS and normal-client-window checks removed the ghost frames and
clipped viewport. The corrected live run has only the GTK Gnosis client in
_NET_CLIENT_LIST; the real Brave client is an override-redirect child at (0,0)
inside its GTK content container. The visible page fills that content area.
Closed tabs now retain at most one blank, unmapped reusable app window to avoid
Chromium exiting between last-tab close and next-tab startup. Process pipe loss
removes native containers and presents a GTK retry action instead of stale pixels.
The repeated close/reopen sequence and process-failure retry still need dedicated
runtime verification. Neither this evidence nor the working website proves the
remaining shell services or WebKit/CEF removal complete.

Compositor prototype: `nix run .#compositor-probe -- URL`. It lives in this
repository and uses wlroots 0.19, a private Wayland socket, ordinary GTK snapshot
rendering, and GdkDmabufTextureBuilder. The successful run logged actual Brave
GPU-buffer import (not screenshot streaming) and the user confirmed it worked.
The probe has a GTK overlay sidebar and forwards basic keyboard/pointer/scroll
input through a Wayland seat. SHM client buffers are explicitly copied as a
fallback. It is a standalone boundary prototype, not yet the Epiphany backend.
Remaining requirements include shared-server tab routing, complete buffer/
renderer shutdown, GPU-format negotiation, popups/geometry/transforms, keyboard
layout/IME, clipboard, presentation synchronization and
frame pacing. Do not claim those passed from a single successful visible run.

Pointer capture: the compositor now bridges inner locked-pointer requests to
the outer GTK Wayland surface's actual pointer constraint and forwards native
relative-motion events. It hides/restores the GTK cursor on activation/release,
releases on Escape/focus loss/unmap/client destruction, and only acknowledges
the inner lock after the desktop activates the outer lock. The capture-enabled
probe logged successful outer lock activation, and the user confirmed it works.
Persistent constraints are inhibited after Escape/focus loss until another
input gesture. That last guard has not yet been rerun. Capture currently requires
the GTK Wayland backend; X11 capture and inner confined-pointer requests are not
implemented. This does not establish full game/IME/clipboard compatibility.

Epiphany integration now defaults to compositor-backed Wayland page widgets.
Each normal/private Brave process owns a shared GnosisCompositor GObject;
registered widgets bind their bootstrap tokens to the corresponding xdg
toplevel. The actual shell has run with two independently registered GPU-backed
tabs on a single Brave process and compositor. A hidden reusable widget retains
the last app window across tab closure. Renderer ownership is held by imported
buffer leases until GTK releases those buffers. The X11 path remains a temporary
fallback via GNOSIS_SURFACE=x11, not the intended final architecture.

The current live follow-up build includes held-key cleanup on focus loss and
GTK modifier synchronization before pointer/scroll events; resize-time scaling
with matching hit coordinates and duplicate-configure suppression; common
new-window URL routing through Epiphany's popup/tab creation; and a real TLS
certificate/security-state adapter with a GTK update signal. The earlier stale
Control state also explains Ctrl-scroll zoom and potentially Ctrl-click link
behavior. These fixes have built and launched, but the individual UI behaviors,
cross-origin/no-opener popup routing, certificate failure/mixed-content cases,
and close/reopen lifetime behavior still need verification. URL routing creates
a fresh page and does not yet preserve arbitrary JS opener/document semantics.
New-tab internal scheme handling remains missing and must be implemented.
The package no longer builds the legacy CEF test executables on every default
`nix run`; those remain available through the explicit Meson unit_tests option.

Follow-up native services: clipboard-bridge.c now accepts private-seat selection
requests and streams MIME offers asynchronously to/from GDK's desktop clipboard.
Clipboard reads/writes from the private client require an active, focused GTK
content view. Host-created data sources cancel pending transfers on replacement;
GTK providers stop offering data when their Wayland source disappears. This has
built and launched in the actual shell; cross-application paste and MIME/lifetime
behavior still need verification beyond permission.site's API result.

Wayland fullscreen requests now change the real GTK window, retain whether the
window was already fullscreen, restore owned state on unmap/destruction, and
notify the shell's content-fullscreen signal. The probe also hides its own GTK
header. This was extended after the user's initial fullscreen test showed browser
chrome. Both the fullscreen transitions and header restoration need another check.

Brave's analytics infobar is disabled before backend launch using the exact
v1.97.56 Local State prefs from Brave's own source: brave.p3a.enabled=false and
brave.p3a.notice_acknowledged=true. This only prepares Gnosis's backend profiles,
preserves other Local State keys, and fails on invalid JSON instead of resetting
the profile. The successful native-services run has those values in its actual
Local State file. It does not claim all other Brave UI has been replaced.

Refresh-rate follow-up: the nested output advertised a hardcoded 60000 mHz.
It now initializes from GDK's monitor rate and updates from mapped GTK windows
as they move between monitors. A live packaged run reported 120000 mHz on the
120 Hz monitor and 60000 on the other monitor. Frame callbacks remain paced by
each widget's GTK frame clock; actual webpage rAF/presentation rate still needs
measurement. One shared output currently advertises the fastest mapped monitor
for the process, so per-window mixed-refresh output isolation is still pending.

Presentation follow-up: changing wl_output refresh metadata alone did not remove
the user's observed 60 fps cap. The compositor now exposes wp_presentation v2
and samples feedback when importing a surface into GTK's snapshot. It completes
that feedback with the enclosing GDK frame's completed presentation timestamp
and refresh interval; it never claims hardware clock/direct scanout. Feedback
is discarded on unmap/disposal or when GDK has no presentation timestamp.
Changing output metadata and forwarding GTK presentation feedback did not alone
remove the cap. Local rAF samples stayed around 57–63 fps with 16.7 ms median
intervals. With Chromium's frame limiter disabled, the same page measured
1541–1787 callbacks/s and GTK recorded about 118–120 callbacks/s; the user
confirmed the run feels 120 Hz. Composited mode now defaults to that workaround
(GNOSIS_UNCAP_FRAMES=0 restores the internal limiter). This is not an efficient
final scheduler: most inner frames are unnecessary. Parent-driven begin-frame
production/backpressure and accurate mixed-monitor feedback remain pending.

Rapid-history follow-up: the backend previously started loading on a main-frame
commit but finished only on Page.loadEventFired. Back/forward-cache restores and
aborted navigations can omit DOM load. Loading now follows main-frame
frameStartedLoading/frameStoppedLoading, excluding subframes; same-document URL
and navigation-history updates are handled separately. Packaged startup passed;
rapid-history UI acceptance initially failed. A real trace then showed
frameStoppedLoading followed by frameNavigated, repeatedly leaving loading=true.
The commit handler now leaves loading owned by start/stop events and finishes a
new shell resource immediately when a cached commit arrives after stop. The
user's concrete reproducer, doromiert.neg-zero.com, uses cross-document view
transitions. The corrected packaged run logged repeated stop-before-commit
sequences with loading remaining false afterward; the user confirmed it works.

Paste follow-up: the shell's editing dispatcher returned immediately without a
CEF browser. Brave editing actions now send native Wayland key/modifier events
to the focused content view. Editing accelerators are reserved for the shell to
avoid duplicate forwarding. Clipboard transfer diagnostics record MIME, byte
counts and errors, never contents. Real text/image transfers were logged and the
user confirmed webpage paste is fixed. Wider MIME/lifetime coverage is pending.

Native pages: Brave now intercepts a per-tab synthetic HTTPS origin with Fetch
and dispatches authorized top-frame requests into the original Epiphany scheme
handlers. HTML/CSS/JS and native message resource URLs are translated; canonical
ephy-about addresses remain in shell/history. Native messages require a verified
internal document and its default main-frame execution context. Frame commits
are tied to fulfilled native documents (or their known loader for BFCache), not
just URL metadata. Ordinary website/iframe authorization still needs adversarial
runtime coverage. Reader/view-source/error-page transport is not implemented.

The new-tab race was a late bootstrap commit clearing the pending native
navigation. Bootstrap commits are now ignored for shell/native authorization.
A packaged rapid-tab run logged 35 native documents, 32 complete overview DOM
checks/handshakes and zero denied main documents. The native page used its
original stylesheet and overview.js, not a replacement layout.

Live overview messages initially retained raw ephy-resource thumbnail URLs.
They are now translated as well; the normal-profile run loaded eight cached
JPEG screenshots through the existing authorized thumbnail endpoint. The normal
profile remains ~/.local/share/gnosis. Native full-document snapshot requests
now capture a bounded taller clip matching Epiphany's 650x540 cache envelope,
rather than only a wide viewport. Existing images refresh on real page visits.
The packaged run refreshed permission.site's real cached JPEG to 650x540;
inspection confirmed additional document content rather than stretched pixels.

Downloads: real Browser.downloadWillBegin/downloadProgress events now feed the
existing EphyEngineDownload and GTK manager. Transfer GUIDs are deduplicated,
frame ownership is checked, and normal/private processes stage files in their
own 0700 directories. GTK chooses the destination and publishes the completed
file with existing overwrite protection. A slow localhost transfer saved
1,184,000 bytes to Downloads with an exact SHA-256 match. Wider cancellation,
cross-filesystem publication, private-context and tab-close coverage remains.
Manual URI-download APIs still have a legacy CEF path and must be ported.

The incorrectly positioned flying arrow was Brave's own UI. A packaged internal
MV3 integration (downloads/downloads.ui permissions only, no content scripts,
external messaging or toolbar action) uses the real downloads.setUiOptions API
to suppress it. Its stable public manifest key fixes the extension identity.
The backend waits for that worker's own readiness promise after Runtime.enable
and Runtime.runIfWaitingForDebugger. An isolated packaged run confirmed the
handoff and produced native GTK download objects. The initial failure queried
Chrome bindings too early; the closed-pipe GLib watch also caused a busy loop.
Disconnect now removes that watch, and worker readiness polling is asynchronous.

GTK download-start feedback now computes the real downloads button's bounds in
the content overlay, waits for the reveal allocation, and displays a transparent,
noninteractive falling icon below it. It follows resizing/RTL and respects the
GTK animations preference. User requested icon-only feedback; the OSD background
has been removed. The icon-only package was launched successfully.

Restored-tab viewport fix: configure the nested toplevel when its GTK host maps,
retain the last size while hidden, and mark embedded windows maximized within
their host. Screenshot requests are serialized; full-document captures require
a mapped, allocated viewport and hold resize requests until capture completes.
GTK retains the preceding frame during capture to hide Chromium's temporary
screenshot resize. Background full-document captures currently fail safely
rather than resizing hidden restore tabs. The package launched on the normal
profile and imported 1904x978 buffers; confirmation of the intermittent restore
case was subsequently confirmed fixed by the user.

Native Brave context menus now use a CDP isolated world, a binding restricted
to that world, authenticated execution contexts and trusted contextmenu events.
The page world has no native binding. Payloads are bounded; password selections
are excluded; captured actions expire with their originating execution context.
Menus use GTK GMenu/PopoverMenu and the existing Epiphany shell actions for link
opening, private windows, search and bookmarks. Editing goes through the native
Wayland editing dispatcher. Full browser API/menu coverage is still incomplete:
media controls, save/download actions, source and inspector are not ported;
out-of-process frames and window-level custom-menu listeners need further work.
Real Brave protocol checks passed link/selection/password cases, synthetic event
rejection and document-level custom-menu cancellation. User confirmed native
menus open. Desktop clipboard/action automation was inconclusive, so it is not
claimed as verified. GTK uses equal-width flat icon buttons for page navigation and
its native popup flip/resize and scrolling behavior for constrained menus.

Resize/crash investigation found a reproducible wlroots teardown assertion:
the server left its public new_surface listener attached at display destruction.
All four server-owned global listeners are now removed before client/global
destruction, including partially initialized servers. Patched isolated processes
exit without that assertion. This verifies the teardown fix, not the preceding
intermittent freeze; full-shell resize reliability remains under investigation.

Input follow-up: native popups release held nested-seat buttons before grabbing
GTK input. Cancellation and tab unmapping also release them, and late physical
releases are deduplicated. Web content claims its own drag sequence to keep
ancestor pan gestures out of text selection. Extra back/forward mouse buttons
no longer become nested right-clicks. These changes address concrete sequence
ownership gaps; user confirmation is pending.

Frequent tab-load crashes are not yet diagnosed. Main Brave stdout/stderr are
now separate from secondary app-launch helper output, browser exit status is
observed directly, and Target.targetCrashed events record renderer status/code.
A CEF-only extension-load callback also now checks for a CEF browser before
dereferencing it; it is not run for Brave-owned views.

Regression rollback (latest state): user traced frequent freezes to the restored
viewport work above. Restored the drawing/screenshot implementation directly
from `/nix/store/plygbkrhch152lnp4jnlh2hnkgqn573y-source`, the preceding icon-only
build. The cached-frame drawing path, screenshot queue, snapshot resize holds,
snapshot presentation workaround and forced initial maximization are removed.
Hidden full-document capture again follows the preceding implementation. The
remaining allocation fix configures on GTK/client mapping and retains positive
sizes while hidden. Revalidation is pending; earlier viewport confirmation does
not verify this reduced implementation. Menu/input fixes and listener cleanup
remain. Restored tab launch helpers now wait for Browser.getVersion before
spawning, so they cannot compete with the first process for its profile lock.

JavaScript dialogs remain unported: the protocol's opening event reports the
dialog but does not replace Brave's browser dialog handler. A real browser-side
hook is needed; displaying a second GTK dialog would not satisfy the migration.

Native zoom now uses the bundled worker's real tabs zoom APIs, with explicit
window-to-tab mapping, serialized/coalesced requests, actual-factor readback and
native zoom-change events. No host or tabs permission was added. GTK controls
were confirmed working by the user; isolated browser checks verified 125% page
zoom, reset and sibling-tab isolation. Ctrl+wheel is routed to GTK's zoom actions.

The native zoom popup is still unresolved in stock Brave. The allowlisted
extension behavior does not suppress automatic/mode-change notifications; that
ineffective allowlist flag has been removed. `engine/brave/patches/` now contains
an opt-in `--gnosis-native-ui` source patch in one stable component file, with
pinned hashes and an upgrade checker exposed through Nix. Source application
and exact reversal pass for 1.97.56/155.0.8059.40 and the newer candidate
1.100.31/156.0.8078.48. Drift/context/hash failures are rejected. This is not a
compiled or runtime-verified native fix: current prebuilt Brave ignores the
switch. A custom Brave package or explicit executable/version override can be
supplied once the native build exists. Root has only 44 GB free; the 1.4 TB NVMe
Btrfs partition is unmounted and mounting it requires administrator access.

Checkpoint for the next session: the user explicitly accepts binary-patch/native
shim fragility to avoid a full Chromium build. No binary patch is implemented.
Read-only inspection of the pinned x86-64 Nix Brave ELF found a candidate
`ZoomBubbleCoordinator::Show` entry at virtual address `0x0bab39d0` (file offset
`0x0bab29d0`); its control flow matches the pinned source: active-tab check,
RefreshIfShowing, Hide/observation reset, ZoomBubbleView allocation/constructor,
widget creation and ShowForReason. This is a static inference, not runtime
proof. Validate the exact binary digest and original bytes before experimenting
on a private copy; never patch the Nix store/system Brave in place. Relevant
zoom symbols are not exported, so simple LD_PRELOAD symbol interposition is
insufficient. Preserve actual page zoom and the sandbox.


Zoom binary overlay implemented after the checkpoint: `engine/brave/native-ui.nix`
creates a runtime package from the stock pinned Brave; only its ELF and launcher
scripts are copied, resources remain links. `patch-brave-binary.py` requires the
full input hash, original hook prefix and final output hash from
`patches/binary-native-ui.json`. The one-byte `ret` at the verified Show entry
suppresses native zoom feedback without changing real zoom upstream. A temporary
graphical diagnostic stub logged two Show calls; the final one-byte patch passed
native 125% zoom/DPR, reset, events and sibling isolation checks. No sandbox
relaxation or Chromium compilation. Default flake selects the overlay; permission
and extension native UI are still unmodified. Earlier source-build requirements
above describe the alternative source patch, not this binary path.

User confirmed the binary-overlay build no longer shows the native zoom popup.

JavaScript dialogs are being moved to native GTK via authenticated Page session
`javascriptDialogOpening`/`javascriptDialogClosed` events and real
`handleJavaScriptDialog` replies. The pinned native delegate's
`GetJavaScriptDialogManager` at VA 0x0b540040 (file 0x0b53f040) is a verified
3-byte xor-eax/eax/ret hook: graphical protocol checks report hasBrowserHandler
false for alert, confirm and prompt; actual JavaScript results still resolve.
GTK owns the dialog lifecycle, origin label and prompt input; tab hide/close,
navigation, renderer failure and protocol closure retire the pending dialog.
Full GTK verification is pending. This does not yet fix close-tab beforeunload.
Extension inventory and toggles now use Brave's actual `chrome.management`
API through the fixed-key packaged worker, reached only over its private CDP
session. GTK reconciles installs, removals, updates and enabled state, including
real Web Store installs; it no longer inserts the old bundled uBlock catalog.
Unpacked installations use the existing validated profile-store copy followed
by real Brave loading. Normal-profile management rejects ephemeral sessions;
extensions still execute with Chromium's native host permissions. Extension
readiness is independent of renderer readiness and cannot blank page rendering.
The fixed-key integration worker is excluded from the visible list. Native
uBlock network blocking, main/frame cosmetics, scriptlets and toggles passed.

The existing GTK popup view is not yet a genuine Brave ExtensionViewHost with
complete native action/activeTab semantics. Do not call that port complete.
`Extensions.triggerAction` on the native app-window tab target previously
crashed an isolated browser; do not reintroduce it as an untested shortcut.

Uninstall confirmation now has a version/hash-checked native model bridge.
Only the exact ExtensionUninstallDialog call site is selected, while the
original DialogModel, hidden host and accept/cancel callbacks retain ownership.
GTK uses its localized title, Remove label and optional Report abuse checkbox;
Cancel uses GTK's locale when Chromium delegates the default label to its host.
The host may show asynchronously, so model tracking lasts until its destructor.
Cancellation preserves installation; accepting invokes the real upstream
uninstaller. This is separate from the GTK installation-confirmation bridge.

Automatic full-page translation offers are disabled using the native
`translate.enabled` preference, preserving explicit translation for a future
GTK controller. The requested site-menu Translate Page action is still pending.
Reader URLs now use the existing private resource transport without granting
the native settings-page message bridge. Reader HTML retains Epiphany's
script-blocking CSP.

A reported freeze at 15:05:12 on 2026-10-25 coincided with Brave's GPU process
exiting with status 133/SIGTRAP. No core was generated. GNOME shell errors also
occurred nearby; these observations do not establish the underlying GPU cause.
Separately, external shell tools inherited the native shim and failed loading
the newer GLib against the desktop's older glibc. The browser now clears
LD_PRELOAD after loading the shim, keeping the shim mapped only where needed.
The GPU crash must not be described as conclusively fixed by that change.

JavaScript GTK verification completed: scoped accessibility actions on the new
Gnosis process accepted the native alert, rejected confirm, entered Unicode
prompt text and cancelled a second prompt. The fixture webpage received exactly
false / "GTK response ✓" / null. Navigation beforeunload and tab-close handling
remain separate pending checks/work. The native shell was built/launched with
nix run; zoom suppression is retained in the same exact-input binary overlay.

Extension runtime findings from a separate temporary graphical profile: full
uBlock Origin 1.75.0 loads through Extensions.loadUnpacked (ID
nnhhoihpllkifklelcdmchjjmefldpad at its pinned package path), and
Extensions.getExtensions returns real enabled state. Extensions.triggerAction
rejects a page target ("Action can only be triggered on a tab target"). Explicit
Target.getTargets filter [{type:"tab",exclude:false}] exposes native tab targets;
triggering uBlock on one of those in the current --app setup terminates Brave.
The probe's private pipe closes; crashpad reports a snapshot, but no native stack
or exact cause has been recovered. Do not route production GTK action clicks to
this API until app-window popup hosting is fixed. Main Gnosis stayed running.
Probe script: /tmp/gnosis-brave-extension-native-probe.py; source inputs remain
pinned to 1.97.56 / 155.0.8059.40. No extension UI change shipped in this step.


Extension install confirmation now has a small prebuilt-runtime adapter rather
than a Chromium rebuild. It replaces only native dialog presentation, exports
Brave's actual permission warnings, and routes GTK responses to the original
native install/cancel callbacks. Both callbacks pass isolated beginInstall-only
probes; user confirmed the GTK dialog displays after fixing routing to require
an actually mapped widget (restored hidden tabs can retain mapped state flags).
The private inherited decision channel is separate from the CDP pipe. Closing,
hiding or navigating its GTK owner cancels the confirmation; native destruction
closes GTK. The 500 ms acceptance delay and Cancel default are retained.
Enterprise justification/withholding checkbox variants still fall back to native
UI. Site permission prompts, extension action popups and full extension catalog
management remain separate work.


GTK printing now branches to the live Brave Page instead of requiring a CEF
browser. GtkPrintDialog still owns setup, printer selection and spooling; native
Page.printToPDF renders the selected paper/orientation/margins into a private
0600 temporary PDF. Cancellation and document-generation checks reject output
from a hidden/closed or explicitly navigated tab. The earlier CEF print branch
remains during migration. A windowed native --app probe verified real A4 portrait
and landscape dimensions, readable fixture text and print-media CSS exclusion.
User confirmed Ctrl+P opens the packaged GTK print dialog. Saving through that
dialog and physical-printer validation remain pending.

Native find-in-page investigation: pinned Chromium 155.0.8059.40 source places
FindTabHelper in components/find_in_page, not chrome/browser/ui/find_bar. It owns
real asynchronous frame results and the native find session. CDP resource/DOM
search and window.find are not equivalent adapters. No native search UI change
has shipped. Native DevToolsAgentHost::GetForId is a candidate target ownership
bridge (static disassembly points to RVA 0x04bcec90, forwarding 0x04bceca0;
refcount at +8 and native deleting destructor vtable +0xe8). These addresses are
research evidence only, not validated runtime hooks. Existing binutils were used;
no REA tools were connected and no REA configuration was changed.


Permission dialog update: the native ShowWidget entry exports the real accessible
origin/permission title. GTK replies invoke the actual native virtual permission
button callback, including one-time grants; closing/cancelling invokes native
Dismiss. A destructor hook retires requests on navigation. Isolated permission.site
Block and Allow returned denied/granted; a temporary Notification fired show.
A full GTK shell probe clicking permission.site's actual Notifications button
reported success/granted and navigator.storage.persist() returned true. The user
confirmed the prompt works, while delivery remains a separate user check.
USB/Serial/HID chooser and initial screen-source picker remain Chromium UI.
PipeWire already invokes XDG ScreenCast CreateSession/SelectSources/Start, so
portal usage alone is insufficient to claim the source picker is native GTK.

The reported browser closure was identified as Widevine's explicit relaunch:
WidevinePermissionRequest::PermissionDecided posts chrome::AttemptRelaunch after
its restart permission is allowed on Linux. An exact-binary hook now delegates
that request to Process::restart. GTK keeps its Page objects/callbacks and tabs;
only the Brave process, its private pipes and compositor surfaces are replaced
after confirmed launcher exit. Private restarts reuse the same private profile.
A probe-only call through the canonical native relaunch callback was intercepted;
a two-tab GTK probe preserved its process/window and reconnected both Page tokens
with fresh targets after the engine restarted. No full Chromium rebuild is used.
Actual module-download and private-context restart validation remain pending.


Further corrections: the file/directory picker failure is primarily missing
APIs, not evidence of an offscreen portal dialog. BraveContentRendererClient
explicitly disables FileSystemAccessLocal while FileSystemAccessAPI is disabled.
The pinned base feature is off by default. Enabling it exposes the native methods
and a real request reaches XDG FileChooser.OpenFile. BraveWebBluetoothAPI is
similarly default-disabled; the launch configuration enables both while retaining
the Nix package's video acceleration features. Packaged picker validation remains.

The notification transport now rewrites only this browser's freedesktop Notify
application identity from Brave to Gnosis, including desktop-entry and the
packaged icon. The original connection, IDs, website content and action callbacks
are preserved. A narrowly filtered live D-Bus capture and notification show event
verified the new identity. Context-menu actions now copy their invocation before
closing/unparenting the popover can destroy its action group and captured closure;
this fixes a use-after-free path affecting Copy and other menu actions.


Copy follow-up: direct GTK copy actions now publish the captured selection or
address before the popover closes, retaining the real activation/Wayland serial.
Editable Copy also uses the captured text rather than sending Ctrl+C after an
input's selection may have lost focus. Other native edit chords retain deferred
focus/grab release. Packaged physical right-click validation remains pending.
On Linux, enabling BraveWebBluetoothAPI alone did not expose navigator.bluetooth;
the narrowly scoped WebBluetooth Blink runtime gate is also required. A real
permission.site native probe now reports functions for showOpenFilePicker,
showDirectoryPicker, navigator.bluetooth.requestDevice and registerProtocolHandler.
API availability does not establish chooser/backend permission completion.

Webpage selection and scrollbar styling now use browser-owned per-document CSS
stylesheets, with GTK accent/contrast notifications and navigation/session guards.
They work under strict site CSP without adding a page-world binding or bypassing
CSP. A hash-pinned, class-local CustomScrollbar vtable relocation plus native
OverlayScrollbar mode makes CSS-styled bars overlay the page instead of reserving
layout width. The scrollbar, track, track-piece and corner backgrounds are cleared;
native wheel and thumb dragging remain upstream. Root and nested zero-gutter
checks and the reported site's transparent track pass. Out-of-process frame
coverage and packaged visual acceptance remain separate follow-ups.

## Translation submenu and KeePassXC (2026-10-25)

Translate Page is a submenu of the existing GTK site menu, with a searchable
language dropdown, native progress/error feedback and Show Original. Brave's
per-tab TranslateManager performs translation and reversion; the exact-binary
shim suppresses only its Views bubble. Real native and packaged GTK translation
and reversion probes pass, including stale-page rejection and navigation cleanup.

The official KeePassXC-Browser 1.10.4.1 is now bundled under a separate stable
identity, loaded only in the normal profile. A profile-owned native host and
sanitized launcher connect it to the installed keepassxc-proxy without modifying
another browser's configuration. Actual host connection / KeePassXC availability
and upstream TOTP/passkey initialization pass. Password/TOTP/passkey functionality
uses upstream encrypted browser integration and still requires first-use database
pairing. The added unlock adapter requests KeePassXC's own prompt on a real user
gesture in a detected login field; its gesture/frame/cooldown/coalescing test
passes. No database password is handled by Gnosis. See
`engine/data/keepassxc/README.md` for maintenance and remaining live validation.

KeePassXC UI follow-up: the upstream provider is now an invisible internal
backend, not a user-facing extension. The hamburger-menu KeePassXC action opens
native libadwaita database, association, login-selection, TOTP and settings
controls. A fresh-profile packaged GTK test verifies the real service, settings
persistence and navigation cleanup. The existing-profile worker-cache issue is
handled by installing only the trusted adapter in the verified private worker;
no user caches/storage are wiped. Page overlays retain upstream closed Shadow
DOM/input handlers but use GTK palette/font variables, libadwaita popover/list
metrics and GNOME symbolic icons. A visibly open fixture overlay verifies the
palette, shape and zero layout shift. Full native uBlock passes alongside this
backend. The user confirmed a GitHub passkey round trip; the Discord failure has
not been attributed.

Follow-up checks: extension action popups now report intrinsic size from a
browser-owned isolated world, grow/shrink within GTK window limits, and give full
uBlock its actual owning tab ID. Real uBlock More/Less changes GTK allocation;
the full native network/cosmetic/scriptlet and settings-toggle checks pass.

KeePassXC's integration revision is now 1.10.4.104. Its native panel includes
Redetect Login Fields. A trusted focus on a newly introduced password field also
rescans upstream combinations once. When an integration loads after the page,
the adapter can inject only its own manifest-declared content scripts into that
tab's top frame, honoring match exclusions and Chromium host grants. This needs
the scripting permission. Unit checks and packaged native redetection checks
pass, including a form introduced after initial load. Brave's separate password
saving and automatic sign-in preferences are disabled without deleting its vault.

The native WebAuthn view/model adapter now presents GTK mechanism choices,
phone/tablet pairing QR, USB instructions, progress and supported error sheets.
Each request is bound to its actual WebContents via its private CDP target;
revision checks, native input protection and original native callbacks remain.
A real local WebAuthn request reaches the GTK QR sheet and Cancel rejects the
original promise. QR material stays on the inherited private socket and is
rendered in memory. Account choices now use native GTK rows and the original
Chromium selection callbacks. Real virtual-CTAP credentials produce a verified
ECDSA assertion for the chosen account, with user verification and counter
checks. A phone/tablet row appears only when the native request offers hybrid
transport; it restarts discovery through the original delegate before invoking
that mechanism. Real QR presentation and cancellation pass. Each sheet is
measured again when its contents change, and actions stay outside the scroller.
PIN entry and GPM sheets retain native
fallbacks. The separate conditional passkey/autofill suggestion still needs its
own GTK adapter; this is not an exhaustive replacement of Chromium auth UI.

Translate Page now gives the language label additional inset and omits the
service-notice footer, as requested.

Overview profiling found one surface commit invalidated every tab's compositor
view (74 of 111 queued redraws targeted hidden views in a three-tab run). Commit
invalidation now follows the actual root/popup parent to the owning view. The
follow-up run queued 10 redraws, but frame intervals varied with monitor placement
and load; these runs do not establish a general animation-latency improvement.
The reported intermittent severe stall remains unconfirmed. The immediate-close
popup-reopen failure in the probe was a synthetic timing artifact: activating a
row before the reopened menu received its first layout. A test that lets GTK
map/layout the menu before selecting the actual row passes reopen and More/Less.


## Gnosis branding and desktop transfers (2026-10-25)

The product name, launcher, About UI, notifications and primary executable now
use Gnosis. The supplied root `gnosis.svg` is packaged unchanged for the normal,
development and Canary application icons; the symbolic icon follows its globe
geometry. The later deep rebrand also renames internal namespaces, protocols,
widgets, build targets, resources, environment variables and application/schema
IDs to Gnosis. The current application ID is `com.neg-zero.Gnosis`, with
`com.neg_zero.Gnosis` for D-Bus interfaces. The first normal launch migrates
profile directories and settings; compatibility redirects preserve stored
absolute extension paths and their IDs. KeePassXC's public key remains stable.

The context menu now offers Copy Image. It reads the original resource through
the owning frame's Brave cache/network context, decodes asynchronously, then
publishes a GTK texture during the real menu activation. Native pointer/clipboard
checks compare original RGBA pixels, dimensions and transparency for cross-origin
images, data/blob URLs and a same-process subframe. Context-menu teardown now
waits until GtkModelButton has activated its action; immediately unparenting on
closed destroyed action resolution. Out-of-process iframe context menus remain
an existing gap and were not fixed by this image-copy change.

The compositor now bridges actual GTK desktop drags and native Wayland data
offers in both directions, preserving MIME streams, native DOM drop events and
Chromium file access. Incoming desktop files use copy actions. A real desktop
GTK file-list drag reaches a trusted DOM drop and exposes the exact fixture file
contents; a webpage link drag reaches a separate GTK destination. Escape-cancel
followed by a real webpage click passes. Host unmapping/disposal cancels transfers.
The automated peer runs under X11 with a real nested Wayland browser; a human
check with Nautilus on the outer Wayland desktop remains useful.

Drag previews now use the offered image bytes, center-cropped into a rounded
80-pixel square, a clipped text square, both for mixed selections, or a compact
link icon. Mixed HTML can retrieve an already-loaded image from the owning
page's resource cache; preview payloads cannot initiate arbitrary network or
filesystem reads. Bounded asynchronous reads cancel with the drag. Real GTK
pointer drags verify all four previews, their square bounds, desktop transfers,
and Escape/input recovery. The About/AppStream developer is Negative Zero team.

The page menu no longer inherits a hardcoded translation-pane width. Its native
GtkPopoverMenu sizing keeps both pages compact without clipping; packaged
open/translate/back navigation and measured bounds pass.

## Small fixes before shutdown (2026-10-26)

The existing edited-form warning now evaluates self-contained default/value
comparisons in the private world. The retired WebKit Ephy helper was absent.
Nine real-browser form cases and GTK last-tab Cancel/Discard pass. Readonly and
disabled fields are ignored; pristine/reset forms and a single short search do
not prompt. This is a heuristic, not a complete native beforeunload bridge.
Tab-close callback lifetime guards and previous-tab weak-pointer cleanup are
also repaired. A multi-tab switching probe remains a follow-up.

Native context menus now open the actual GTK-hosted Developer Tools window.
Actual menu activation and full native uBlock regressions pass. See the dated
handoff in GNOSIS-TODO.md for the next session and persistent test artifacts.
