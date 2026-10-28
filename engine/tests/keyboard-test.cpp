/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <source_location>
#include <string>

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("Keyboard test timed out at %s:%u", location.file_name(), location.line());
}
static std::string evaluate(EphyEngineView* view, const char* script) {
  GAsyncResult* result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject*, GAsyncResult* result, gpointer data) {
      *static_cast<GAsyncResult**>(data) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError* error = nullptr;
  auto* value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  auto text = script_result_text(value);
  json_object_unref(value); g_object_unref(result);
  return text;
}
static void xdo(std::initializer_list<const char*> arguments) {
  std::vector<const char*> command{"xdotool"};
  command.insert(command.end(), arguments.begin(), arguments.end()); command.push_back(nullptr);
  GError* error = nullptr;
  auto* child = g_subprocess_newv(command.data(), G_SUBPROCESS_FLAGS_NONE, &error);
  g_assert_no_error(error); g_assert_nonnull(child);
  g_assert_true(g_subprocess_wait_check(child, nullptr, &error)); g_assert_no_error(error);
  g_object_unref(child);
}
static GtkWidget* surface(GtkWidget* widget) {
  if (GTK_IS_GL_AREA(widget)) return widget;
  for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto* result = surface(child)) return result;
  return nullptr;
}
int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  // Exercise GTK's real Unicode composition deterministically without an external IM daemon.
  g_setenv("GTK_IM_MODULE", "simple", true);
  auto* app = adw_application_new("com.neg-zero.Gnosis.KeyboardTest", G_APPLICATION_NON_UNIQUE);
  GError* error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/gnosis-keyboard-test-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  auto* window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_title(GTK_WINDOW(window), "Gnosis keyboard contract");
  gtk_window_set_default_size(GTK_WINDOW(window), 700, 500);
  auto* view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool* loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, R"HTML(data:text/html;charset=utf-8,<!doctype html><title>Keyboard fixture</title>
    <button id=button>Activate</button><input id=check type=checkbox><textarea id=field></textarea>
    <script>
    window.clicks=0;window.events=[];
    button.onclick=()=>clicks++;
    for(const type of ['keydown','keypress','keyup','compositionstart','compositionend'])
      document.addEventListener(type,e=>events.push({type,key:e.key,trusted:e.isTrusted,shift:e.shiftKey,ctrl:e.ctrlKey,alt:e.altKey,meta:e.metaKey}));
    </script>)HTML");
  gtk_window_present(GTK_WINDOW(window)); wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, handler);
  xdo({"search", "--sync", "--name", "^Gnosis keyboard contract$", "windowfocus", "--sync"});
  g_assert_nonnull(surface(GTK_WIDGET(view)));
  gtk_widget_grab_focus(surface(GTK_WIDGET(view)));
  evaluate(view, "button.focus();true");
  xdo({"keydown", "space"});
  wait_for([&] { return evaluate(view, "events.filter(e=>e.type==='keypress'&&e.key===' '&&e.trusted).length===1&&clicks===0") == "true"; });
  xdo({"keyup", "space"});
  wait_for([&] { return evaluate(view, "clicks===1") == "true"; });
  evaluate(view, "check.focus();events=[];true");
  xdo({"key", "space"});
  wait_for([&] { return evaluate(view, "check.checked&&events.filter(e=>e.type==='keypress'&&e.key===' ').length===1") == "true"; });
  g_print("Real GTK Space produces one trusted keypress, activates buttons on keyup, and toggles checkboxes: PASS\n");

  evaluate(view, "field.focus();events=[];true");
  xdo({"key", "space", "shift+space"});
  wait_for([&] { return evaluate(view, "field.value==='  '&&events.filter(e=>e.type==='keypress'&&e.key===' ').length===2&&events.some(e=>e.type==='keypress'&&e.shift)") == "true"; });
  for (const auto& [key, modifier] : std::vector<std::pair<const char*, const char*>>{
      {"ctrl+space", "ctrl"}, {"alt+space", "alt"}, {"super+space", "meta"}}) {
    evaluate(view, "events=[];true");
    xdo({"key", key});
    wait_for([&] { return evaluate(view, "events.some(e=>e.type==='keyup'&&e.key===' ')") == "true"; });
    g_print("%s state: %s\n", key, evaluate(view, "JSON.stringify({text:field.value,events})").c_str());
    // Blink's Linux ShouldInsertCharacter blocks Ctrl, but accepts Meta; Xvfb has no GNOME shortcut grab.
    const char* expected = !g_strcmp0(modifier, "meta") ? "   " : "  ";
    auto check = std::string("field.value==='")+expected+
      "'&&events.filter(e=>e.type==='keypress'&&e.key===' ').length<=1&&events.filter(e=>e.type==='keydown'&&e.key===' ').every(e=>e."+
      modifier+")&&events.filter(e=>e.type==='keypress'&&e.key===' ').every(e=>e."+modifier+")";
    g_assert_true(evaluate(view, check.c_str()) == "true");
  }
  g_print("Literal/Shift Space insert once; Ctrl/Alt/Meta preserve modifiers and Blink's native Linux insertion behavior without duplicates: PASS\n");

  evaluate(view, "field.value='';events=[];true");
  xdo({"key", "ctrl+shift+u", "1", "f", "6", "8", "0", "Return"});
  wait_for([&] { return evaluate(view, "field.value==='🚀'") == "true"; });
  g_assert_true(evaluate(view, "events.some(e=>e.type==='compositionstart')&&events.some(e=>e.type==='compositionend')") == "true");
  // Space completes this real IM sequence: it must commit the composed character, not a literal keypress.
  evaluate(view, "events=[];true");
  xdo({"key", "ctrl+shift+u", "e", "9", "space"});
  wait_for([&] { return evaluate(view, "field.value==='🚀é'") == "true"; });
  g_assert_true(evaluate(view, "!events.some(e=>e.type==='keypress'&&e.key===' ')") == "true");
  g_print("Real GTK Unicode composition preserves non-BMP text and consumes its finishing Space without inserting or activating it: PASS\n");

  evaluate(view, "button.focus();events=[];true"); xdo({"key", "Return"});
  wait_for([&] { return evaluate(view, "clicks===2") == "true"; });
  g_print("Enter button activation remains functional: PASS\n");
  gtk_window_destroy(GTK_WINDOW(window)); wait_for([] { return Browser::active_count == 0; });
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
