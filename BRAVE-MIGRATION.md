# Brave backend migration

Objective: take the working Brave PoC, embed it in Epiphany's actual GTK shell,
remove WebKit and the CEF compatibility dependencies, and connect shell services
to the real Brave browser. This replaces the previous CEF engine direction.

User constraints: work solo, prioritize implementation, use `nix run` to build and
launch, keep the Chromium sandbox enabled and core dumps disabled, no interface-
polish skills. The original X11 PoC can be closed; preserve useful test windows
while replacing their backend. Brave supplies the web-content backend; Jupiter
owns all browser UI and user-facing browser behavior.

Launch preference: reuse the normal ~/.local/share/jupiter profile for user test
builds. Earlier per-build /tmp profiles made history appear to disappear. Their
databases remain intact; the default history had 43 saved URLs when checked.
Use a fresh isolated profile only for a specific isolation test, not each build.

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

The standalone PoC in ../jupiter-brave-poc is already visibly verified: GTK native
surface -> native child container -> actual Brave app window. Its normal-window
coordinates add GTK's CSD shadow offset; native window extents are cleared and
Mutter withdrawal completes before reparenting. F12 opens GTK Inspector and F9
shows the native content boundary. This is external-window X11 embedding, not a
source fork of Brave or native Wayland integration; don't claim otherwise.

Integration evidence (2026-10-24): the first extraction incorrectly matched a
Mutter decoration frame by its copied page title. Restoring the PoC's exact
Brave WM_CLASS and normal-client-window checks removed the ghost frames and
clipped viewport. The corrected live run has only the GTK Jupiter client in
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
Each normal/private Brave process owns a shared JupiterCompositor GObject;
registered widgets bind their bootstrap tokens to the corresponding xdg
toplevel. The actual shell has run with two independently registered GPU-backed
tabs on a single Brave process and compositor. A hidden reusable widget retains
the last app window across tab closure. Renderer ownership is held by imported
buffer leases until GTK releases those buffers. The X11 path remains a temporary
fallback via JUPITER_SURFACE=x11, not the intended final architecture.

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
brave.p3a.notice_acknowledged=true. This only prepares Jupiter's backend profiles,
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
(JUPITER_UNCAP_FRAMES=0 restores the internal limiter). This is not an efficient
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
profile remains ~/.local/share/jupiter. Native full-document snapshot requests
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
an opt-in `--jupiter-native-ui` source patch in one stable component file, with
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
