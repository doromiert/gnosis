/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include <filesystem>
#include <functional>
#include <source_location>
#include <string>
#include <thread>
CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);

static void wait_for(const std::function<bool()>& predicate, const char* operation = "condition",
                     std::source_location caller = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("Timed out at %s:%u: %s", caller.file_name(), caller.line(), operation);
}
struct Evaluation {
  GAsyncResult *result = nullptr;
  unsigned calls = 0;
  std::string script;
  ~Evaluation() { g_clear_object(&result); }
  static void done(GObject*, GAsyncResult *result, gpointer user) {
    auto *self = static_cast<Evaluation*>(user);
    ++self->calls;
    g_assert_null(self->result);
    self->result = G_ASYNC_RESULT(g_object_ref(result));
  }
  void start(EphyEngineView *view, const char *script, const char *world = nullptr,
             GCancellable *cancel = nullptr, const char *source = nullptr) {
    ephy_engine_web_view_evaluate_javascript(view, script, -1, world, source, cancel, done, this);
    this->script = script;
  }
  GAsyncResult *get() { wait_for([this]{ return result; }, script.c_str()); return result; }
};
static JsonObject *result(EphyEngineView *view, const char *script, const char *world = nullptr, const char *source = nullptr) {
  Evaluation eval;
  eval.start(view, script, world, nullptr, source);
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, eval.get(), &error);
  if (error) g_error("Script failed: %s; %s", script, error->message);
  g_assert_nonnull(value);
  return value;
}
static std::string evaluate(EphyEngineView *view, const char *script, const char *world = nullptr, const char *source = nullptr) {
  auto *value = result(view, script, world, source);
  auto text = script_result_text(value); json_object_unref(value); return text;
}
static void expect(EphyEngineView *view, const char *script, const char *expected, const char *world = nullptr) {
  auto output = evaluate(view, script, world);
  g_assert_cmpstr(output.c_str(), ==, expected);
}
static void navigate(EphyEngineView *view, const char *uri) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, uri);
  wait_for([&]{ return loaded; }, uri);
  g_signal_handler_disconnect(view, handler);
}
static EphyEngineView *page(GtkApplication *app, EphyEngineSession *session, const char *uri) {
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  gtk_window_present(GTK_WINDOW(window));
  navigate(view, uri);
  auto title = evaluate(view, "document.title");
  g_assert_cmpstr(title.c_str(), ==, "Script worlds");
  return view;
}
static void assert_error(EphyEngineView *view, Evaluation& eval, GIOErrorEnum code) {
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, eval.get(), &error);
  g_assert_null(value);
  g_assert_error(error, G_IO_ERROR, code);
  g_clear_error(&error);
  g_assert_cmpuint(eval.calls, ==, 1);
}
static void serialized_results(EphyEngineView *view) {
  g_autoptr(JsonObject) undefined = result(view, "undefined");
  g_assert_cmpstr(json_object_get_string_member(undefined, "type"), ==, "undefined");
  g_assert_false(json_object_has_member(undefined, "value"));
  g_autoptr(JsonObject) null = result(view, "null");
  g_assert_cmpstr(json_object_get_string_member(null, "subtype"), ==, "null");
  g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(null, "value")));
  g_autoptr(JsonObject) object = result(view, "({title:'Unicode: ąß🪐', items:[null,false,42,{name:'nested'}]})");
  auto *payload = json_object_get_object_member(object, "value");
  g_assert_cmpstr(json_object_get_string_member(payload, "title"), ==, "Unicode: ąß🪐");
  auto *items = json_object_get_array_member(payload, "items");
  g_assert_cmpuint(json_array_get_length(items), ==, 4);
  g_assert_true(JSON_NODE_HOLDS_NULL(json_array_get_element(items, 0)));
  g_assert_false(json_array_get_boolean_element(items, 1));
  g_assert_cmpint(json_array_get_int_element(items, 2), ==, 42);
  g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 3), "name"), ==, "nested");
  for (const char *value : {"NaN", "Infinity", "-Infinity", "-0", "9007199254740993n"}) {
    g_autoptr(JsonObject) remote = result(view, value);
    g_assert_cmpstr(json_object_get_string_member(remote, "unserializableValue"), ==, value);
    g_assert_false(json_object_has_member(remote, "value"));
  }
  for (const char *value : {"(()=>{})"}) {
    g_autoptr(JsonObject) remote = result(view, value);
    g_assert_cmpstr(json_object_get_string_member(remote, "type"), ==, "function");
    g_assert_false(json_object_has_member(remote, "objectId"));
  }
  for (const char *script : {"throw new Error('native failure')", "Promise.reject(new Error('rejected'))", "Symbol('native')", "(()=>{const cycle={};cycle.self=cycle;return cycle})()"}) {
    Evaluation eval; eval.start(view, script);
    assert_error(view, eval, G_IO_ERROR_FAILED);
  }
  g_print("Native Chromium results preserve nested JSON, Unicode, null/undefined, BigInt and non-finite numbers, release handles, and report serialization/script errors: PASS\n");
}
static void same_document_navigation(EphyEngineView* view, const std::string& uri) {
  auto* resource = static_cast<EphyEngineResource*>(g_object_ref(ephy_engine_web_view_get_main_resource(view)));
  auto* response = ephy_engine_web_resource_get_response(resource);
  g_assert_nonnull(response);
  unsigned events = 0;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent, unsigned* events) {
    ++*events;
  }), &events);
  Evaluation pending;
  pending.start(view, "globalThis.fragmentPromise=new Promise(resolve=>globalThis.resolveFragment=resolve)", "fragment-world");
  wait_for([&] { return evaluate(view, "typeof resolveFragment", "fragment-world") == "function"; });
  expect(view, "(()=>{const a=document.createElement('a');a.id='fragment-link';a.href='#native-fragment';a.textContent='Fragment';a.style='position:fixed;left:0;top:0;width:140px;height:50px;z-index:10000';document.body.append(a);return true})()", "true");
  auto host = ephy_engine_view_browser(view)->GetHost();
  host->SetFocus(true);
  CefMouseEvent mouse; mouse.x = 40; mouse.y = 25;
  host->SendMouseClickEvent(mouse, MBT_LEFT, false, 1);
  host->SendMouseClickEvent(mouse, MBT_LEFT, true, 1);
  auto fragment = uri + "#native-fragment";
  auto current_uri = [&] {
    auto* item = ephy_engine_back_forward_list_get_current_item(ephy_engine_web_view_get_back_forward_list(view));
    return item ? std::string(ephy_engine_back_forward_list_item_get_uri(item)) : "";
  };
  wait_for([&] { return current_uri() == fragment; }, "fragment history");
  ephy_engine_web_view_go_back(view);
  wait_for([&] { return current_uri() == uri; }, "same-document back history");
  expect(view, "new Promise(resolve=>{const f=document.querySelector('iframe');f.onload=()=>resolve('child loaded');f.src+='?fragment-test'})", "child loaded");
  g_assert_true(ephy_engine_web_view_get_main_resource(view) == resource);
  g_assert_true(ephy_engine_web_resource_get_response(resource) == response);
  g_assert_cmpuint(events, ==, 0);
  g_assert_null(pending.result);
  expect(view, "document.getElementById('fragment-link').remove();true", "true");
  expect(view, "resolveFragment('preserved');true", "true", "fragment-world");
  auto* result = ephy_engine_web_view_evaluate_javascript_finish(view, pending.get(), nullptr);
  g_assert_nonnull(result);
  g_assert_true(script_result_text(result) == "preserved");
  json_object_unref(result);
  g_signal_handler_disconnect(view, handler);
  g_object_unref(resource);
  g_print("Fragments, same-document history and subframe loads preserve the main resource, native response and pending isolated-world scripts without false document events: PASS\n");
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.ScriptTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-script-test-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  std::string uri = std::string(g_getenv("JUPITER_TEST_URL")) + "script-fixture";
  auto *view = page(GTK_APPLICATION(app), ephy_engine_session_get_default(), uri.c_str());
  same_document_navigation(view, uri);
  serialized_results(view);
  expect(view, "globalThis.pageSecret='page';pageSecret", "page");
  expect(view, "typeof pageSecret", "undefined", "epiphany");
  expect(view, "globalThis.nativeSecret='native';document.body.dataset.native='shared DOM';nativeSecret", "native", "epiphany");
  expect(view, "nativeSecret", "native", "epiphany");
  expect(view, "typeof nativeSecret", "undefined", "other-world");
  expect(view, "typeof nativeSecret==='undefined' && document.body.dataset.native==='shared DOM'", "true");
  expect(view, "(()=>{try{return !!document.querySelector('iframe').contentWindow.document}catch(e){return false}})()", "false", "epiphany");
  expect(view, "Promise.resolve('awaited')", "awaited", "epiphany");
  Evaluation hidden;
  hidden.start(view, "new Promise(resolve=>globalThis.resolveHidden=resolve)", "epiphany");
  wait_for([&]{ return evaluate(view, "typeof resolveHidden", "epiphany") == "function"; });
  gtk_widget_set_visible(GTK_WIDGET(view), false);
  expect(view, "resolveHidden('background');true", "true", "epiphany");
  auto *hidden_value = ephy_engine_web_view_evaluate_javascript_finish(view, hidden.get(), &error);
  g_assert_no_error(error);
  g_assert_true(g_str_equal(json_object_get_string_member(hidden_value, "type"), "string"));
  char *hidden_text = g_strdup(script_result_text(hidden_value).c_str());
  g_assert_cmpstr(hidden_text, ==, "background");
  g_free(hidden_text); json_object_unref(hidden_value); g_clear_object(&hidden.result);
  gtk_widget_set_visible(GTK_WIDGET(view), true);
  auto stack = evaluate(view, "new Error('source').stack", "epiphany", "https://example.test/source\nglobalThis.injected=true");
  g_assert_nonnull(strstr(stack.c_str(), "https://example.test/source%0AglobalThis.injected=true"));
  expect(view, "typeof injected", "undefined", "epiphany");
  g_print("Native named worlds isolate globals, share DOM, obey cross-origin boundaries and preserve source URLs: PASS\n");

  auto *other = page(GTK_APPLICATION(app), ephy_engine_session_get_default(), uri.c_str());
  expect(other, "typeof nativeSecret", "undefined", "epiphany");
  auto *private_session = ephy_engine_session_new(true);
  auto *private_view = page(GTK_APPLICATION(app), private_session, uri.c_str());
  expect(private_view, "typeof nativeSecret", "undefined", "epiphany");
  expect(private_view, "globalThis.nativeSecret='private';nativeSecret", "private", "epiphany");
  expect(view, "nativeSecret", "native", "epiphany");
  std::string next = uri + "?next";
  navigate(view, next.c_str());
  expect(view, "typeof nativeSecret", "undefined", "epiphany");
  expect(view, "typeof pageSecret", "undefined");
  g_print("Execution contexts are isolated between tabs/private sessions and renewed after navigation: PASS\n");

  auto *cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  Evaluation precancelled;
  precancelled.start(view, "globalThis.cancelLeak=true", "epiphany", cancel);
  assert_error(view, precancelled, G_IO_ERROR_CANCELLED);
  expect(view, "typeof cancelLeak", "undefined", "epiphany");
  g_object_unref(cancel);
  cancel = g_cancellable_new();
  Evaluation pending;
  pending.start(view, "document.body.dataset.waiting='yes';globalThis.waitingPromise=new Promise(()=>{})", "epiphany", cancel);
  wait_for([&]{ return evaluate(view, "document.body.dataset.waiting") == "yes"; });
  std::thread([cancel]{ g_cancellable_cancel(cancel); }).join();
  assert_error(view, pending, G_IO_ERROR_CANCELLED);
  g_object_unref(cancel);
  Evaluation exception;
  exception.start(view, "throw new Error('fixture')", "epiphany");
  assert_error(view, exception, G_IO_ERROR_FAILED);
  g_print("Pre-cancelled scripts do not run; pending promises cancel and exceptions propagate: PASS\n");

  Evaluation navigating;
  navigating.start(view, "document.body.dataset.navigating='yes';globalThis.navigationPromise=new Promise(()=>{})", "epiphany");
  wait_for([&]{ return evaluate(view, "document.body.dataset.navigating") == "yes"; });
  navigate(view, uri.c_str());
  assert_error(view, navigating, G_IO_ERROR_CANCELLED);
  Evaluation superseded;
  superseded.start(view, "globalThis.mustNotRun=true", "epiphany");
  navigate(view, next.c_str());
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, superseded.get(), &error);
  g_clear_pointer(&value,json_object_unref); g_clear_error(&error);
  expect(view, "typeof mustNotRun", "undefined", "epiphany");
  Evaluation moving;
  moving.start(view, "globalThis.moveSecret='retained';document.body.dataset.moving='yes';globalThis.movingPromise=new Promise(()=>{})", "epiphany");
  wait_for([&]{ return evaluate(view, "document.body.dataset.moving") == "yes"; });
  auto *old_window = ADW_APPLICATION_WINDOW(gtk_widget_get_root(GTK_WIDGET(view)));
  auto *new_window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  g_object_ref(view);
  adw_application_window_set_content(old_window, nullptr);
  adw_application_window_set_content(new_window, GTK_WIDGET(view));
  g_object_unref(view);
  gtk_window_present(GTK_WINDOW(new_window));
  assert_error(view, moving, G_IO_ERROR_CLOSED);
  expect(view, "moveSecret", "retained", "epiphany");
  for (auto *eval : {&precancelled, &pending, &exception, &navigating, &superseded, &moving}) {
    g_assert_cmpuint(eval->calls, ==, 1);
    g_clear_object(&eval->result);
  }
  Evaluation closing[6];
  // Keep promises reachable so garbage collection cannot finish the fixture early.
  for (auto& eval : closing) eval.start(view, "globalThis.closingPromises??=[];closingPromises[closingPromises.length]=new Promise(()=>{})", "epiphany");
  wait_for([&]{ return evaluate(view, "globalThis.closingPromises?.length", "epiphany") == "6"; });
  gtk_window_destroy(GTK_WINDOW(new_window));
  for (auto& eval : closing) {
    assert_error(view, eval, G_IO_ERROR_CLOSED);
    g_clear_object(&eval.result);
  }
  while (auto *windows = gtk_application_get_windows(GTK_APPLICATION(app))) gtk_window_destroy(GTK_WINDOW(windows->data));
  wait_for([]{ return Browser::active_count == 0; });
  g_print("Navigation, reparenting and normal window closure resolve scripts once without retaining closed tabs: PASS\n");
  g_object_unref(private_session);
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
