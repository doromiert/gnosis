/* SPDX-License-Identifier: GPL-3.0-or-later */
// Diagnostic: pinned CEF OnLoadStart can expose a stale window.document.
// Keep this outside the default suite until the native script-start hook is built.
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "ephy-engine-scheme.h"
#include "page-messages.hpp"
#include "browser.hpp"
#include "include/cef_app.h"
#include "include/cef_api_hash.h"
#include "include/cef_parser.h"
#include <filesystem>
#include <functional>
#include <source_location>
#include <string>
#include <unistd.h>

namespace {
class UserContentStartRenderer final : public CefRenderProcessHandler, public CefLoadHandler {
public:
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> info) override {
    delegate_->OnBrowserCreated(browser, info);
  }
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    delegate_->OnBrowserDestroyed(browser);
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    delegate_->OnContextCreated(browser, frame, context);
  }
  void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   TransitionType transition) override {
    if (auto handler = delegate_->GetLoadHandler()) handler->OnLoadStart(browser, frame, transition);
    const auto url = frame->GetURL().ToString();
    g_print("USER_CONTENT_COMMIT pid=%d main=%d frame=%s url=%s\n", getpid(), frame->IsMain(), frame->GetIdentifier().ToString().c_str(), url.c_str());
    if (url.find("/user-content-start-denied") != std::string::npos ||
        (url.find("/user-content-start") == std::string::npos &&
         url != "ephy-about:user-content-start" && url != "ephy-reader:user-content-start")) return;
    auto context = frame->GetV8Context();
    g_assert_nonnull(context.get());
    const bool entered = context->Enter();
    g_assert_true(entered);
    auto marker = CefDictionaryValue::Create();
    marker->SetInt("pid", getpid());
    marker->SetInt("browser", browser->GetIdentifier());
    marker->SetBool("main", frame->IsMain());
    marker->SetString("committedURL", url);
    auto value = CefValue::Create();
    value->SetDictionary(marker);
    const auto code = std::string("Object.defineProperty(globalThis,'__gnosisUserContentStart',{value:Object.freeze({...") +
      CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() +
      ",documentElementPresent:!!document.documentElement,injectedHref:String(location.href),injectedDocumentURL:String(document.URL),injectedReady:document.readyState}),writable:false,configurable:false});true";
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    const bool ok = context->Eval(code, "gnosis://native-user-content-start", 1, result, exception);
    if (!ok || exception || !result || !result->IsBool() || !result->GetBoolValue()) {
      g_printerr("USER_CONTENT_START_NATIVE_EVAL_FAILED pid=%d url=%s error=%s\n", getpid(), url.c_str(),
                 exception ? exception->GetMessage().ToString().c_str() : "No successful boolean result");
    }
    context->Exit();
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    delegate_->OnContextReleased(browser, frame, context);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source, CefRefPtr<CefProcessMessage> message) override {
    return delegate_->OnProcessMessageReceived(browser, frame, source, message);
  }
private:
  CefRefPtr<CefRenderProcessHandler> delegate_ = PageMessages::renderer_handler();
  IMPLEMENT_REFCOUNTING(UserContentStartRenderer);
};
class UserContentStartApp final : public CefApp {
public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return renderer_; }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    ephy_engine_register_schemes(registrar);
  }
