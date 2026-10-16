/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <string>

static void wait_for(const std::function<bool()>& predicate, const char *operation = "native operation") {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("Settings test timed out: %s", operation);
}
static std::string evaluate(EphyEngineView *view, const char *script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject*, GAsyncResult *result, gpointer user) { *static_cast<GAsyncResult**>(user) = G_ASYNC_RESULT(g_object_ref(result)); }, &result);
  wait_for([&]{ return result; }, script);
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  char *text = g_strdup(script_result_text(value).c_str());
  std::string output(text); g_free(text); json_object_unref(value); g_object_unref(result);
  return output;
}
static EphyEngineView *page(GtkApplication *app, EphyEngineSession *session, const char *uri) {
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, uri); gtk_window_present(GTK_WINDOW(window));
  wait_for([&]{ return loaded; }); g_signal_handler_disconnect(view, handler);
  g_print("Settings fixture loaded: %s\n", ephy_engine_web_view_get_uri(view));
  return view;
}
static void assert_script(EphyEngineView *view, const char *script) {
  std::string output;
  auto deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
  do {
    output = evaluate(view, script);
    if (output == "true") return;
    g_usleep(20000);
  } while (g_get_monotonic_time() < deadline);
  g_error("Settings script failed: %s; result: %s", script, output.c_str());
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  bool flags = argc > 1 && !strcmp(argv[1], "--flags");
  auto *settings = ephy_engine_settings_get_default();
  const char *languages[] = {"en-GB", "en", nullptr};
  g_object_set(settings, "default-font-family", "DejaVu Sans Mono", "default-font-size", 23u,
    "default-monospace-font-size", 19u, "monospace-font-family", "DejaVu Sans Mono",
    "minimum-font-size", 7u, "preferred-languages", languages,
    "enable-spell-checking", false, "enable-caret-browsing", true,
    "user-agent", "JupiterSettingsFixture/1.0", nullptr);
  if (flags) {
    gsize count;
    auto *features = ephy_engine_settings_get_features(&count);
    for (gsize i = 0; i < count; ++i) {
      bool enabled = !features[i].default_value;
      ephy_engine_settings_set_feature_enabled(settings, &features[i], enabled);
      g_assert_cmpint(ephy_engine_settings_get_feature_enabled(settings, &features[i]), ==, enabled);
    }
  }
  auto *app = adw_application_new("io.github.doromiert.Jupiter.SettingsTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-settings-test-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  auto *normal = ephy_engine_session_get_default();
  std::string uri = std::string(g_getenv("JUPITER_TEST_URL")) + "settings-fixture";
  auto *view = page(GTK_APPLICATION(app), normal, uri.c_str());
  assert_script(view, "initialHeaders.userAgent==='JupiterSettingsFixture/1.0' && navigator.userAgent===initialHeaders.userAgent");
  assert_script(view, "initialHeaders.language.startsWith('en-GB') && navigator.language==='en-GB'");
  assert_script(view, "getComputedStyle(sample).fontSize==='23px' && getComputedStyle(sample).fontFamily.includes('DejaVu Sans Mono') && getComputedStyle(fixed).fontSize==='19px'");
  auto context = ephy_engine_session_context(normal);
  g_assert_false(context->GetPreference("browser.enable_spellchecking")->GetBool());
  g_assert_true(context->GetPreference("settings.a11y.caretbrowsing.enabled")->GetBool());
  g_assert_cmpint(context->GetPreference("webkit.webprefs.minimum_font_size")->GetInt(), ==, 7);
  g_print("Native startup fonts, languages, first-request user agent, spellcheck and caret preferences: PASS\n");
  if (flags) {
    assert_script(view, "document.createElement('canvas').getContext('webgl')===null");
    assert_script(view, "typeof navigator.gpu==='undefined' || navigator.gpu.requestAdapter().then(adapter=>adapter===null)");
    g_print("Restarted feature overrides disable actual Chromium WebGL and WebGPU: PASS\n");
  }
  auto *private_session = ephy_engine_session_new(true);
  auto *private_view = page(GTK_APPLICATION(app), private_session, uri.c_str());
  assert_script(private_view, "getComputedStyle(sample).fontSize==='23px' && navigator.language==='en-GB' && navigator.userAgent==='JupiterSettingsFixture/1.0'");
  const char *polish[] = {"pl-PL", "pl", nullptr};
  g_object_set(settings, "default-font-size", 29u, "preferred-languages", polish,
    "user-agent", "JupiterSettingsFixture/2.0", "enable-spell-checking", true, nullptr);
  for (auto *v : {view, private_view}) {
    assert_script(v, "getComputedStyle(sample).fontSize==='29px' && navigator.language==='pl-PL' && navigator.userAgent==='JupiterSettingsFixture/2.0'");
    assert_script(v, "fetch('/echo-headers').then(r=>r.json()).then(h=>h.userAgent==='JupiterSettingsFixture/2.0' && h.language.startsWith('pl-PL'))");
  }
  g_print("Live font, language and HTTP/JavaScript user-agent changes reach regular and private tabs: PASS\n");
  auto *another_session = ephy_engine_session_new(true);
  auto *another_view = page(GTK_APPLICATION(app), another_session, uri.c_str());
  assert_script(another_view, "initialHeaders.userAgent==='JupiterSettingsFixture/2.0' && navigator.language==='pl-PL' && getComputedStyle(sample).fontSize==='29px'");
  g_print("New private contexts inherit current browser preferences: PASS\n");
  for (auto *session : {normal, another_session}) {
    auto *download = ephy_engine_session_download_uri(session, (std::string(g_getenv("JUPITER_TEST_URL")) + "download-headers").c_str());
    bool done = false;
    g_signal_connect(download, "finished", G_CALLBACK(+[](EphyEngineDownload*, bool *done) { *done = true; }), &done);
    g_signal_connect(download, "failed", G_CALLBACK(+[](EphyEngineDownload*, GError *error, gpointer) { g_error("Download settings check: %s", error->message); }), nullptr);
    auto path = std::string(profile) + (session == normal ? "/headers-normal" : "/headers-private");
    ephy_engine_download_set_destination(download, path.c_str());
    wait_for([&]{ return done; }, "background identity download");
    char *bytes = nullptr;
    g_assert_true(g_file_get_contents(path.c_str(), &bytes, nullptr, &error)); g_assert_no_error(error);
    g_assert_nonnull(strstr(bytes, "JupiterSettingsFixture/2.0"));
    g_assert_nonnull(strstr(bytes, "\"language\": \"pl-PL"));
    g_free(bytes); g_object_unref(download);
  }
  g_print("Background downloads use current identity and language in regular and private contexts: PASS\n");
  ephy_engine_web_view_set_user_agent_override(private_view, "PrivateViewFixture/1.0");
  assert_script(private_view, "navigator.userAgent==='PrivateViewFixture/1.0'");
  assert_script(private_view, "fetch('/echo-headers').then(r=>r.json()).then(h=>h.userAgent==='PrivateViewFixture/1.0')");
  assert_script(view, "fetch('/echo-headers').then(r=>r.json()).then(h=>h.userAgent==='JupiterSettingsFixture/2.0')");
  ephy_engine_web_view_set_user_agent_override(private_view, nullptr);
  assert_script(private_view, "navigator.userAgent==='JupiterSettingsFixture/2.0'");
  g_print("Per-view identity overrides do not change other tabs: PASS\n");
  g_object_set(settings, "user-agent", "", nullptr);
  for (auto *v : {view, private_view, another_view}) {
    assert_script(v, "navigator.userAgent.includes('Chrome/') && !navigator.userAgent.includes('JupiterSettingsFixture')");
    assert_script(v, "fetch('/echo-headers').then(r=>r.json()).then(h=>h.userAgent===navigator.userAgent)");
  }
  g_print("Clearing a custom user agent restores Chromium's actual identity: PASS\n");
  ephy_engine_session_set_storage_enabled(private_session, false);
  g_object_set(settings, "default-font-size", 25u, nullptr);
  g_assert_cmpint(ephy_engine_session_context(private_session)->GetPreference("profile.default_content_setting_values.cookies")->GetInt(), ==, CEF_CONTENT_SETTING_VALUE_BLOCK);
  ephy_engine_session_set_storage_enabled(private_session, true);
  for (auto *v : {view, private_view, another_view})
    assert_script(v, "document.cookie='storagePolicy=allowed;SameSite=Lax;Path=/';document.cookie.includes('storagePolicy=allowed')");
  g_object_set(settings, "enable-website-data-storage", false, nullptr);
  for (auto *v : {view, another_view}) {
    assert_script(v, "fetch('/echo-cookie').then(r=>r.text()).then(cookie=>!cookie.includes('storagePolicy='))");
    assert_script(v, "document.cookie===''");
    assert_script(v, "(()=>{try{localStorage.setItem('policy','blocked');return false}catch(e){return e.name==='SecurityError'}})()");
    assert_script(v, "new Promise(resolve=>{try{let r=indexedDB.open('policy');r.onsuccess=()=>{r.result.close();resolve(false)};r.onerror=()=>resolve(true)}catch(e){resolve(e.name==='SecurityError')}})");
  }
  assert_script(private_view, "fetch('/echo-cookie').then(r=>r.text()).then(cookie=>cookie.includes('storagePolicy=allowed'))");
  for (auto *session : {normal, another_session}) {
    auto *download = ephy_engine_session_download_uri(session, (std::string(g_getenv("JUPITER_TEST_URL")) + "download-cookie").c_str());
    bool done = false;
    g_signal_connect(download, "finished", G_CALLBACK(+[](EphyEngineDownload*, bool *done) { *done = true; }), &done);
    g_signal_connect(download, "failed", G_CALLBACK(+[](EphyEngineDownload*, GError *error, gpointer) { g_error("Download storage policy: %s", error->message); }), nullptr);
    auto path = std::string(profile) + (session == normal ? "/blocked-normal-cookies" : "/blocked-private-cookies");
    ephy_engine_download_set_destination(download, path.c_str());
    wait_for([&]{ return done; }, "blocked-cookie download");
    char *bytes = nullptr;
    g_assert_true(g_file_get_contents(path.c_str(), &bytes, nullptr, &error)); g_assert_no_error(error);
    g_assert_cmpstr(bytes, ==, "");
    g_free(bytes); g_object_unref(download);
  }
  g_object_set(settings, "enable-website-data-storage", true, nullptr);
  g_print("Storage policy covers actual HTTP/download/JavaScript cookies, localStorage and IndexedDB; session overrides remain isolated: PASS\n");
  context = nullptr;
  while (auto *windows = gtk_application_get_windows(GTK_APPLICATION(app))) gtk_window_destroy(GTK_WINDOW(windows->data));
  wait_for([]{ return Browser::active_count == 0; });
  g_object_unref(private_session); g_object_unref(another_session);
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
