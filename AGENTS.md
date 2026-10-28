# Gnosis

Keep the shell grounded in Epiphany's actual GTK layouts and native libadwaita
styles. The active engine direction is the working Brave native-window PoC,
embedded in this actual Epiphany shell. Reuse Select's tab/workspace behavior.
Remove WebKit and the temporary CEF compatibility layer as their services move
to Brave; do not describe a partial bridge as a completed engine migration.
Work solo now. The user temporarily authorized parallel agents until the Codex
usage/quota reset; root observed that the current weekly window started at
2026-10-26 07:05:30 Europe/Warsaw and stopped all subagents. Do not spawn or
reactivate agents without another explicit user instruction.

Do not use the interface-polish / make-interfaces-feel-better skill for Gnosis.
The user explicitly considers it irrelevant here. Follow Epiphany's source and
GTK/libadwaita conventions directly for UI work.

Do not replace full uBlock Origin with Lite, a mock extension API, or a separate
URL blocker. Changes to extension/engine support must pass the real uBlock checks.
Do not disable the Chromium sandbox during ordinary browsing. Keep profiles and
application identifiers independent from Select and Epiphany.

For local build-and-launch workflows, use `nix run`; it already builds the app.
Do not run a separate `nix build` first.
Close outdated Gnosis instances as soon as an application build starts. The
user explicitly prefers this to leaving old windows available during builds.

Prefer small, version/hash-checked changes to the prebuilt Brave binary or a
native shim over a local Chromium rebuild. The user accepts some upgrade
fragility and does not have the storage/time for a full build. Preserve isolated
source patches as reference and keep upgrade/application checks explicit.

Run meaningful unit checks and the affected native integration checks. New
extension adapters must enforce host permissions and isolate private contexts.

The product and internal namespaces are Gnosis. The application/schema ID is
`com.neg-zero.Gnosis`; D-Bus interface names use `com.neg_zero.Gnosis` because
interfaces cannot contain hyphens. Legacy Jupiter names belong only in the
one-time migration code. Preserve user data and the bundled extension public
keys, which retain KeePassXC pairing and extension storage.
