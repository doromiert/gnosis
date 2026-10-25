/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <functional>
#include <cstring>
#include <source_location>
#include <string>

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("Extension popup test timed out at %s:%u", location.file_name(), location.line());
}
static std::string evaluate(EphyEngineView *view, const std::string& script, bool allow_navigation = false) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr,
    +[](GObject *, GAsyncResult *result, gpointer pointer) {
      *static_cast<GAsyncResult **>(pointer) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  if (allow_navigation && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      (error && ephy_engine_web_view_is_loading(view)))) {
    g_clear_error(&error); g_clear_pointer(&value, json_object_unref); g_object_unref(result);
    return "navigation pending";
  }
  g_assert_no_error(error); g_assert_nonnull(value);
  auto text = script_result_text(value);
  json_object_unref(value); g_object_unref(result);
  return text;
}
struct Window {
  AdwApplicationWindow *window;
  AdwTabView *tabs;
  EphyEngineView *opened_by = nullptr;
  EphyEngineView *created = nullptr;
};
static EphyEngineView *add_tab(Window *window, const std::string& url, bool active = true, bool defer = false) {
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  auto *page = adw_tab_view_append(window->tabs, GTK_WIDGET(view));
  if (active) adw_tab_view_set_selected_page(window->tabs, page);
  g_signal_connect(view, "open-link", G_CALLBACK(+[](EphyEngineView *owner, const char *url, gboolean active,
                                                    gboolean defer, Window *window) -> GtkWidget * {
    window->opened_by = owner;
    window->created = add_tab(window, url, active, defer);
    return GTK_WIDGET(window->created);
  }), window);
  if (!defer) ephy_engine_web_view_load_uri(view, url.c_str());
  return view;
}
static Window make_window(GtkApplication *app) {
  Window window{ADW_APPLICATION_WINDOW(adw_application_window_new(app)), ADW_TAB_VIEW(adw_tab_view_new())};
  gtk_window_set_default_size(GTK_WINDOW(window.window), 650, 450);
  adw_application_window_set_content(window.window, GTK_WIDGET(window.tabs));
  return window;
}
static EphyEngineView *make_popup(GtkApplication *app, EphyEngineView *owner, const std::string& base_url,
                                  GtkWindow **window) {
  auto tab = ephy_engine_web_view_get_extension_tab_id(owner);
  g_assert_cmpint(tab, >=, 0);
  const auto url = base_url + "?portrait=1&tabId=" + std::to_string(tab);
  g_autoptr(GUri) owner_uri = g_uri_parse(ephy_engine_web_view_get_uri(owner), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  g_assert_nonnull(owner_uri); g_assert_nonnull(g_uri_get_host(owner_uri));
  const std::string expected = std::string("ready:") + g_uri_get_host(owner_uri);
  auto *popup = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  g_assert_true(ephy_engine_web_view_set_extension_popup(popup, owner));
  *window = GTK_WINDOW(adw_application_window_new(app));
  gtk_window_set_default_size(*window, 480, 520);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(*window), GTK_WIDGET(popup));
  bool loaded = false;
  auto handler = g_signal_connect(popup, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(popup, url.c_str());
  gtk_window_present(*window);
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(popup, handler);
  std::string status, previous_status;
  wait_for([&] {
    status = evaluate(popup, R"JS((async()=>{try {
      if (!chrome.tabs.__jupiter) return 'bridge pending';
      const nativeURL = chrome.runtime.getURL('');
      if (!chrome.runtime.getManifest().version) return 'manifest pending';
      const lists = await vAPI.messaging.send('dashboard', {what:'getLists'});
      if (!lists?.userFiltersPath) return 'background pending';
      if (document.body.classList.contains('loading')) return 'hostname pending';
      return 'ready:' + document.querySelector('#hostname')?.innerText.replace(/[\s\u00ad]/g,'');
    } catch(error) { return String(error); }})())JS", true);
    if (status != previous_status) {
      g_print("Popup readiness (%s): %s\n", expected.c_str(), status.c_str());
      auto details = evaluate(popup, R"JS((()=>{
        const report={url:location.href,bridge:!!globalThis.chrome?.tabs?.__jupiter,
          hostname:document.querySelector('#hostname')?.innerText,
          loading:document.body?.classList.contains('loading'),ready:document.readyState};
        try {report.nativeURL=chrome.runtime.getURL('');report.version=chrome.runtime.getManifest().version}
        catch(error) {report.nativeError=String(error)}
        return JSON.stringify(report);
      })())JS", true);
      g_print("Popup readiness DOM: %s\n", details.c_str());
      previous_status = status;
    }
    return status == expected;
  });
  g_print("CEF genuine uBlock popup native APIs, background and hostname: %s\n", status.c_str());
  return popup;
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int child = ephy_engine_execute_process(argc, argv); if (child >= 0) return child;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.ExtensionPopupTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-extension-popup-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  g_autoptr(JsonArray) catalog = ephy_engine_extensions_dup_installed();
  g_assert_cmpuint(json_array_get_length(catalog), >=, 1);
  auto *ubo = json_array_get_object_element(catalog, 0);
  g_assert_cmpstr(json_object_get_string_member(ubo, "name"), ==, "uBlock Origin");
  g_assert_true(json_object_get_boolean_member(ubo, "enabled"));
  g_assert_true(json_object_get_boolean_member(ubo, "builtin"));
  g_assert_true(json_object_get_boolean_member(ubo, "has_action"));
  const std::string extension = std::string("chrome-extension://") + json_object_get_string_member(ubo, "id") + "/";
  const auto popup_url = extension + json_object_get_string_member(ubo, "popup");
  g_assert_nonnull(g_getenv("JUPITER_TEST_URL"));
  const std::string fixture = g_getenv("JUPITER_TEST_URL");
  auto a = make_window(GTK_APPLICATION(app)), b = make_window(GTK_APPLICATION(app));
  auto *a1 = add_tab(&a, fixture + "?owner=a1");
  gtk_window_present(GTK_WINDOW(a.window));
  wait_for([&] { return !ephy_engine_web_view_is_loading(a1) && !g_strcmp0(ephy_engine_web_view_get_title(a1), "CEF native contract"); });
  auto *a2 = add_tab(&a, fixture + "?owner=a2");
  auto other_host = fixture;
  const auto address = other_host.find("127.0.0.1");
  g_assert_true(address != std::string::npos);
  other_host.replace(address, strlen("127.0.0.1"), "localhost");
  auto *b1 = add_tab(&b, other_host + "?owner=b1");
  gtk_window_present(GTK_WINDOW(b.window));
  wait_for([&] { return !ephy_engine_web_view_is_loading(a2) && !ephy_engine_web_view_is_loading(b1) &&
    !g_strcmp0(ephy_engine_web_view_get_title(a2), "CEF native contract") && !g_strcmp0(ephy_engine_web_view_get_title(b1), "CEF native contract"); });
  // Wait for genuine Chrome tab IDs reported by the installed integration extension.
  wait_for([&] { return ephy_engine_web_view_get_extension_tab_id(a1) >= 0 &&
    ephy_engine_web_view_get_extension_tab_id(a2) >= 0 && ephy_engine_web_view_get_extension_tab_id(b1) >= 0; });
  if (argc > 1 && !strcmp(argv[1], "--dashboard-diagnostic")) {
    auto *dashboard = add_tab(&b, extension + "dashboard.html");
    wait_for([&] { const char *title = ephy_engine_web_view_get_title(dashboard);
      return title && strstr(title, "Dashboard"); });
    const auto details = evaluate(dashboard, R"JS(new Promise(resolve=>setTimeout(()=>{
      const report={url:location.href,ready:document.readyState,title:document.title,
        html:document.body?.innerHTML.length,text:document.body?.innerText.slice(0,1000),
        style:document.body&&getComputedStyle(document.body).cssText,
        frames:[...document.querySelectorAll('iframe')].map(frame=>{
          try{return {src:frame.src,text:frame.contentDocument?.body?.innerText.slice(0,500)}}
          catch(error){return {src:frame.src,error:String(error)}}})};
      try{report.nativeURL=chrome.runtime.getURL('');report.manifest=chrome.runtime.getManifest().version}
      catch(error){report.nativeError=String(error)}
      resolve(JSON.stringify(report));
    },5000)))JS");
    g_print("DASHBOARD_DOM:%s\n", details.c_str());
    bool reload_finished = false;
    auto reload_handler = g_signal_connect(dashboard, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, bool *finished) {
      if (event == WEBKIT_LOAD_FINISHED) *finished = true;
    }), &reload_finished);
    ephy_engine_web_view_reload(dashboard);
    wait_for([&] { return reload_finished; });
    g_signal_handler_disconnect(dashboard, reload_handler);
    const auto reloaded = evaluate(dashboard, R"JS(new Promise(resolve=>setTimeout(()=>{
      const report={url:location.href,ready:document.readyState,title:document.title,
        text:document.body?.innerText.slice(0,500),frames:[...document.querySelectorAll('iframe')].map(f=>f.src)};
      try{report.nativeURL=chrome.runtime.getURL('');report.manifest=chrome.runtime.getManifest().version}
      catch(error){report.nativeError=String(error)}
      resolve(JSON.stringify(report));
    },8000)))JS");
    g_print("DASHBOARD_RELOADED_DOM:%s\n", reloaded.c_str());
    gtk_window_destroy(GTK_WINDOW(a.window)); gtk_window_destroy(GTK_WINDOW(b.window));
    wait_for([] { return Browser::active_count == 0; });
    ephy_engine_shutdown(); g_object_unref(app);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--owner-reload-diagnostic")) {
    auto *dashboard = add_tab(&b, extension + "dashboard.html");
    wait_for([&] { const char *title = ephy_engine_web_view_get_title(dashboard);
      return title && strstr(title, "Dashboard"); });
    std::string readiness;
    wait_for([&] {
      readiness = evaluate(dashboard, R"JS((async()=>{try {
        chrome.runtime.getURL(''); chrome.runtime.getManifest();
        const lists = await vAPI.messaging.send('dashboard', {what:'getLists'});
        return lists?.userFiltersPath ? 'ready' : 'background pending';
      } catch(error) { return String(error); }})())JS", true);
      return readiness == "ready";
    });
    g_print("OWNER_RELOAD_DIAGNOSTIC: genuine uBlock startup/native APIs ready\n");
    // Causal diagnostic only: restore native HTTP navigation events lost during fresh extension reload.
    for (auto *owner : {a1, a2, b1}) {
      bool loaded = false;
      auto handler = g_signal_connect(owner, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, bool *loaded) {
        if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
      }), &loaded);
      ephy_engine_web_view_reload(owner);
      wait_for([&] { return loaded; });
      g_signal_handler_disconnect(owner, handler);
    }
    adw_tab_view_close_page(b.tabs, adw_tab_view_get_page(b.tabs, GTK_WIDGET(dashboard)));
    wait_for([&] { return adw_tab_view_get_n_pages(b.tabs) == 1; });
    adw_tab_view_set_selected_page(b.tabs, adw_tab_view_get_page(b.tabs, GTK_WIDGET(b1)));
    g_print("OWNER_RELOAD_DIAGNOSTIC: normal HTTP owners reloaded after extension startup\n");
  }
  auto *private_session = ephy_engine_session_new(true);
  auto *private_view = EPHY_ENGINE_VIEW(g_object_ref_sink(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", private_session, nullptr)));
  auto *unbound_popup = EPHY_ENGINE_VIEW(g_object_ref_sink(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr)));
  g_assert_false(ephy_engine_web_view_set_extension_popup(unbound_popup, private_view));
  g_assert_false(ephy_engine_web_view_set_extension_popup(private_view, a2));
  g_object_unref(private_view); g_object_unref(private_session); g_object_unref(unbound_popup);
  GtkWindow *popup_a_window, *popup_b_window;
  auto *popup_a = make_popup(GTK_APPLICATION(app), a2, popup_url, &popup_a_window);
  auto *popup_b = make_popup(GTK_APPLICATION(app), b1, popup_url, &popup_b_window);
  auto active = [](EphyEngineView *view) {
    return evaluate(view, "chrome.tabs.query({active:true,currentWindow:true}).then(t=>JSON.stringify(t.map(t=>({url:t.url,window:t.windowId}))))");
  };
  auto a_active = active(popup_a), b_active = active(popup_b);
  g_print("Popup A active: %s\nPopup B active: %s\n", a_active.c_str(), b_active.c_str());
  g_print("All tabs: %s\n", evaluate(popup_b, "chrome.tabs.query({}).then(JSON.stringify)").c_str());
  g_assert_nonnull(strstr(a_active.c_str(), "owner=a2"));
  g_assert_null(strstr(a_active.c_str(), "owner=b1"));
  g_assert_nonnull(strstr(b_active.c_str(), "owner=b1"));
  g_assert_null(strstr(b_active.c_str(), "owner=a2"));
  g_assert_true(evaluate(popup_a, "chrome.tabs.query({}).then(t=>t.length)") == "3");
  g_print("CEF real uBlock popups: owner-relative currentWindow and popup exclusion: PASS\n");
  auto created = evaluate(popup_b, "(async()=>{try {const t=await chrome.tabs.create({url:chrome.runtime.getURL('dashboard.html')});return JSON.stringify({id:t.id,url:t.url,window:t.windowId})}catch(error){return 'CREATE_ERROR:'+error.message}})()");
  g_print("Created tab: %s\n", created.c_str());
  g_assert_true(b.opened_by == b1); g_assert_nonnull(b.created); g_assert_null(a.created);
  g_assert_true(gtk_widget_get_root(GTK_WIDGET(b.created)) == GTK_ROOT(b.window));
  if (argc > 1 && !strcmp(argv[1], "--owner-reload-diagnostic") && created.starts_with("CREATE_ERROR:")) {
    g_print("DASHBOARD_NATIVE_ID_DIAGNOSTIC: cached=%d loading=%d uri=%s\n",
      ephy_engine_web_view_get_extension_tab_id(b.created), ephy_engine_web_view_is_loading(b.created),
      ephy_engine_web_view_get_uri(b.created));
    const auto native = evaluate(b.created, R"JS(new Promise(resolve=>{
      const report={url:location.href,ready:document.readyState,bridge:!!globalThis.chrome?.tabs?.__jupiter,
        title:document.title,frames:[...document.querySelectorAll('iframe')].map(frame=>frame.src)};
      let finished=false;
      const done=()=>{if(finished)return;finished=true;clearTimeout(timer);resolve(JSON.stringify(report))};
      const timer=setTimeout(()=>{report.callbackTimeout=true;done()},2500);
      try {
        report.nativeURL=chrome.runtime.getURL('');report.version=chrome.runtime.getManifest().version;
        chrome.tabs.getCurrent(tab=>{
          const error=chrome.runtime.lastError;
          report.callbackError=error?.message??null;
          report.currentTabType=typeof tab;report.currentTab=tab??null;
          done();
        });
      } catch(error) {report.nativeError=String(error);done()}
    }))JS", true);
    g_print("DASHBOARD_NATIVE_ID_DIAGNOSTIC: %s\n", native.c_str());
  }
  g_assert_nonnull(strstr(created.c_str(), "dashboard.html"));
  g_autoptr(JsonParser) created_parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(created_parser, created.c_str(), created.size(), &error));
  g_assert_no_error(error);
  auto *created_tab = json_node_get_object(json_parser_get_root(created_parser));
  const int cached_id = ephy_engine_web_view_get_extension_tab_id(b.created);
  g_assert_cmpint(cached_id, >=, 0);
  g_assert_cmpint(json_object_get_int_member(created_tab, "id"), ==, cached_id);
  const auto native_id = evaluate(b.created, "new Promise(resolve=>chrome.tabs.getCurrent(tab=>{const error=chrome.runtime.lastError;resolve(error?'ERROR:'+error.message:String(tab?.id))}))");
  g_assert_true(native_id == std::to_string(cached_id));
  g_assert_true(evaluate(popup_a, "chrome.tabs.query({}).then(t=>t.length)") == "4");
  g_print("CEF real uBlock dashboard tabs.create: owning GTK window and native tab ID: PASS\n");
  // A translated header is insufficient: require the real settings iframe and initialized controls.
  std::string settings, previous_settings;
  wait_for([&] {
    settings = evaluate(b.created, R"JS((()=>{try {
      chrome.runtime.getURL('');
      if (!chrome.runtime.getManifest().version) return 'manifest pending';
      const frame = document.querySelector('#iframe');
      const pane = frame?.contentDocument;
      if (!pane || !frame.contentWindow.location.pathname.endsWith('/settings.html')) return 'settings frame pending';
      frame.contentWindow.chrome.runtime.getURL('');
      const control = pane.querySelector('input[data-setting-name="collapseBlocked"]');
      const label = pane.querySelector('[data-i18n="settingsCollapseBlockedPrompt"]');
      if (!control || control.disabled || !label || label.innerText.trim().length < 2 ||
          !pane.querySelector('select[data-setting-name="uiTheme"]') || !control.getClientRects().length) return 'settings controls pending';
      return 'ready';
    } catch(error) { return String(error); }})())JS", true);
    if (settings != previous_settings) {
      g_print("Dashboard settings readiness: %s\n", settings.c_str());
      previous_settings = settings;
    }
    return settings == "ready";
  });
  g_print("CEF real uBlock dashboard settings pane and native APIs: PASS\n");
  // Closing an owner must not redirect the stale popup into a different window.
  gtk_window_destroy(GTK_WINDOW(a.window));
  g_assert_true(evaluate(popup_a, "chrome.tabs.query({active:true,currentWindow:true}).then(t=>t.length)") == "0");
  g_assert_true(evaluate(popup_a, "chrome.tabs.create({url:'https://example.invalid/'}).then(()=>false,()=>true)") == "true");
  g_print("CEF closed popup owner and private owner rejection: PASS\n");
  gtk_window_destroy(popup_a_window); gtk_window_destroy(popup_b_window); gtk_window_destroy(GTK_WINDOW(b.window));
  wait_for([] { return Browser::active_count == 0; });
  ephy_engine_shutdown(); g_object_unref(app);
  return 0;
}
