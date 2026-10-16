/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include <filesystem>
#include <functional>
#include <string>

static void wait_for(const std::function<bool()>& predicate) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  g_assert_true(predicate());
}
static void navigate(EphyEngineView *view, const std::string& uri) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, uri.c_str());
  wait_for([&]{ return loaded; });
  g_signal_handler_disconnect(view, handler);
}
static void loaded_after(EphyEngineView *view, const std::function<void()>& action) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  action();
  wait_for([&]{ return loaded; });
  g_signal_handler_disconnect(view, handler);
}
static std::string evaluate(EphyEngineView *view, const char *script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject*, GAsyncResult *result, gpointer user) {
      *static_cast<GAsyncResult**>(user) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&]{ return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  char *text = g_strdup(script_result_text(value).c_str());
  std::string output(text);
  g_free(text); json_object_unref(value); g_object_unref(result);
  return output;
}
static void assert_closed(EphyEngineResource *resource) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_resource_get_data(resource, nullptr, +[](GObject*, GAsyncResult *result, gpointer user) {
    *static_cast<GAsyncResult**>(user) = G_ASYNC_RESULT(g_object_ref(result));
  }, &result);
  wait_for([&]{ return result; });
  GError *error = nullptr;
  gsize length;
  auto *data = ephy_engine_web_resource_get_data_finish(resource, result, &length, &error);
  g_assert_null(data);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  g_clear_error(&error); g_object_unref(result);
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.ResponseTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error));
  g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-response-test-XXXXXX";
  g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true));
  disable_diagnostic_core_dumps();
  auto raw = CefResponse::Create();
  raw->SetStatus(201);
  raw->SetMimeType("application/octet-stream");
  raw->SetHeaderByName("Content-Disposition", "inline; filename=\"../../report.bin\"", true);
  auto *snapshot = ephy_engine_response_new("https://example.test/file", raw);
  g_assert_cmpstr(ephy_engine_response_get_suggested_filename(snapshot), ==, "report.bin");
  g_assert_cmpint(ephy_engine_response_get_content_length(snapshot), ==, -1);
  raw->SetStatus(500);
  g_assert_cmpuint(ephy_engine_response_get_status_code(snapshot), ==, 201);
  g_object_unref(snapshot);
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  gtk_window_present(GTK_WINDOW(window));
  const std::string base = g_getenv("JUPITER_TEST_URL");
  const auto document_uri = base + "response-document";
  const auto missing_uri = base + "response-missing";
  navigate(view, base + "response-redirect");
  auto *resource = ephy_engine_web_view_get_main_resource(view);
  g_object_ref(resource);
  auto *response = ephy_engine_web_resource_get_response(resource);
  g_assert_nonnull(response);
  g_assert_cmpstr(ephy_engine_response_get_uri(response), ==, document_uri.c_str());
  g_assert_cmpstr(ephy_engine_web_resource_get_uri(resource), ==, document_uri.c_str());
  g_assert_cmpstr(ephy_engine_response_get_mime_type(response), ==, "text/html");
  g_assert_cmpuint(ephy_engine_response_get_status_code(response), ==, 200);
  g_assert_cmpint(ephy_engine_response_get_content_length(response), >, 0);
  g_assert_cmpstr(ephy_engine_response_get_suggested_filename(response), ==, "report_é.html");
  g_assert_cmpstr(soup_message_headers_get_one(ephy_engine_response_get_http_headers(response), "X-Jupiter-Fixture"), ==, "actual-response");
  g_print("CEF response metadata follows redirects, preserves headers and UTF-8 filenames, excludes subframe responses: PASS\n");
  navigate(view, base + "response-missing");
  auto *missing = ephy_engine_web_view_get_main_resource(view);
  g_assert_true(missing != resource);
  response = ephy_engine_web_resource_get_response(missing);
  g_assert_nonnull(response);
  g_assert_cmpuint(ephy_engine_response_get_status_code(response), ==, 404);
  g_assert_cmpstr(ephy_engine_response_get_uri(response), ==, missing_uri.c_str());
  assert_closed(resource);
  g_print("HTTP errors retain their actual response and old resources cannot read a replacement document: PASS\n");
  g_object_ref(missing);
  bool restored = false;
  auto restored_handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *restored) {
    if (event == WEBKIT_LOAD_FINISHED) *restored = true;
  }), &restored);
  ephy_engine_web_view_go_back(view);
  wait_for([&]{ return restored; });
  g_signal_handler_disconnect(view, restored_handler);
  auto *back_response = ephy_engine_web_resource_get_response(ephy_engine_web_view_get_main_resource(view));
  g_assert_nonnull(back_response);
  g_assert_cmpstr(ephy_engine_response_get_uri(back_response), ==, document_uri.c_str());
  g_assert_cmpuint(ephy_engine_response_get_status_code(back_response), ==, 200);
  g_print("History navigation restores actual response metadata: PASS\n");
  auto uri_handler = g_signal_connect(view, "notify::uri", G_CALLBACK(+[](EphyEngineView *view, GParamSpec*, gpointer) {
    g_assert_false(g_str_has_prefix(ephy_engine_web_view_get_uri(view), "data:"));
  }), nullptr);
  loaded_after(view, [&]{ ephy_engine_web_view_load_alternate_html(view, "<head><title>Saved page</title></head>", document_uri.c_str(), nullptr); });
  g_assert_cmpstr(ephy_engine_web_view_get_uri(view), ==, document_uri.c_str());
  g_assert_cmpstr(ephy_engine_web_resource_get_uri(ephy_engine_web_view_get_main_resource(view)), ==, document_uri.c_str());
  auto origin = evaluate(view, "location.origin");
  g_assert_cmpstr(origin.c_str(), ==, "null");
  auto *saved = ephy_engine_web_view_get_session_state(view);
  auto *bytes = ephy_engine_web_view_session_state_serialize(saved);
  auto *state = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("(ss)"), bytes, false));
  const char *saved_uri, *saved_title;
  g_variant_get(state, "(&s&s)", &saved_uri, &saved_title);
  g_assert_cmpstr(saved_uri, ==, document_uri.c_str());
  g_assert_cmpstr(saved_title, ==, "Saved page");
  g_variant_unref(state); g_bytes_unref(bytes);
  auto *item = ephy_engine_back_forward_list_get_current_item(ephy_engine_web_view_get_back_forward_list(view));
  g_assert_cmpstr(ephy_engine_back_forward_list_item_get_uri(item), ==, document_uri.c_str());
  loaded_after(view, [&]{ ephy_engine_web_view_go_to_back_forward_list_item(view, item); });
  auto title = evaluate(view, "document.title");
  g_assert_cmpstr(title.c_str(), ==, "Response metadata");
  // Identical placeholder HTML for another tab must retain its own destination.
  loaded_after(view, [&]{ ephy_engine_web_view_load_alternate_html(view, "<head><title>Saved page</title></head>", missing_uri.c_str(), nullptr); });
  g_assert_cmpstr(ephy_engine_web_view_get_uri(view), ==, missing_uri.c_str());
  ephy_engine_web_view_restore_session_state(view, saved);
  item = ephy_engine_back_forward_list_get_current_item(ephy_engine_web_view_get_back_forward_list(view));
  g_assert_cmpstr(ephy_engine_back_forward_list_item_get_uri(item), ==, document_uri.c_str());
  loaded_after(view, [&]{ ephy_engine_web_view_go_to_back_forward_list_item(view, item); });
  title = evaluate(view, "document.title");
  g_assert_cmpstr(title.c_str(), ==, "Response metadata");
  g_assert_cmpstr(ephy_engine_web_view_get_uri(view), ==, document_uri.c_str());
  ephy_engine_web_view_session_state_unref(saved);
  loaded_after(view, [&]{ ephy_engine_web_view_load_alternate_html(view, "<title>Error page</title>", missing_uri.c_str(), nullptr); });
  loaded_after(view, [&]{ ephy_engine_web_view_reload_bypass_cache(view); });
  title = evaluate(view, "document.title");
  g_assert_cmpstr(title.c_str(), ==, "Response metadata");
  g_signal_handler_disconnect(view, uri_handler);
  const char *user_data = "data:text/html,<title>User data URL</title>";
  navigate(view, user_data);
  g_assert_cmpstr(ephy_engine_web_view_get_uri(view), ==, user_data);
  g_print("Alternate pages keep opaque origins and logical addresses; saved placeholders restore and reload real destinations; user data URLs remain visible: PASS\n");
  navigate(view, "about:blank");
  auto *blank = ephy_engine_web_view_get_main_resource(view);
  g_assert_null(ephy_engine_web_resource_get_response(blank));
  g_assert_cmpstr(ephy_engine_web_resource_get_uri(blank), ==, "about:blank");
  unsigned callbacks = 0;
  for (int i = 0; i < 8; ++i)
    ephy_engine_web_view_evaluate_javascript(view, "Promise.resolve('closing')", -1, nullptr, nullptr, nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer user) {
        GError *error = nullptr;
        auto *value = ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(source), result, &error);
        g_clear_pointer(&value,json_object_unref); g_clear_error(&error);
        ++*static_cast<unsigned*>(user);
      }, &callbacks);
  // Queue CEF replies without dispatching GTK's deferred deliveries, then close.
  for (int i = 0; i < 50; ++i) { CefDoMessageLoopWork(); g_usleep(1000); }
  g_object_ref(view);
  adw_application_window_set_content(window, nullptr);
  g_object_run_dispose(G_OBJECT(view));
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(view);
  wait_for([&]{ return Browser::active_count == 0 && callbacks == 8; });
  g_print("Queued JavaScript replies complete once when a view closes before GTK delivery: PASS\n");
  assert_closed(missing);
  g_assert_cmpuint(ephy_engine_response_get_status_code(ephy_engine_web_resource_get_response(missing)), ==, 404);
  g_object_unref(resource); g_object_unref(missing);
  ephy_engine_shutdown(); g_object_unref(app);
  std::filesystem::remove_all(profile);
  g_print("Resource metadata survives tab destruction; closed reads fail safely: PASS\n");
}
