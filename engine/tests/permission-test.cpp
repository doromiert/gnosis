/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);
static void wait_for(const std::function<bool()>& predicate) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
  g_assert_true(predicate());
}
static std::string evaluate(EphyEngineView *view, const char *script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject*, GAsyncResult *result, gpointer user) { *static_cast<GAsyncResult**>(user) = G_ASYNC_RESULT(g_object_ref(result)); }, &result);
  wait_for([&]{ return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  char *text = g_strdup(script_result_text(value).c_str()); std::string output(text);
  g_free(text); json_object_unref(value); g_object_unref(result); return output;
}
static void navigate(EphyEngineView *view, const std::string& uri) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, uri.c_str()); wait_for([&]{ return loaded; }); g_signal_handler_disconnect(view, handler);
}
struct Requests {
  std::vector<EphyEnginePermission*> requests;
  ~Requests() { for (auto *request : requests) g_object_unref(request); }
  static gboolean received(EphyEngineView*, EphyEnginePermission *request, Requests *self) {
    self->requests.push_back(EPHY_ENGINE_PERMISSION(g_object_ref(request))); return true;
  }
  EphyEnginePermission *ask(EphyEngineView *view, const char *script) {
    auto count = requests.size(); evaluate(view, script);
    wait_for([&]{ return requests.size() > count; }); return requests.back();
  }
};
static EphyEngineView *page(GtkApplication *app, EphyEngineSession *session, const std::string& uri, Requests& requests) {
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view)); gtk_window_present(GTK_WINDOW(window));
  g_signal_connect(view, "permission-request", G_CALLBACK(Requests::received), &requests);
  navigate(view, uri); return view;
}
static void outcome(EphyEngineView *view, const char *expected) {
  wait_for([&]{ return evaluate(view, "outcome") == expected; });
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps(); if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.PermissionTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr; g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-permission-test-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  const std::string base = g_getenv("JUPITER_TEST_URL");
  const std::string uri = base + "permission-fixture";
  const std::string origin = base.substr(0, base.size()-1);
  auto *regular = ephy_engine_session_get_default();
  EphyEnginePermissionRecord legacy[] = {
    {EPHY_ENGINE_PERMISSION_NOTIFICATIONS, origin.c_str(), EPHY_ENGINE_PERMISSION_STATE_DENY},
    {EPHY_ENGINE_PERMISSION_LOCATION, origin.c_str(), EPHY_ENGINE_PERMISSION_STATE_ALLOW}
  };
  ephy_engine_session_import_permissions(regular, legacy, G_N_ELEMENTS(legacy));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();

  auto *unit_session = ephy_engine_session_new(true);
  unsigned replies = 0, finished = 0;
  auto *invalid = ephy_engine_permission_new(EPHY_ENGINE_PERMISSION_LOCATION, "data:text/html,opaque", "https://example.test", unit_session, nullptr,
    [&](auto decision){ g_assert_cmpint(decision, ==, EPHY_ENGINE_PERMISSION_DENY); ++replies; });
  g_assert_null(invalid); g_assert_cmpuint(replies, ==, 1);
  auto *unit = ephy_engine_permission_new(EPHY_ENGINE_PERMISSION_LOCATION, "https://EXAMPLE.test:443/path", "https://top.test/path", unit_session, nullptr,
    [&](auto decision){ g_assert_cmpint(decision, ==, EPHY_ENGINE_PERMISSION_ALLOW); ++replies; });
  g_assert_cmpstr(ephy_engine_permission_get_origin(unit), ==, "https://example.test");
  g_assert_true(ephy_engine_permission_is_ephemeral(unit));
  g_signal_connect(unit, "finished", G_CALLBACK(+[](EphyEnginePermission*, unsigned *count){ ++*count; }), &finished);
  g_assert_true(ephy_engine_permission_decide(unit, EPHY_ENGINE_PERMISSION_ALLOW));
  g_assert_false(ephy_engine_permission_decide(unit, EPHY_ENGINE_PERMISSION_DENY));
  ephy_engine_permission_cancel(unit); g_object_unref(unit); g_object_unref(unit_session);
  g_assert_cmpuint(replies, ==, 2); g_assert_cmpuint(finished, ==, 1);
  g_print("Native permission decisions are single-use and canonicalize origins; opaque origins cannot receive grants: PASS\n");

  Requests normal;
  const std::string child_origin = std::string("http://localhost:") + std::to_string(std::stoi(base.substr(base.rfind(':')+1)));
  auto set = [&](auto kind, auto state, const std::string& requester, const std::string& top) {
    g_assert_true(ephy_engine_session_set_permission(regular, kind, requester.c_str(), top.c_str(), state));
  };
  auto reset = [&] {
    for (auto kind : {EPHY_ENGINE_PERMISSION_MICROPHONE, EPHY_ENGINE_PERMISSION_CAMERA})
      set(kind, EPHY_ENGINE_PERMISSION_STATE_ASK, origin, origin);
    set(EPHY_ENGINE_PERMISSION_MICROPHONE, EPHY_ENGINE_PERMISSION_STATE_ASK, child_origin, origin);
  };
  auto *view = page(GTK_APPLICATION(app), regular, uri, normal);
  auto query = [&](const char *name) {
    return evaluate(view, ("(async()=> (await navigator.permissions.query({name:'" + std::string(name) + "'})).state)()").c_str());
  };
  g_assert_true(evaluate(view, "Notification.permission") == "denied");
  g_assert_true(query("geolocation") == "granted");
  g_assert_false(ephy_engine_session_set_permission(regular, EPHY_ENGINE_PERMISSION_LOCATION, "", nullptr, EPHY_ENGINE_PERMISSION_STATE_DENY));
  g_assert_false(ephy_engine_session_set_permission(regular, EPHY_ENGINE_PERMISSION_LOCATION, "data:text/html,opaque", nullptr, EPHY_ENGINE_PERMISSION_STATE_DENY));
  g_assert_false(ephy_engine_session_set_permission(regular, EPHY_ENGINE_PERMISSION_LOCATION, origin.c_str(), "not-an-origin", EPHY_ENGINE_PERMISSION_STATE_DENY));
  g_assert_false(ephy_engine_session_set_permission(regular, EPHY_ENGINE_PERMISSION_OTHER, origin.c_str(), nullptr, EPHY_ENGINE_PERMISSION_STATE_ALLOW));
  g_assert_false(ephy_engine_session_set_permission(regular, EPHY_ENGINE_PERMISSION_LOCATION, origin.c_str(), nullptr, static_cast<EphyEnginePermissionState>(2)));
  g_assert_true(query("geolocation") == "granted");
  set(EPHY_ENGINE_PERMISSION_LOCATION, EPHY_ENGINE_PERMISSION_STATE_ASK, origin, origin);
  ephy_engine_session_import_permissions(regular, legacy, G_N_ELEMENTS(legacy));
  g_assert_true(query("geolocation") == "prompt");
  set(EPHY_ENGINE_PERMISSION_NOTIFICATIONS, EPHY_ENGINE_PERMISSION_STATE_ASK, origin, origin);
  g_assert_true(query("notifications") == "prompt");
  g_print("Legacy choices import before navigation, revocations survive re-import, invalid settings cannot change defaults: PASS\n");
  auto *request = normal.ask(view, "ask({audio:true});'started'");
  g_assert_cmpint(ephy_engine_permission_get_kind(request), ==, EPHY_ENGINE_PERMISSION_MICROPHONE);
  g_assert_cmpstr(ephy_engine_permission_get_origin(request), ==, origin.c_str());
  g_assert_false(ephy_engine_permission_is_ephemeral(request));
  g_assert_true(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_DENY)); outcome(view, "NotAllowedError");
  g_assert_true(query("microphone") == "denied");
  auto count = normal.requests.size();
  evaluate(view, "ask({audio:true});'started'"); outcome(view, "NotAllowedError");
  g_assert_cmpuint(normal.requests.size(), ==, count);
  reset();
  request = normal.ask(view, "ask({audio:true,video:true});'started'");
  g_assert_cmpint(ephy_engine_permission_get_kind(request), ==, EPHY_ENGINE_PERMISSION_CAMERA_AND_MICROPHONE);
  g_assert_true(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW)); outcome(view, "allowed");
  g_assert_true(query("microphone") == "granted");
  g_assert_true(query("camera") == "granted");
  evaluate(view, "ask({audio:true,video:true});'started'"); outcome(view, "allowed");
  g_assert_cmpuint(normal.requests.size(), ==, count+1);
  g_print("Saved native media choices take effect without repeating the dialog: PASS\n");
  g_print("Actual Chromium media requests deny and allow the precise microphone/camera permissions: PASS\n");

  std::string ask_child = "outcome='pending';document.querySelector('iframe').contentWindow.postMessage('microphone','"+child_origin+"');'started'";
  wait_for([&]{ return evaluate(view, "childReady") == "true"; });
  request = normal.ask(view, ask_child.c_str());
  g_assert_cmpstr(ephy_engine_permission_get_origin(request), ==, child_origin.c_str());
  g_assert_cmpstr(ephy_engine_permission_get_top_origin(request), ==, origin.c_str());
  g_assert_true(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_DENY)); outcome(view, "NotAllowedError");
  g_print("Cross-origin frame requests carry the requesting origin separately from the top-level origin: PASS\n");
  reset();
  request = normal.ask(view, ask_child.c_str());
  evaluate(view, "document.querySelector('iframe').src='about:blank';'navigating child'");
  wait_for([&]{ return ephy_engine_permission_is_resolved(request); });
  g_assert_false(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW));
  navigate(view, uri+"?child-removal");
  wait_for([&]{ return evaluate(view, "childReady") == "true"; });
  reset();
  request = normal.ask(view, ask_child.c_str());
  evaluate(view, "document.querySelector('iframe').remove();'removed child'");
  wait_for([&]{ return ephy_engine_permission_is_resolved(request); });
  g_assert_false(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW));
  g_print("Navigating or removing a requesting subframe withdraws its pending media request: PASS\n");

  Requests private_requests;
  auto *session = ephy_engine_session_new(true);
  auto *private_view = page(GTK_APPLICATION(app), session, uri, private_requests);
  g_assert_true(evaluate(private_view, "(async()=> (await navigator.permissions.query({name:'microphone'})).state)()") == "prompt");
  g_assert_true(evaluate(private_view, "Notification.permission") == "default");
  request = private_requests.ask(private_view, "ask({audio:true});'started'");
  g_assert_true(ephy_engine_permission_is_ephemeral(request));
  g_assert_true(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_DISMISS)); outcome(private_view, "NotAllowedError");
  g_print("Private-session requests remain independent and expose their ephemeral scope: PASS\n");

  reset();
  request = normal.ask(view, "ask({audio:true});'started'");
  finished = 0;
  g_signal_connect(request, "finished", G_CALLBACK(+[](EphyEnginePermission*, unsigned *count){ ++*count; }), &finished);
  navigate(view, uri+"?next");
  wait_for([&]{ return ephy_engine_permission_is_resolved(request); });
  g_assert_false(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW));
  g_assert_cmpuint(finished, ==, 1);
  request = normal.ask(view, "ask({audio:true});'started'");
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(view))));
  g_assert_true(ephy_engine_permission_is_resolved(request));
  g_assert_false(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW));
  while (auto *windows = gtk_application_get_windows(GTK_APPLICATION(app))) gtk_window_destroy(GTK_WINDOW(windows->data));
  wait_for([]{ return Browser::active_count == 0; });
  g_print("Navigation and ordinary tab closure withdraw pending requests and reject late grants: PASS\n");
  g_object_unref(session); ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