private:
  CefRefPtr<CefRenderProcessHandler> renderer_ = new UserContentStartRenderer;
  IMPLEMENT_REFCOUNTING(UserContentStartApp);
};
void wait_for(const std::function<bool()>& predicate,
              std::source_location at = std::source_location::current()) {
  const auto deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  if (!predicate()) g_error("User-content-start timeout at %s:%u", at.file_name(), at.line());
}
void load(EphyEngineView* view, const char* url) {
  bool loaded = false;
  const auto signal = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool* done) {
    if (event == WEBKIT_LOAD_FINISHED) *done = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, url);
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, signal);
}
EphyEngineView* page(GtkApplication* app, EphyEngineSession* session) {
  auto* window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto* view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  gtk_window_present(GTK_WINDOW(window));
  return view;
}
JsonObject* assert_report(JsonObject* all, const char* name, bool main) {
  g_assert_true(json_object_has_member(all, name));
  auto* report = json_object_get_object_member(all, name);
  g_assert_nonnull(report);
  g_assert_true(json_object_get_boolean_member(report, "firstSawMarker"));
  g_assert_true(json_object_get_boolean_member(report, "nonceRan"));
  g_assert_false(json_object_get_boolean_member(report, "unauthorizedRan"));
  g_assert_cmpint(json_object_get_boolean_member(report, "main"), ==, main);
  g_assert_cmpstr(json_object_get_string_member(report, "committedURL"), ==, json_object_get_string_member(report, "href"));
  g_assert_cmpstr(json_object_get_string_member(report, "injectedHref"), ==, json_object_get_string_member(report, "href"));
  g_assert_cmpstr(json_object_get_string_member(report, "injectedDocumentURL"), ==, json_object_get_string_member(report, "href"));
  g_assert_cmpint(json_object_get_int_member(report, "pid"), >, 0);
  g_assert_cmpint(json_object_get_int_member(report, "browser"), >, 0);
  return report;
}
gint64 check(EphyEngineView* view, const char* url, bool children, const char* label) {
  load(view, url);
  wait_for([&] {
    const auto *native_title = ephy_engine_web_view_get_title(view);
    const std::string title = native_title ? native_title : "";
    return title.starts_with("USER_CONTENT_START ") &&
      (!children || (title.find("\"local\":") != std::string::npos && title.find("\"oopif\":") != std::string::npos && title.find("\"excluded\":") != std::string::npos));
  });
  // Read the actual native title, so CDP evaluation under strict CSP cannot
  // accidentally become the mechanism or prerequisite being tested.
  const auto text = std::string(ephy_engine_web_view_get_title(view)).substr(sizeof("USER_CONTENT_START ") - 1);
  g_print("%s real first-inline reports: %s\n", label, text.c_str());
  auto* parser = json_parser_new();
  GError* error = nullptr;
  g_assert_true(json_parser_load_from_data(parser, text.c_str(), -1, &error));
  g_assert_no_error(error);
  auto* reports = json_node_get_object(json_parser_get_root(parser));
  auto* main = assert_report(reports, "main", true);
  g_assert_cmpstr(json_object_get_string_member(main, "href"), ==, url);
  const auto browser = json_object_get_int_member(main, "browser");
  if (children) {
    auto* local = assert_report(reports, "local", false);
    auto* remote = assert_report(reports, "oopif", false);
    auto* excluded = json_object_get_object_member(reports, "excluded");
    g_assert_nonnull(excluded);
    g_assert_false(json_object_get_boolean_member(excluded, "firstSawMarker"));
    g_assert_false(json_object_get_boolean_member(excluded, "unauthorizedRan"));
    g_assert_true(json_object_get_boolean_member(excluded, "nonceRan"));
    g_assert_cmpint(json_object_get_int_member(main, "pid"), ==, json_object_get_int_member(local, "pid"));
    g_assert_cmpint(json_object_get_int_member(main, "pid"), !=, json_object_get_int_member(remote, "pid"));
    g_assert_cmpint(browser, ==, json_object_get_int_member(local, "browser"));
    g_assert_cmpint(browser, ==, json_object_get_int_member(remote, "browser"));
  }
  g_object_unref(parser);
  g_print("%s strict CSP and synchronous native main-world start%s: PASS\n", label,
          children ? ", including actual same-process child and different-process OOPIF" : "");
  return browser;
}
} // namespace

NO_STACK_PROTECTOR int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  auto command = CefCommandLine::CreateCommandLine();
  command->InitFromArgv(argc, argv);
  if (command->HasSwitch("type")) {
    return CefExecuteProcess(CefMainArgs(argc, argv), new UserContentStartApp, nullptr);
  }
  const int child = ephy_engine_execute_process(argc, argv);
  if (child >= 0) return child;
  auto* app = adw_application_new("com.neg-zero.Gnosis.UserContentStartTest", G_APPLICATION_NON_UNIQUE);
  GError* error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error));
  g_assert_no_error(error);
  char profile[] = "/tmp/gnosis-user-content-start-profile-XXXXXX";
  g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true));
  disable_diagnostic_core_dumps();
  const auto fixture = std::filesystem::path(g_getenv("GNOSIS_USER_CONTENT_START_INTERNAL_HTML"));
  for (const char* scheme : {"ephy-about", "ephy-reader"}) {
    ephy_engine_register_uri_scheme(scheme,
      +[](EphyEngineSchemeRequest* request, gpointer path) {
        gchar* body = nullptr;
        gsize size = 0;
        GError* error = nullptr;
        g_assert_true(g_file_get_contents(static_cast<const char*>(path), &body, &size, &error));
        g_assert_no_error(error);
        auto* stream = g_memory_input_stream_new_from_data(body, size, g_free);
        ephy_engine_uri_scheme_request_finish(request, stream, size, "text/html");
        g_object_unref(stream);
      }, g_strdup(fixture.c_str()), g_free);
  }
  auto* normal = page(GTK_APPLICATION(app), ephy_engine_session_get_default());
  const auto normal_browser = check(normal, g_getenv("GNOSIS_USER_CONTENT_START_URL"), true, "regular HTTP");
  check(normal, "ephy-about:user-content-start", false, "regular ephy-about");
  check(normal, "ephy-reader:user-content-start", false, "regular ephy-reader");
  auto* private_session = ephy_engine_session_new(true);
  auto* private_view = page(GTK_APPLICATION(app), private_session);
  const auto private_browser = check(private_view, g_getenv("GNOSIS_USER_CONTENT_START_URL"), true, "private HTTP");
  g_assert_cmpint(normal_browser, !=, private_browser);
  check(private_view, "ephy-about:user-content-start", false, "private ephy-about");
  check(private_view, "ephy-reader:user-content-start", false, "private ephy-reader");
  while (auto* windows = gtk_application_get_windows(GTK_APPLICATION(app)))
    gtk_window_destroy(GTK_WINDOW(windows->data));
  wait_for([] { return Browser::active_count == 0; });
  g_object_unref(private_session);
  ephy_engine_shutdown();
  g_object_unref(app);
  std::filesystem::remove_all(profile);
  g_print("Native main-world document-start checks completed; USER CSS and named-world lifecycle are not covered.\n");
}
