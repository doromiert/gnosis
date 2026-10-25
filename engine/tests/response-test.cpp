/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include <filesystem>
#include <cstring>
#include <functional>
#include <map>
#include <vector>
#include <source_location>
#include <string>

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  if (!predicate())
    g_error("Timed out at %s:%u (%s)", location.file_name(), location.line(), location.function_name());
}
static void navigate(EphyEngineView *view, const std::string& uri,
                     const std::source_location location = std::source_location::current()) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, uri.c_str());
  wait_for([&]{ return loaded; }, location);
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
// These gates hold actual HTTP responses in the fixture server. Keepalive lets
// the old request finish after navigation, rather than "passing" by never
// starting a request or cancelling it before the response hook can run.
struct ResponseCompletions {
  std::map<std::string, std::vector<std::string>> uris;
  unsigned count(const std::string& key) { return uris[key].size(); }
  static void received(EphyEngineView *, const char *uri, EphyEngineResponse *response,
                       ResponseCompletions *self) {
    if (!strstr(uri, "/response-hold")) return;
    const char *query = strstr(uri, "?case=");
    g_assert_nonnull(query);
    std::string key(query + 6);
    if (auto end = key.find('&'); end != std::string::npos) key.resize(end);
    g_assert_cmpstr(ephy_engine_response_get_uri(response), ==, uri);
    g_assert_cmpuint(ephy_engine_response_get_status_code(response), ==, 200);
    self->uris[key].push_back(uri);
  }
};
struct HeldResponse {
  std::filesystem::path marker;
  std::string key, uri;
  HeldResponse(const std::string& base, const char *name, bool redirect = false) : key(name) {
    const char *directory = g_getenv("JUPITER_RESPONSE_FIXTURE_DIR");
    g_assert_nonnull(directory);
    marker = std::filesystem::path(directory) / key;
    uri = base + "response-hold?case=" + key + (redirect ? "&redirect=1" : "");
  }
  std::filesystem::path file(const char *suffix) const { return marker.string() + suffix; }
  void wait_started() { wait_for([&] { return std::filesystem::exists(file(".started")); }); }
  void fetch(EphyEngineView *view, bool child = false) {
    const auto script = std::string(child ? "document.querySelector('iframe').contentWindow." : "") +
      "fetch('" + uri + "',{keepalive:true}).catch(()=>{});'started'";
    evaluate(view, script.c_str());
    wait_started();
  }
  void release() { g_assert_true(g_file_set_contents(file(".release").c_str(), "release", -1, nullptr)); }
  void finished(bool must_send = true) {
    wait_for([&] { return std::filesystem::exists(file(".finished")); });
    g_autofree char *contents = nullptr;
    g_assert_true(g_file_get_contents(file(".finished").c_str(), &contents, nullptr, nullptr));
    if (must_send) g_assert_cmpstr(contents, ==, "sent");
  }
};
static void settle_responses() {
  const auto deadline = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
}
static void assert_resource_lifetimes(EphyEngineView *view, const std::string& base,
                                     ResponseCompletions& completions) {
  navigate(view, base + "response-document");
  HeldResponse positive(base, "current-document");
  positive.fetch(view); positive.release(); positive.finished();
  wait_for([&] { return completions.count(positive.key) == 1; });

  HeldResponse redirect(base, "current-redirect", true);
  ephy_engine_web_view_load_uri(view, redirect.uri.c_str());
  redirect.wait_started(); redirect.release(); redirect.finished();
  wait_for([&] { return completions.count(redirect.key) == 1; });
  g_assert_true(completions.uris[redirect.key].front().find("/response-hold-final?") != std::string::npos);
  g_print("Native resource completion accepts real released responses and same-chain main-frame redirects: PASS\n");

  navigate(view, base + "response-document");
  HeldResponse old_main(base, "old-main");
  old_main.fetch(view);
  navigate(view, base + "response-missing");
  old_main.release(); old_main.finished(); settle_responses();
  g_assert_cmpuint(completions.count(old_main.key), ==, 0);

  navigate(view, base + "response-document");
  HeldResponse old_redirect(base, "old-redirect", true);
  old_redirect.fetch(view);
  navigate(view, base + "response-missing");
  old_redirect.release(); old_redirect.finished(); settle_responses();
  g_assert_cmpuint(completions.count(old_redirect.key), ==, 0);
  g_print("Old-document keepalive completions and late redirects cannot reach the replacement page: PASS\n");

  navigate(view, base + "response-document");
  HeldResponse old_child(base, "old-subframe");
  old_child.fetch(view, true);
  evaluate(view, "document.querySelector('iframe').src='/response-child?replacement=1';'navigating'");
  wait_for([&] { return evaluate(view, "document.querySelector('iframe').contentWindow.location.search") == "?replacement=1"; });
  old_child.release(); old_child.finished(); settle_responses();
  g_assert_cmpuint(completions.count(old_child.key), ==, 0);
  HeldResponse current_child(base, "current-subframe");
  current_child.fetch(view, true); current_child.release(); current_child.finished();
  wait_for([&] { return completions.count(current_child.key) == 1; });
  g_print("Subframe navigation rejects the previous document while accepting its replacement's real response: PASS\n");

  HeldResponse hidden(base, "hidden-view");
  hidden.fetch(view);
  gtk_widget_set_visible(GTK_WIDGET(view), false);
  wait_for([&] { return !gtk_widget_get_mapped(GTK_WIDGET(view)); });
  hidden.release(); hidden.finished(); settle_responses();
  g_assert_cmpuint(completions.count(hidden.key), ==, 0);
  gtk_widget_set_visible(GTK_WIDGET(view), true);
  wait_for([&] { return gtk_widget_get_mapped(GTK_WIDGET(view)); });
  HeldResponse shown(base, "shown-view");
  shown.fetch(view); shown.release(); shown.finished();
  wait_for([&] { return completions.count(shown.key) == 1; });
  g_print("Hidden-view resource completion is invalidated; new requests after showing remain functional: PASS\n");
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
  unsigned child_responses = 0;
  auto resource_response_handler = g_signal_connect(view, "resource-response",
    G_CALLBACK(+[](EphyEngineView *, const char *uri, EphyEngineResponse *response, unsigned *count) {
      if (!g_str_has_suffix(uri, "/response-child")) return;
      g_assert_cmpstr(ephy_engine_response_get_uri(response), ==, uri);
      g_assert_cmpuint(ephy_engine_response_get_status_code(response), ==, 200);
      g_assert_cmpstr(ephy_engine_response_get_mime_type(response), ==, "text/html");
      ++*count;
    }), &child_responses);
  navigate(view, base + "response-redirect");
  wait_for([&] { return child_responses > 0; });
  g_print("Native subresource response signal reports the actual completed iframe response: PASS\n");
  g_signal_handler_disconnect(view, resource_response_handler);
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
  ResponseCompletions completions;
  g_signal_connect(view, "resource-response", G_CALLBACK(ResponseCompletions::received), &completions);
  assert_resource_lifetimes(view, base, completions);
  navigate(view, "about:blank");
  g_assert_cmpstr(ephy_engine_web_view_get_uri(view), ==, "about:blank");
  auto *blank = ephy_engine_web_view_get_main_resource(view);
  g_assert_null(ephy_engine_web_resource_get_response(blank));
  g_assert_cmpstr(ephy_engine_web_resource_get_uri(blank), ==, "about:blank");
  navigate(view, base + "response-document");
  HeldResponse closing(base, "closed-view");
  closing.fetch(view);
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
  closing.release(); closing.finished(false);
  wait_for([&]{ return Browser::active_count == 0 && callbacks == 8; });
  settle_responses();
  g_assert_cmpuint(completions.count(closing.key), ==, 0);
  g_print("Closing the owning view rejects a pending real HTTP resource completion: PASS\n");
  g_print("Queued JavaScript replies complete once when a view closes before GTK delivery: PASS\n");
  assert_closed(missing);
  g_assert_cmpuint(ephy_engine_response_get_status_code(ephy_engine_web_resource_get_response(missing)), ==, 404);
  g_object_unref(resource); g_object_unref(missing);
  ephy_engine_shutdown(); g_object_unref(app);
  std::filesystem::remove_all(profile);
  g_print("Resource metadata survives tab destruction; closed reads fail safely: PASS\n");
}
