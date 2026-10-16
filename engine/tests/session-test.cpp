#include "diagnostic-process.hpp"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include "select/devtools.hpp"
#include <filesystem>
#include <functional>
#include <string>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);

static void wait_for(const std::function<bool()>& predicate) {
  auto deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  g_assert_true(predicate());
}
struct Async {
  GAsyncResult *result = nullptr;
  static void done(GObject*, GAsyncResult *result, gpointer user) {
    static_cast<Async*>(user)->result = G_ASYNC_RESULT(g_object_ref(result));
  }
  ~Async() { g_clear_object(&result); }
  GAsyncResult *get() { wait_for([this]{ return result; }); return result; }
};
static GList *cookies(EphyEngineSession *session, const char *url) {
  Async done;
  ephy_engine_session_get_cookies(session, url, nullptr, Async::done, &done);
  GError *error = nullptr;
  auto *list = ephy_engine_session_get_cookies_finish(session, done.get(), &error);
  g_assert_no_error(error);
  return list;
}
static SoupCookie *cookie_named(GList *list, const char *name, const char *path) {
  for (auto *item = list; item; item = item->next) {
    auto *cookie = static_cast<SoupCookie*>(item->data);
    if (!g_strcmp0(soup_cookie_get_name(cookie), name) && !g_strcmp0(soup_cookie_get_path(cookie), path)) return cookie;
  }
  return nullptr;
}
static void set_cookie(EphyEngineSession *session, const char *value, const char *path = "/") {
  auto *cookie = soup_cookie_new("native", value, "127.0.0.1", path, 3600);
  soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
  Async done;
  ephy_engine_session_add_cookie(session, cookie, nullptr, Async::done, &done);
  soup_cookie_free(cookie);
  GError *error = nullptr;
  g_assert_true(ephy_engine_session_add_cookie_finish(session, done.get(), &error));
  g_assert_no_error(error);
}
static void devtools(EphyEngineView *view, const char *method, const char *json) {
  class Reply final : public CefDevToolsMessageObserver {
  public:
    int id = next_devtools_message_id();
    bool done = false, success = false;
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int message, bool ok, const void*, size_t) override {
      if (message == id) { success = ok; done = true; }
    }
  private:
    IMPLEMENT_REFCOUNTING(Reply);
  };
  CefRefPtr<Reply> reply = new Reply;
  auto browser = ephy_engine_view_browser(view);
  auto observer = browser->GetHost()->AddDevToolsMessageObserver(reply);
  g_assert_true(send_devtools_method(browser, reply->id, method,
    CefParseJSON(json, JSON_PARSER_RFC)->GetDictionary()));
  wait_for([&]{ return reply->done; });
  g_assert_true(reply->success);
}
static std::string evaluate(EphyEngineView *view, const char *script) {
  Async done;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr, Async::done, &done);
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, done.get(), &error);
  g_assert_no_error(error);
  g_assert_nonnull(value);
  char *text = g_strdup(script_result_text(value).c_str());
  std::string result(text);
  g_free(text); json_object_unref(value);
  return result;
}
static EphyEngineView *page(GtkApplication *app, EphyEngineSession *session, const char *url) {
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, url);
  gtk_window_present(GTK_WINDOW(window));
  wait_for([&]{ return loaded; });
  g_signal_handler_disconnect(view, handler);
  return view;
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv);
  if (sub >= 0) return sub;
  bool child = argc == 3 && !strcmp(argv[1], "--check-persistence");
  auto *app = adw_application_new("io.github.doromiert.Jupiter.SessionTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error));
  g_assert_no_error(error);
  char scratch[] = "/tmp/jupiter-session-test-XXXXXX";
  const char *profile = child ? argv[2] : mkdtemp(scratch);
  g_assert_nonnull(profile);
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true));
  disable_diagnostic_core_dumps();
  auto *normal = ephy_engine_session_get_default();
  const char *url = g_getenv("JUPITER_TEST_URL");
  g_assert_nonnull(url);
  if (child) {
    auto *list = cookies(normal, url);
    auto *cookie = cookie_named(list, "native", "/");
    g_assert_nonnull(cookie);
    g_assert_cmpstr(soup_cookie_get_value(cookie), ==, "persisted");
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    g_print("CEF cookie persistence across processes: PASS\n");
  } else {
    set_cookie(normal, "persisted");
    auto *view = page(GTK_APPLICATION(app), normal, url);
    auto text = evaluate(view, "fetch('/echo-cookie').then(r=>r.text())");
    g_assert_nonnull(strstr(text.c_str(), "native=persisted"));
    g_print("Native cookie API and Chromium HTTP requests share a store: PASS\n");

    auto *private_session = ephy_engine_session_new(true);
    auto *isolated_session = ephy_engine_session_new(true);
    auto *list = cookies(private_session, url);
    g_assert_null(list);
    set_cookie(private_session, "private");
    auto *private_view = page(GTK_APPLICATION(app), private_session, url);
    auto *private_view2 = page(GTK_APPLICATION(app), private_session, url);
    for (auto *page : {private_view, private_view2}) {
      auto text = evaluate(page, "fetch('/echo-cookie').then(r=>r.text())");
      g_assert_nonnull(strstr(text.c_str(), "native=private"));
      g_assert_null(strstr(text.c_str(), "persisted"));
    }
    list = cookies(isolated_session, url);
    g_assert_null(list);
    g_print("Private tabs share their session and isolate other sessions: PASS\n");

    set_cookie(private_session, "nested", "/nested");
    std::string nested = std::string(url) + "nested/page";
    list = cookies(private_session, nested.c_str());
    auto *cookie = cookie_named(list, "native", "/nested");
    g_assert_nonnull(cookie);
    Async removed;
    ephy_engine_session_delete_cookie(private_session, cookie, nullptr, Async::done, &removed);
    g_assert_true(ephy_engine_session_delete_cookie_finish(private_session, removed.get(), &error));
    g_assert_no_error(error);
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    list = cookies(private_session, nested.c_str());
    g_assert_null(cookie_named(list, "native", "/nested"));
    g_assert_nonnull(cookie_named(list, "native", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    g_print("Cookie removal preserves cookies with another path: PASS\n");

    ephy_engine_session_set_storage_enabled(private_session, false);
    text = evaluate(private_view, "fetch('/echo-cookie').then(r=>r.text())");
    g_assert_null(strstr(text.c_str(), "native="));
    evaluate(private_view, "document.cookie='blocked=1;SameSite=Lax;Path=/';document.cookie");
    evaluate(private_view, "fetch('/set-cookie').then(r=>r.text())");
    list = cookies(private_session, url);
    g_assert_null(cookie_named(list, "blocked", "/"));
    g_assert_null(cookie_named(list, "networkCookie", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    ephy_engine_session_set_storage_enabled(private_session, true);
    evaluate(private_view, "fetch('/set-cookie').then(r=>r.text())");
    list = cookies(private_session, url);
    g_assert_nonnull(cookie_named(list, "networkCookie", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    g_print("Storage policy blocks cookies in actual Chromium traffic and JavaScript: PASS\n");

    auto *cancel = g_cancellable_new();
    g_cancellable_cancel(cancel);
    Async cancelled;
    ephy_engine_session_get_cookies(normal, url, cancel, Async::done, &cancelled);
    g_assert_null(ephy_engine_session_get_cookies_finish(normal, cancelled.get(), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error); g_object_unref(cancel);
    g_print("Cookie task cancellation: PASS\n");

    const char *cached_request = "fetch('/cache-fixture').then(r=>r.text())";
    auto normal_cached = evaluate(view, cached_request);
    g_assert_true(evaluate(view, cached_request) == normal_cached);
    auto private_cached = evaluate(private_view, cached_request);
    g_assert_true(private_cached != normal_cached);
    g_assert_true(evaluate(private_view2, cached_request) == private_cached);
    auto *cache_cancel = g_cancellable_new();
    g_cancellable_cancel(cache_cancel);
    Async cache_cancelled;
    ephy_engine_session_clear_cache(normal, cache_cancel, Async::done, &cache_cancelled);
    g_assert_false(ephy_engine_session_clear_cache_finish(normal, cache_cancelled.get(), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error); g_object_unref(cache_cancel);
    g_assert_true(evaluate(view, cached_request) == normal_cached);
    Async normal_cache_clear;
    ephy_engine_session_clear_cache(normal, nullptr, Async::done, &normal_cache_clear);
    g_assert_true(ephy_engine_session_clear_cache_finish(normal, normal_cache_clear.get(), &error));
    g_assert_no_error(error);
    auto normal_refreshed = evaluate(view, cached_request);
    g_assert_true(normal_refreshed != normal_cached);
    g_assert_true(evaluate(private_view, cached_request) == private_cached);
    Async private_cache_clear;
    ephy_engine_session_clear_cache(private_session, nullptr, Async::done, &private_cache_clear);
    g_assert_true(ephy_engine_session_clear_cache_finish(private_session, private_cache_clear.get(), &error));
    g_assert_no_error(error);
    g_assert_true(evaluate(private_view2, cached_request) != private_cached);
    g_assert_true(evaluate(view, cached_request) == normal_refreshed);
    list = cookies(normal, url);
    g_assert_nonnull(cookie_named(list, "native", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    g_print("Real HTTP cache clearing, cancellation, and regular/private isolation preserve cookies: PASS\n");

    // Two CHIPS partitions and an unpartitioned cookie share a domain/name/path.
    for (const char *site : {"https://top-a.test", "https://top-b.test"}) {
      std::string json = std::string("{\"name\":\"partitioned\",\"value\":\"chip\",\"url\":\"https://partitioned.test/\",\"secure\":true,\"sameSite\":\"None\",\"partitionKey\":{\"topLevelSite\":\"") + site + "\",\"hasCrossSiteAncestor\":true}}";
      devtools(view, "Network.setCookie", json.c_str());
    }
    devtools(view, "Network.setCookie", R"({"name":"partitioned","value":"plain","url":"https://partitioned.test/","secure":true,"httpOnly":true})");
    devtools(view, "Network.setCookie", R"({"name":"keep","value":"other-domain","url":"https://unrelated.test/","secure":true})");
    list = cookies(normal, nullptr);
    guint partitioned = 0;
    for (auto *l = list; l; l = l->next)
      if (!strcmp(soup_cookie_get_domain(static_cast<SoupCookie*>(l->data)), "partitioned.test")) ++partitioned;
    g_assert_cmpuint(partitioned, ==, 3);
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    Async invalid_domain;
    ephy_engine_session_clear_cookies(normal, "", nullptr, Async::done, &invalid_domain);
    g_assert_false(ephy_engine_session_clear_cookies_finish(normal, invalid_domain.get(), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    auto *cookie_cancel = g_cancellable_new();
    g_cancellable_cancel(cookie_cancel);
    Async cookie_clear_cancelled;
    ephy_engine_session_clear_cookies(normal, nullptr, cookie_cancel, Async::done, &cookie_clear_cancelled);
    g_assert_false(ephy_engine_session_clear_cookies_finish(normal, cookie_clear_cancelled.get(), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error); g_object_unref(cookie_cancel);
    list = cookies(normal, "https://partitioned.test/");
    g_assert_nonnull(cookie_named(list, "partitioned", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    Async cookie_group;
    ephy_engine_session_clear_cookies(normal, "partitioned.test", nullptr, Async::done, &cookie_group);
    g_assert_true(ephy_engine_session_clear_cookies_finish(normal, cookie_group.get(), &error));
    g_assert_no_error(error);
    list = cookies(normal, nullptr);
    g_assert_null(cookie_named(list, "partitioned", "/"));
    g_assert_nonnull(cookie_named(list, "keep", "/"));
    g_assert_nonnull(cookie_named(list, "native", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    Async private_cookies;
    ephy_engine_session_clear_cookies(private_session, nullptr, nullptr, Async::done, &private_cookies);
    g_assert_true(ephy_engine_session_clear_cookies_finish(private_session, private_cookies.get(), &error));
    g_assert_no_error(error);
    g_assert_null(cookies(private_session, nullptr));
    g_assert_true(evaluate(private_view2, "document.cookie").empty());
    list = cookies(normal, url);
    g_assert_nonnull(cookie_named(list, "native", "/"));
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    g_print("Cookie domain clearing includes CHIPS and HttpOnly cookies, preserves other domains, cancellation and private isolation: PASS\n");

    auto *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    while (windows) {
      gtk_window_destroy(GTK_WINDOW(windows->data));
      windows = gtk_application_get_windows(GTK_APPLICATION(app));
    }
    wait_for([]{ return Browser::active_count == 0; });
    g_object_unref(private_session); g_object_unref(isolated_session);
  }
  ephy_engine_shutdown();
  g_object_unref(app);
  if (!child) {
    char *child_args[] = {argv[0], const_cast<char*>("--check-persistence"), const_cast<char*>(profile), nullptr};
    int status;
    g_assert_true(g_spawn_sync(nullptr, child_args, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, nullptr, &status, &error));
    g_assert_no_error(error);
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    std::filesystem::remove_all(profile);
  }
  return 0;
}
