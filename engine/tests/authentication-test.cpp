/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include "include/cef_callback.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView *);
static constexpr const char *username = "jupiter-user", *password = "fixture-password";
static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("HTTP authentication timed out at %s:%u", location.file_name(), location.line());
}
static std::string evaluate(EphyEngineView *view, const char *script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject *, GAsyncResult *result, gpointer user) {
      *static_cast<GAsyncResult **>(user) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  auto text = script_result_text(value);
  json_object_unref(value); g_object_unref(result);
  return text;
}
static void clear_authentication(EphyEngineSession *session) {
  class Done final : public CefCompletionCallback {
  public:
    bool done = false;
    void OnComplete() override { done = true; }
  private:
    IMPLEMENT_REFCOUNTING(Done);
  };
  CefRefPtr<Done> done = new Done;
  ephy_engine_session_context(session)->ClearHttpAuthCredentials(done);
  wait_for([&] { return done->done; });
}
static void set_fixture_proxy(EphyEngineSession *session, int port) {
  g_assert_cmpint(port, >, 0); g_assert_cmpint(port, <=, 65535);
  auto context = ephy_engine_session_context(session); g_assert_nonnull(context.get());
  auto settings = CefValue::Create(); auto dict = CefDictionaryValue::Create();
  dict->SetString("mode", "fixed_servers"); dict->SetString("server", "127.0.0.1:" + std::to_string(port));
  dict->SetString("bypass_list", "<-loopback>"); settings->SetDictionary(dict);
  CefString error;
  g_assert_true(context->SetPreference("proxy", settings, error));
  g_assert_true(error.empty());
  auto actual = context->GetPreference("proxy");
  g_assert_nonnull(actual.get()); g_assert_true(actual->IsEqual(settings));
}
struct Request {
  EphyEngineAuthentication *request;
  EphyEngineView *view;
  unsigned finished = 0, cancelled = 0;
  gulong finish_handler, cancel_handler;
  ~Request() {
    g_signal_handler_disconnect(request, finish_handler);
    g_signal_handler_disconnect(ephy_engine_authentication_get_cancellable(request), cancel_handler);
    g_object_unref(request);
  }
  void terminal() {
    g_assert_true(ephy_engine_authentication_is_resolved(request));
    g_assert_true(g_cancellable_is_cancelled(ephy_engine_authentication_get_cancellable(request)));
    g_assert_cmpuint(finished, ==, 1); g_assert_cmpuint(cancelled, ==, 1);
    g_assert_false(ephy_engine_authentication_cancel(request));
    g_assert_false(ephy_engine_authentication_authenticate(request, username, password));
    g_assert_cmpuint(finished, ==, 1); g_assert_cmpuint(cancelled, ==, 1);
  }
};
struct Requests {
  std::vector<std::unique_ptr<Request>> entries;
  static gboolean received(EphyEngineView *view, EphyEngineAuthentication *request, Requests *self) {
    auto record = std::make_unique<Request>();
    record->request = EPHY_ENGINE_AUTHENTICATION(g_object_ref(request)); record->view = view;
    record->finish_handler = g_signal_connect(request, "finished", G_CALLBACK(+[](EphyEngineAuthentication *, Request *r) {
      ++r->finished;
    }), record.get());
    record->cancel_handler = g_signal_connect(ephy_engine_authentication_get_cancellable(request), "cancelled",
      G_CALLBACK(+[](GCancellable *, Request *r) { ++r->cancelled; }), record.get());
    g_print("Native challenge: host=%s port=%d realm=%s scheme=%s proxy=%d private=%d\n",
      ephy_engine_authentication_get_host(request), ephy_engine_authentication_get_port(request),
      ephy_engine_authentication_get_realm(request), ephy_engine_authentication_get_scheme(request),
      ephy_engine_authentication_is_proxy(request), ephy_engine_authentication_is_ephemeral(request));
    self->entries.push_back(std::move(record));
    return true;
  }
  Request& next(EphyEngineView *view, const std::string& uri) {
    const auto count = entries.size();
    ephy_engine_web_view_load_uri(view, uri.c_str());
    wait_for([&] { return entries.size() > count; });
    g_assert_cmpuint(entries.size(), ==, count + 1);
    g_assert_true(entries.back()->view == view);
    return *entries.back();
  }
};
struct Page {
  AdwApplicationWindow *window;
  EphyEngineView *view;
  bool loaded = false;
  static std::unique_ptr<Page> create(GtkApplication *app, EphyEngineSession *session, Requests *requests) {
    auto page = std::make_unique<Page>();
    page->window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    page->view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
    g_object_ref_sink(page->view);
    adw_application_window_set_content(page->window, GTK_WIDGET(page->view));
    if (requests) g_signal_connect(page->view, "authentication-request", G_CALLBACK(Requests::received), requests);
    g_signal_connect(page->view, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, Page *page) {
      if (event == WEBKIT_LOAD_FINISHED) page->loaded = true;
    }), page.get());
    ephy_engine_web_view_load_uri(page->view, "about:blank");
    gtk_window_present(GTK_WINDOW(page->window));
    wait_for([&] { return page->loaded; });
    return page;
  }
  void navigate(const std::string& uri) {
    loaded = false; ephy_engine_web_view_load_uri(view, uri.c_str());
    wait_for([&] { return loaded; });
  }
  void authenticated(const std::string& realm) {
    wait_for([&] { return !ephy_engine_web_view_is_loading(view); });
    g_assert_true(evaluate(view, "document.querySelector('#authenticated')?.textContent || ''") == "Authenticated " + realm);
  }
  ~Page() { if (window) gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(view); }
};
static void metadata(Request& request, const std::string& origin, const char *realm,
                     const char *scheme, bool proxy, bool ephemeral, int port) {
  auto *r = request.request;
  g_assert_cmpstr(ephy_engine_authentication_get_origin_url(r), ==, origin.c_str());
  g_assert_cmpstr(ephy_engine_authentication_get_host(r), ==, "127.0.0.1");
  g_assert_cmpint(ephy_engine_authentication_get_port(r), ==, port);
  g_assert_cmpstr(ephy_engine_authentication_get_realm(r), ==, realm);
  g_assert_cmpstr(ephy_engine_authentication_get_scheme(r), ==, scheme);
  g_assert_cmpint(ephy_engine_authentication_is_proxy(r), ==, proxy);
  g_assert_cmpint(ephy_engine_authentication_is_ephemeral(r), ==, ephemeral);
  g_assert_false(ephy_engine_authentication_is_resolved(r));
}
static bool server_observed(const std::string& base, const char *realm, bool authorized, const char *scheme) {
  auto *session = soup_session_new();
  auto *message = soup_message_new("GET", (base + "settings-reports").c_str());
  GError *error = nullptr;
  auto *bytes = soup_session_send_and_read(session, message, nullptr, &error);
  g_assert_no_error(error); g_assert_nonnull(bytes);
  g_autoptr(JsonParser) parser = json_parser_new();
  gsize length; const char *body = static_cast<const char *>(g_bytes_get_data(bytes, &length));
  g_assert_true(json_parser_load_from_data(parser, body, length, &error)); g_assert_no_error(error);
  auto *array = json_node_get_array(json_parser_get_root(parser)); bool found = false;
  for (guint i = 0; i < json_array_get_length(array); ++i) {
    auto *record = json_array_get_object_element(array, i);
    if (json_object_has_member(record, "httpAuth") &&
        !g_strcmp0(json_object_get_string_member(record, "realm"), realm) &&
        json_object_get_boolean_member(record, "authorized") == authorized &&
        !g_strcmp0(json_object_get_string_member(record, "authorizationScheme"), scheme)) found = true;
  }
  g_bytes_unref(bytes); g_object_unref(message); g_object_unref(session);
  return found;
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.AuthenticationTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char scratch[] = "/tmp/jupiter-authentication-test-XXXXXX";
  std::string prepared_profile;
  constexpr const char *prepare_option = "--prepare-proxy-profile=";
  for (int i = 1; i < argc; ++i) {
    if (g_str_has_prefix(argv[i], prepare_option)) {
      g_assert_true(prepared_profile.empty());
      prepared_profile = argv[i] + strlen(prepare_option);
      g_assert_true(g_path_is_absolute(prepared_profile.c_str()));
    }
  }
  const char *profile = prepared_profile.empty() ? mkdtemp(scratch) : prepared_profile.c_str();
  g_assert_nonnull(profile);
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  const char *fixture = g_getenv("JUPITER_TEST_URL"); g_assert_nonnull(fixture);
  std::string base = fixture;
  g_autoptr(GUri) fixture_uri = g_uri_parse(fixture, G_URI_FLAGS_NONE, &error); g_assert_no_error(error);
  const int port = g_uri_get_port(fixture_uri);
  auto *normal_session = ephy_engine_session_get_default();
  if (!prepared_profile.empty()) {
    wait_for([&] { return bool(ephy_engine_session_context(normal_session)); });
    set_fixture_proxy(normal_session, port);
    g_assert_true(ephy_engine_prepare_shutdown());
    ephy_engine_shutdown(); g_object_unref(app);
    g_print("Native proxy preference set and verified; isolated profile preserved: %s\n", profile);
    return 0;
  }
  Requests requests;
  auto normal = Page::create(GTK_APPLICATION(app), normal_session, &requests);

  auto uri = base + "auth/basic?realm=basic-retry";
  auto& wrong = requests.next(normal->view, uri);
  metadata(wrong, uri, "basic-retry", "basic", false, false, port);
  const auto before_retry = requests.entries.size();
  g_assert_true(ephy_engine_authentication_authenticate(wrong.request, username, "incorrect")); wrong.terminal();
  wait_for([&] { return requests.entries.size() > before_retry; });
  auto& retry = *requests.entries.back();
  metadata(retry, uri, "basic-retry", "basic", false, false, port);
  g_assert_true(ephy_engine_authentication_authenticate(retry.request, username, password)); retry.terminal();
  normal->authenticated("basic-retry");
  g_assert_true(server_observed(base, "basic-retry", false, "Basic"));
  g_assert_true(server_observed(base, "basic-retry", true, "Basic"));
  g_print("Real Basic 401 exchange, wrong credential retry, exact metadata and once-only completion: PASS\n");

  clear_authentication(normal_session);
  uri = base + "auth/digest?realm=digest-native";
  auto& digest = requests.next(normal->view, uri);
  metadata(digest, uri, "digest-native", "digest", false, false, port);
  g_assert_true(ephy_engine_authentication_authenticate(digest.request, username, password)); digest.terminal();
  normal->authenticated("digest-native");
  g_assert_true(server_observed(base, "digest-native", true, "Digest"));
  g_print("Real Digest nonce/qop/response verified by the independent HTTP server: PASS\n");

  clear_authentication(normal_session);
  auto *private_session = ephy_engine_session_new(true);
  auto private_page = Page::create(GTK_APPLICATION(app), private_session, &requests);
  uri = base + "auth/basic?realm=session-isolation";
  auto& regular = requests.next(normal->view, uri);
  auto& incognito = requests.next(private_page->view, uri);
  metadata(regular, uri, "session-isolation", "basic", false, false, port);
  metadata(incognito, uri, "session-isolation", "basic", false, true, port);
  g_assert_false(ephy_engine_session_context(normal_session)->IsSame(ephy_engine_session_context(private_session)));
  g_assert_true(ephy_engine_authentication_authenticate(regular.request, username, password));
  regular.terminal(); normal->authenticated("session-isolation");
  g_assert_false(ephy_engine_authentication_is_resolved(incognito.request));
  const auto cached_count = requests.entries.size();
  normal->navigate(uri); normal->authenticated("session-isolation");
  g_assert_cmpuint(requests.entries.size(), ==, cached_count);
  g_assert_true(ephy_engine_authentication_cancel(incognito.request)); incognito.terminal();
  private_page->navigate(base);
  auto& private_again = requests.next(private_page->view, uri);
  g_assert_true(ephy_engine_authentication_authenticate(private_again.request, username, password));
  private_again.terminal(); private_page->authenticated("session-isolation");
  auto *fresh_session = ephy_engine_session_new(true);
  auto fresh = Page::create(GTK_APPLICATION(app), fresh_session, &requests);
  auto& fresh_request = requests.next(fresh->view, uri);
  g_assert_true(ephy_engine_authentication_cancel(fresh_request.request)); fresh_request.terminal();
  g_print("Concurrent regular/private requests and actual native credential caches remain isolated: PASS\n");

  for (const std::string action : {"navigation", "hide", "unroot", "close", "cancel"}) {
    clear_authentication(normal_session);
    auto page = Page::create(GTK_APPLICATION(app), normal_session, &requests);
    auto& pending = requests.next(page->view, base + "auth/basic?realm=" + action);
    if (action == "navigation") page->navigate(base);
    else if (action == "hide") gtk_widget_set_visible(GTK_WIDGET(page->view), false);
    else if (action == "unroot") adw_application_window_set_content(page->window, nullptr);
    else if (action == "close") { gtk_window_destroy(GTK_WINDOW(page->window)); page->window = nullptr; }
    else g_assert_true(ephy_engine_authentication_cancel(pending.request));
    wait_for([&] { return ephy_engine_authentication_is_resolved(pending.request); }); pending.terminal();
  }
  auto unhandled = Page::create(GTK_APPLICATION(app), normal_session, nullptr);
  unhandled->navigate(base + "auth/basic?realm=unhandled");
  g_assert_true(server_observed(base, "unhandled", false, ""));
  g_print("Navigation, hide, unroot, close, explicit cancel and unhandled challenges reject late answers: PASS\n");

  auto *proxy_session = ephy_engine_session_new(true);
  auto proxy = Page::create(GTK_APPLICATION(app), proxy_session, &requests);
  set_fixture_proxy(proxy_session, port);
  uri = "http://auth-proxy.invalid/auth/proxy?realm=proxy-native";
  auto& proxy_request = requests.next(proxy->view, uri);
  metadata(proxy_request, uri, "proxy-native", "basic", true, true, port);
  g_assert_true(ephy_engine_authentication_authenticate(proxy_request.request, username, password));
  proxy_request.terminal(); proxy->authenticated("proxy-native");
  g_assert_true(server_observed(base, "proxy-native", true, "Basic"));
  g_print("Real 407 proxy challenge uses actual proxy recipient and native credentials: PASS\n");

  clear_authentication(normal_session);
  auto& shutdown_request = requests.next(normal->view, base + "auth/basic?realm=shutdown");
  g_assert_true(ephy_engine_prepare_shutdown());
  shutdown_request.terminal();
  g_print("Actual engine shutdown cancels pending native credentials and rejects late answers: PASS\n");

  proxy.reset(); unhandled.reset(); fresh.reset(); private_page.reset(); normal.reset();
  wait_for([] { return Browser::active_count == 0; });
  g_object_unref(proxy_session); g_object_unref(fresh_session); g_object_unref(private_session);
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
