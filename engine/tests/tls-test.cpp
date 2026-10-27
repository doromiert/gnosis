/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-certificate.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);

static void wait_for(const std::function<bool()>& ready) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!ready() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  g_assert_true(ready());
}

static std::string evaluate(EphyEngineView *view, const std::string& script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr,
    +[](GObject*, GAsyncResult *result, gpointer user) {
      *static_cast<GAsyncResult**>(user) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(value);
  auto text = script_result_text(value);
  json_object_unref(value);
  g_object_unref(result);
  return text;
}

static void rejected_download(EphyEngineSession *session, const std::string& uri, const std::string& path) {
  struct Result { bool finished = false; GError *error = nullptr; } result;
  auto *download = ephy_engine_session_download_uri(session, uri.c_str());
  ephy_engine_download_set_destination(download, path.c_str());
  g_signal_connect(download, "finished", G_CALLBACK(+[](EphyEngineDownload*, Result *result) {
    result->finished = true;
  }), &result);
  g_signal_connect(download, "failed", G_CALLBACK(+[](EphyEngineDownload*, GError *error, Result *result) {
    result->error = g_error_copy(error);
  }), &result);
  wait_for([&] { return result.finished || result.error; });
  g_assert_nonnull(result.error);
  g_assert_false(result.finished);
  g_assert_false(g_file_test(path.c_str(), G_FILE_TEST_EXISTS));
  g_clear_error(&result.error);
  g_signal_handlers_disconnect_by_data(download, &result);
  g_object_unref(download);
}

struct Page {
  EphyEngineView *view;
  GtkWindow *window;
  unsigned tls = 0, generic = 0, commits = 0, loads = 0;
  bool replace_tls_error = false;
  GTlsCertificate *certificate = nullptr;
  GTlsCertificateFlags errors = GTlsCertificateFlags(0);
  std::string failed_uri;
  Page(GtkApplication *app, EphyEngineSession *session) {
    window = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_default_size(window, 900, 650);
    view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(view));
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(+[](EphyEngineView *view, const char *uri,
        GTlsCertificate *certificate, GTlsCertificateFlags errors, Page *page) -> gboolean {
      page->tls++;
      page->failed_uri = uri;
      page->errors = errors;
      g_set_object(&page->certificate, certificate);
      if (page->replace_tls_error) {
        page->commits = page->loads = 0;
        ephy_engine_web_view_load_alternate_html(view,
          "<!doctype html><title>Native TLS error replacement</title><p>Certificate rejected</p>", uri, nullptr);
      }
      return true;
    }), this);
    g_signal_connect(view, "load-failed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent,
        const char*, GError*, Page *page) -> gboolean { page->generic++; return true; }), this);
    g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, Page *page) {
      if (event == WEBKIT_LOAD_COMMITTED) page->commits++;
      if (event == WEBKIT_LOAD_FINISHED) page->loads++;
    }), this);
    gtk_window_present(window);
  }
  ~Page() { g_clear_object(&certificate); }
  void rejected(const std::string& uri, GTlsCertificateFlags expected) {
    g_print("Checking native TLS rejection: %s\n", uri.c_str());
    auto before = tls, generic_before = generic;
    ephy_engine_web_view_load_uri(view, uri.c_str());
    wait_for([&] {
      auto *resource = ephy_engine_web_view_get_main_resource(view);
      auto *response = resource ? ephy_engine_web_resource_get_response(resource) : nullptr;
      return tls != before || generic != generic_before ||
        (response && ephy_engine_response_get_status_code(response) == 200 &&
         g_strcmp0(ephy_engine_web_resource_get_uri(resource), uri.c_str()) == 0);
    });
    g_print("Observed rejection result: tls=%u generic=%u loads=%u errors=%u title=%s\n", tls, generic, loads, errors, ephy_engine_web_view_get_title(view));
    g_assert_cmpuint(tls, ==, before + 1);
    g_assert_cmpuint(generic, ==, generic_before);
    g_assert_true(failed_uri == uri);
    g_assert_cmpuint(errors & expected, ==, expected);
    g_assert_nonnull(certificate);
  }
  void loaded(const std::string& uri, const char *title) {
    g_print("Checking native navigation response: %s\n", uri.c_str());
    auto before = loads, tls_before = tls, generic_before = generic;
    ephy_engine_web_view_load_uri(view, uri.c_str());
    wait_for([&] {
      auto *resource = ephy_engine_web_view_get_main_resource(view);
      auto *response = resource ? ephy_engine_web_resource_get_response(resource) : nullptr;
      return loads > before && response && ephy_engine_response_get_status_code(response) == 200 &&
        !ephy_engine_web_view_is_loading(view) &&
        g_strcmp0(ephy_engine_web_view_get_title(view), title) == 0;
    });
    g_assert_cmpuint(tls, ==, tls_before);
    g_assert_cmpuint(generic, ==, generic_before);
    g_assert_cmpstr(ephy_engine_web_view_get_title(view), ==, title);
  }
  void allow(EphyEngineSession *session, const char *host) {
    ephy_engine_session_allow_tls_certificate_for_host(session, certificate, host);
  }
  void date_error(GTlsCertificateFlags expected) {
    auto browser = ephy_engine_view_browser(view);
    auto entry = browser->GetHost()->GetVisibleNavigationEntry();
    auto ssl = entry ? entry->GetSSLStatus() : nullptr;
    auto native = ssl ? ssl->GetX509Certificate() : nullptr;
    g_assert_nonnull(native.get());
    g_autoptr(GTlsCertificate) converted = ephy_engine_certificate_to_tls(native);
    g_assert_true(g_tls_certificate_is_same(converted, certificate));
    g_assert_cmpuint(ephy_engine_certificate_errors(CERT_STATUS_DATE_INVALID, native), ==, expected);
  }
};

