# Jupiter

Keep the shell grounded in Epiphany's actual GTK layouts and native libadwaita
styles. Reuse Select's existing CEF rendering/input and workspace behavior.

Do not replace full uBlock Origin with Lite, a mock extension API, or a separate
URL blocker. Changes to extension/engine support must pass the real uBlock checks.
Do not disable the Chromium sandbox during ordinary browsing. Keep profiles and
application identifiers independent from Select and Epiphany.

Run meaningful unit checks and the affected native integration checks. New
extension adapters must enforce host permissions and isolate private contexts.
