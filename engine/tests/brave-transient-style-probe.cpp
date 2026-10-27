/* SPDX-License-Identifier: GPL-3.0-or-later
 * Preload only into a disposable Gnosis --profile instance; it quits on completion.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static std::shared_ptr<GnosisBrave::Page> page;
static unsigned writes;
static std::string main_frame;
static bool started;
static void finish(bool ok, const std::string& detail) {
  g_print("TRANSIENT_STYLE_%s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  page.reset();
  g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
}
static gboolean verify(gpointer) {
  page->command("CSS.createStyleSheet", "{\"frameId\":" + GnosisBrave::quote(main_frame) + ",\"force\":true}", [](JsonObject*, const std::string& error) {
    if (error != "No target stylesheet found" && error.find("not enabled") == std::string::npos) { finish(false, "CSS agent remained enabled: " + error); return; }
    page->command("Runtime.evaluate", R"JSON({"expression":"[1,2,3].map(i=>getComputedStyle(document.getElementById('item'+i)).color).join('|')","returnByValue":true})JSON",
      [](JsonObject* result, const std::string& failure) {
        auto* value = result ? json_object_get_object_member(result, "result") : nullptr;
        const std::string colors = value ? json_object_get_string_member_with_default(value, "value", "") : "";
        finish(failure.empty() && colors == "rgb(31, 41, 51)|rgb(32, 42, 52)|rgb(33, 43, 53)", colors + failure);
      });
  });
  return G_SOURCE_REMOVE;
}
static gboolean poll(gpointer) {
  if (!page) {
    auto* app = g_application_get_default();
    if (!GTK_IS_APPLICATION(app)) return G_SOURCE_CONTINUE;
    auto* window = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (!window) return G_SOURCE_CONTINUE;
    auto embed = reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT, "ephy_window_get_active_embed"));
    auto view = reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT, "ephy_embed_get_web_view"));
    auto get = reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT, "_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
    if (!embed || !view || !get || !embed(window)) return G_SOURCE_CONTINUE;
    page = get(view(embed(window)));
  }
  if (!page || page->loading || page->session.empty() || page->title != "Gnosis transient styles" || started) return G_SOURCE_CONTINUE;
  started = true;
  page->command("Page.getFrameTree", "{}", [](JsonObject* result, const std::string& failure) {
    auto* tree = result ? json_object_get_object_member(result, "frameTree") : nullptr;
    auto* frame = tree ? json_object_get_object_member(tree, "frame") : nullptr;
    if (!frame || !failure.empty()) { finish(false, failure); return; }
    const std::string id = json_object_get_string_member(frame, "id");
    main_frame = id;
    for (unsigned i = 1; i <= 3; ++i)
      page->command_for_frame(id, "CSS.createStyleSheet", "{\"frameId\":" + GnosisBrave::quote(id) + ",\"force\":true}",
        [id, i](JsonObject* sheet, const std::string& error) {
          if (!sheet || !error.empty()) { finish(false, error); return; }
          const std::string css = "#item" + std::to_string(i) + "{color:rgb(" + std::to_string(30+i) + "," + std::to_string(40+i) + "," + std::to_string(50+i) + ")!important}";
          page->command_for_frame(id, "CSS.setStyleSheetText", "{\"styleSheetId\":" + GnosisBrave::quote(json_object_get_string_member(sheet, "styleSheetId")) + ",\"text\":" + GnosisBrave::quote(css) + "}",
            [](JsonObject*, const std::string& failure) {
              if (!failure.empty()) finish(false, failure);
              else if (++writes == 3) g_timeout_add(300, verify, nullptr);
            });
        });
  });
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0) return; exe[n] = 0;
  if (strstr(exe, "/.gnosis-wrapped")) g_timeout_add_seconds(2, poll, nullptr);
}