int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv);
  if (sub >= 0) return sub;
  auto *app = adw_application_new("com.neg-zero.Gnosis.TlsTest", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, nullptr));
  char scratch[] = "/tmp/gnosis-tls-test-XXXXXX";
  g_assert_nonnull(mkdtemp(scratch));
  g_assert_true(ephy_engine_initialize(argc, argv, scratch, true));
  disable_diagnostic_core_dumps();
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, g_getenv("GNOSIS_TLS_FIXTURES"), -1, nullptr));
  auto *urls = json_node_get_object(json_parser_get_root(parser));
  auto url = [&](const char *name) { return std::string(json_object_get_string_member(urls, name)); };
  auto *normal = ephy_engine_session_get_default();
  g_autoptr(EphyEngineSession) private_session = ephy_engine_session_new(true);
  g_autoptr(EphyEngineSession) isolated_session = ephy_engine_session_new(true);
  g_autoptr(EphyEngineSession) download_session = ephy_engine_session_new(true);
  g_autoptr(EphyEngineSession) redirect_session = ephy_engine_session_new(true);
  std::vector<std::unique_ptr<Page>> pages;
  auto page = [&](EphyEngineSession *session) -> Page& {
    pages.push_back(std::make_unique<Page>(GTK_APPLICATION(app), session));
    return *pages.back();
  };
  auto& regular = page(normal);
  regular.rejected(url("valid"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  g_autoptr(GTlsCertificate) root = g_tls_certificate_new_from_file(g_getenv("GNOSIS_TLS_CA"), nullptr);
  auto *issuer = g_tls_certificate_get_issuer(regular.certificate);
  g_assert_nonnull(issuer);
  g_assert_true(g_tls_certificate_is_same(issuer, root));
  g_assert_cmpuint(ephy_engine_certificate_errors(static_cast<cef_cert_status_t>(
    CERT_STATUS_IS_EV | CERT_STATUS_REV_CHECKING_ENABLED | CERT_STATUS_SHA1_SIGNATURE_PRESENT |
    CERT_STATUS_CT_COMPLIANCE_FAILED), nullptr), ==, 0);
  g_assert_cmpuint(ephy_engine_certificate_errors(static_cast<cef_cert_status_t>(
    CERT_STATUS_COMMON_NAME_INVALID | CERT_STATUS_AUTHORITY_INVALID | CERT_STATUS_REVOKED |
    CERT_STATUS_WEAK_KEY | CERT_STATUS_INVALID | CERT_STATUS_IS_EV), nullptr), ==,
    G_TLS_CERTIFICATE_BAD_IDENTITY | G_TLS_CERTIFICATE_UNKNOWN_CA | G_TLS_CERTIFICATE_REVOKED |
    G_TLS_CERTIFICATE_INSECURE | G_TLS_CERTIFICATE_GENERIC_ERROR);
  g_print("Native TLS failure exposes the real issuer chain and maps error bits without informational statuses: PASS\n");

  regular.allow(normal, "127.0.0.1");
  regular.loaded(url("valid"), "TLS fixture valid");
  GTlsCertificate *visible = nullptr;
  GTlsCertificateFlags visible_errors;
  g_assert_true(ephy_engine_web_view_get_tls_info(regular.view, &visible, &visible_errors));
  g_assert_true(g_tls_certificate_is_same(visible, regular.certificate));
  g_assert_nonnull(g_tls_certificate_get_issuer(visible));
  g_assert_cmpuint(visible_errors & G_TLS_CERTIFICATE_UNKNOWN_CA, !=, 0);
  g_object_unref(visible);
  g_print("Accepting the exact native certificate reloads real HTTPS and retains certificate security details: PASS\n");

  auto& private_page = page(private_session);
  private_page.rejected(url("valid"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  auto mask = ephy_engine_session_context(private_session)->GetWebsiteSetting(
    "https://127.0.0.1", "https://127.0.0.1", CEF_CONTENT_SETTING_TYPE_SSL_CERT_DECISIONS);
  g_assert_nonnull(mask.get());
  g_assert_true(mask->GetDictionary()->GetString("decision_expiration_time") == "9223372036854775807");
  g_assert_cmpuint(mask->GetDictionary()->GetDictionary("cert_exceptions_map")->GetSize(), ==, 0);
  regular.loaded(url("valid"), "TLS fixture valid");
  private_page.rejected(url("valid"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  private_page.allow(private_session, "127.0.0.1");
  private_page.loaded(url("valid"), "TLS fixture valid");
  auto& isolated_page = page(isolated_session);
  isolated_page.rejected(url("valid"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  g_print("Certificate exceptions isolate regular, private, and independent private contexts: PASS\n");

  regular.rejected(url("replacement"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  auto another_host = url("valid");
  another_host.replace(another_host.find("127.0.0.1"), 9, "localhost");
  regular.rejected(another_host, G_TLS_CERTIFICATE_BAD_IDENTITY);
  regular.rejected(url("wrong-host"), G_TLS_CERTIFICATE_BAD_IDENTITY);
  // The local CA is untrusted: Chromium may stop verification before reporting dates.
  regular.rejected(url("expired"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  regular.date_error(G_TLS_CERTIFICATE_EXPIRED);
  regular.rejected(url("future"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  regular.date_error(G_TLS_CERTIFICATE_NOT_ACTIVATED);
  regular.rejected(url("self-signed"), G_TLS_CERTIFICATE_UNKNOWN_CA);
  g_print("Certificate/host boundaries and native status/date mapping: PASS\n");

  auto& error_page = page(normal);
  auto expired_uri = url("expired");
  error_page.replace_tls_error = true;
  error_page.rejected(expired_uri, G_TLS_CERTIFICATE_UNKNOWN_CA);
  wait_for([&] {
    return error_page.loads && !ephy_engine_web_view_is_loading(error_page.view) &&
      g_strcmp0(ephy_engine_web_view_get_title(error_page.view), "Native TLS error replacement") == 0;
  });
  g_assert_true(evaluate(error_page.view,
    "new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve("
    "document.readyState==='complete'&&document.title==='Native TLS error replacement'&&"
    "document.body.textContent==='Certificate rejected'))))") == "true");
  g_assert_cmpuint(error_page.tls, ==, 1);
  g_assert_cmpuint(error_page.generic, ==, 0);
  g_assert_cmpuint(error_page.commits, ==, 1);
  g_assert_cmpuint(error_page.loads, ==, 1);
  g_assert_cmpstr(ephy_engine_web_view_get_uri(error_page.view), ==, expired_uri.c_str());
  g_print("Native TLS alternate HTML retains the failed URI and emits exactly one committed/finished pair: PASS\n");

  auto cross_host = url("replacement");
  cross_host.replace(cross_host.find("127.0.0.1"), 9, "localhost");
  regular.rejected(cross_host, G_TLS_CERTIFICATE_BAD_IDENTITY);
  regular.allow(normal, "localhost");
  regular.loaded(cross_host, "TLS fixture replacement");
  private_page.loaded(url("plain"), "TLS fixture plain");
  auto private_before = private_page.tls;
  g_assert_true(evaluate(private_page.view,
    "fetch('" + url("valid") + "private-accepted-resource').then(r=>r.ok ? 'allowed' : 'failed',()=> 'blocked')") == "allowed");
  g_assert_true(evaluate(private_page.view,
    "fetch('" + cross_host + "subresource').then(()=> 'allowed',()=> 'blocked')") == "blocked");
  g_assert_cmpuint(private_page.tls, ==, private_before);
  isolated_page.loaded(url("plain"), "TLS fixture plain");
  g_assert_true(evaluate(isolated_page.view,
    "Promise.all(Array.from({length:4},(_,i)=>fetch('" + cross_host +
    "subresource?parallel='+i).then(()=>false,()=>true))).then(results=>results.every(Boolean))") == "true");
  auto& redirect_page = page(redirect_session);
  redirect_page.loaded(url("plain"), "TLS fixture plain");
  g_assert_true(evaluate(redirect_page.view,
    "fetch('" + url("plain") + "redirect?to='+encodeURIComponent('" + cross_host +
    "subresource')).then(()=> 'allowed',()=> 'blocked')") == "blocked");
  rejected_download(download_session, cross_host + "private-download-denied", std::string(scratch) + "/refused-download");
  g_print("Private own exceptions load subresources; later normal exceptions remain blocked for other hosts, concurrent first requests, redirects and background downloads: PASS\n");

  regular.loaded(url("plain"), "TLS fixture plain");
  auto before = regular.tls;
  auto script = "fetch('" + url("wrong-host") + "subresource').then(()=> 'allowed',()=> 'blocked')";
  g_assert_true(evaluate(regular.view, script) == "blocked");
  g_assert_cmpuint(regular.tls, ==, before);
  ephy_engine_web_view_load_uri(regular.view, url("delayed").c_str());
  wait_for([] { return g_file_test(g_getenv("GNOSIS_TLS_DELAY_STARTED"), G_FILE_TEST_EXISTS); });
  regular.loaded(url("plain"), "TLS fixture plain");
  g_assert_true(g_file_set_contents(g_getenv("GNOSIS_TLS_DELAY_RELEASE"), "release", -1, nullptr));
  wait_for([] { return g_file_test(g_getenv("GNOSIS_TLS_DELAY_COMPLETED"), G_FILE_TEST_EXISTS); });
  g_assert_cmpuint(regular.tls, ==, before);
  auto generic_before = regular.generic;
  ephy_engine_web_view_load_uri(regular.view, url("unavailable").c_str());
  wait_for([&] { return regular.generic > generic_before; });
  g_assert_cmpuint(regular.tls, ==, before);
  g_print("Invalid TLS subresources, canceled navigations, and unrelated network failures never escalate into a stale main-frame TLS prompt: PASS\n");

  for (auto& current : pages) gtk_window_destroy(current->window);
  wait_for([] { return Browser::active_count == 0; });
  pages.clear();
  g_clear_object(&private_session);
  g_clear_object(&isolated_session);
  g_clear_object(&download_session);
  g_clear_object(&redirect_session);
  ephy_engine_shutdown();
  g_object_unref(app);
  std::filesystem::remove_all(scratch);
  g_print("Closing native TLS test tabs and contexts exits cleanly: PASS\n");
}
