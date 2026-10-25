/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <source_location>
#include <string>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView *);

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  const auto deadline = g_get_monotonic_time() + 25 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  if (!predicate()) g_error("Native popup test timed out at %s:%u", location.file_name(), location.line());
}

static std::string evaluate(EphyEngineView *view, const char *script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
    +[](GObject *, GAsyncResult *result, gpointer pointer) {
      *static_cast<GAsyncResult **>(pointer) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(value);
  const auto text = script_result_text(value);
  json_object_unref(value);
  g_object_unref(result);
  return text;
}

struct Popup {
  GtkApplication *app;
  GtkWindow *window = nullptr;
  EphyEngineView *view = nullptr;
  EphyEnginePopupProperties requested{};
  EphyEnginePopupProperties resized{};
  unsigned created = 0, ready = 0, bounds = 0, closed = 0;
};

static GtkWidget *create_popup(EphyEngineView *owner,
                               const EphyEnginePopupProperties *properties,
                               Popup *popup) {
  g_assert_cmpuint(++popup->created, ==, 1);
  g_assert_nonnull(properties);
  popup->requested = *properties;
  popup->view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "related-view", owner, nullptr));
  g_assert_null(ephy_engine_view_browser(popup->view).get());
  popup->window = GTK_WINDOW(adw_application_window_new(popup->app));
  gtk_window_set_default_size(popup->window, properties->geometry.width, properties->geometry.height);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(popup->window), GTK_WIDGET(popup->view));
  g_signal_connect(popup->view, "ready-to-show", G_CALLBACK(+[](EphyEngineView *view, Popup *popup) {
    g_assert_nonnull(ephy_engine_view_browser(view).get());
    ++popup->ready;
  }), popup);
  g_signal_connect(popup->view, "popup-geometry-changed", G_CALLBACK(+[](EphyEngineView *view, Popup *popup) {
    popup->resized = *ephy_engine_web_view_get_popup_properties(view);
    ++popup->bounds;
    gtk_window_set_default_size(popup->window, popup->resized.geometry.width, popup->resized.geometry.height);
  }), popup);
  g_signal_connect(popup->view, "close", G_CALLBACK(+[](EphyEngineView *, Popup *popup) {
    ++popup->closed;
    auto *window = popup->window;
    popup->window = nullptr;
    popup->view = nullptr;
    gtk_window_destroy(window);
  }), popup);
  // Mapping during create-popup must not start a second browser before CEF binds its child.
  gtk_window_present(popup->window);
  return GTK_WIDGET(popup->view);
}

static GtkWindow *exercise_popup(GtkApplication *app, EphyEngineSession *session,
                                 const char *url, CefRefPtr<CefRequestContext> normal_context) {
  const bool ephemeral = ephy_engine_session_is_ephemeral(session);
  auto *window = GTK_WINDOW(adw_application_window_new(app));
  gtk_window_set_default_size(window, 700, 500);
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(view));
  bool loaded = false;
  auto load_handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, url);
  gtk_window_present(window);
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, load_handler);

  const auto initial_storage = evaluate(view, "localStorage.getItem('jupiterPopupBoundary')");
  if (ephemeral) g_assert_true(initial_storage == "null");
  g_assert_true(evaluate(view, ephemeral
    ? "localStorage.setItem('jupiterPopupBoundary','private');true"
    : "localStorage.setItem('jupiterPopupBoundary','regular');true") == "true");
  Popup popup{app};
  const auto create_handler = g_signal_connect(view, "create-popup", G_CALLBACK(create_popup), &popup);
  unsigned main_bounds = 0;
  const auto bounds_handler = g_signal_connect(view, "popup-geometry-changed",
    G_CALLBACK(+[](EphyEngineView *, unsigned *count) { ++*count; }), &main_bounds);
  g_assert_true(evaluate(view, R"JS((()=>{
    window.popupOwner='native opener';
    const button=document.createElement('button');
    button.textContent='Open native popup';
    button.style='position:fixed;left:20px;top:20px;width:200px;height:60px;z-index:10000';
    button.onclick=event=>{
      window.popupClickTrusted=event.isTrusted;
      window.nativeChild=window.open('about:blank','', 'popup,width=420,height=300');
      if(nativeChild) {
        nativeChild.document.title='Retained native popup';
        nativeChild.document.body.textContent='written through original opener';
        nativeChild.retainedValue='retained child global';
      }
    };
    document.body.append(button);
    return true;
  })())JS") == "true");
  const auto baseline = Browser::active_count;
  auto browser = ephy_engine_view_browser(view);
  auto context = browser->GetHost()->GetRequestContext();
  if (ephemeral) {
    g_assert_false(context->IsSame(normal_context));
    g_assert_false(context->IsSharingWith(normal_context));
  }
  browser->GetHost()->SetFocus(true);
  CefMouseEvent mouse;
  mouse.x = 60;
  mouse.y = 45;
  browser->GetHost()->SendMouseClickEvent(mouse, MBT_LEFT, false, 1);
  browser->GetHost()->SendMouseClickEvent(mouse, MBT_LEFT, true, 1);
  wait_for([&] { return popup.view && popup.ready; });
  g_assert_cmpuint(popup.ready, ==, 1);
  g_assert_cmpuint(Browser::active_count, ==, baseline + 1);
  g_assert_true(popup.requested.is_popup);
  g_assert_true(popup.requested.width_set);
  g_assert_true(popup.requested.height_set);
  g_assert_cmpint(popup.requested.geometry.width, ==, 420);
  g_assert_cmpint(popup.requested.geometry.height, ==, 300);
  g_assert_false(popup.requested.x_set);
  g_assert_false(popup.requested.y_set);
  g_assert_true(evaluate(view, "popupClickTrusted && nativeChild.opener===window") == "true");
  wait_for([&] {
    return evaluate(popup.view, "document.body.textContent==='written through original opener' && retainedValue==='retained child global' && opener.popupOwner==='native opener'") == "true";
  });
  g_assert_true(evaluate(popup.view, "localStorage.getItem('jupiterPopupBoundary')") == (ephemeral ? "private" : "regular"));
  EphyEngineSession *child_session = nullptr;
  g_object_get(popup.view, "network-session", &child_session, nullptr);
  g_assert_true(child_session == session);
  g_assert_cmpint(ephy_engine_session_is_ephemeral(child_session), ==, ephemeral);
  g_object_unref(child_session);
  auto child_browser = ephy_engine_view_browser(popup.view);
  g_assert_true(child_browser->GetHost()->GetRequestContext()->IsSame(context));
  g_print("Native %s popup: trusted creation, requested geometry, ready, retained opener DOM and session: PASS\n",
          ephemeral ? "private" : "regular");

  const auto previous_bounds = popup.bounds;
  g_assert_true(evaluate(popup.view, "window.resizeTo(520,360);true") == "true");
  wait_for([&] { return popup.bounds > previous_bounds; });
  g_assert_cmpint(popup.resized.geometry.width, ==, 520);
  g_assert_cmpint(popup.resized.geometry.height, ==, 360);
  g_assert_true(popup.resized.width_set);
  g_assert_true(popup.resized.height_set);
  g_assert_true(evaluate(view, "new Promise(resolve=>{window.resizeTo(610,410);setTimeout(()=>resolve(true),250)})") == "true");
  g_assert_cmpuint(main_bounds, ==, 0);
  g_assert_false(ephy_engine_web_view_get_popup_properties(view)->is_popup);
  g_print("Native %s popup: actual resizeTo bounds forwarded only for native child: PASS\n",
          ephemeral ? "private" : "regular");

  g_assert_true(evaluate(view, "nativeChild.close();true") == "true");
  wait_for([&] { return popup.closed == 1 && Browser::active_count == baseline; });
  g_assert_null(popup.view);
  g_assert_true(evaluate(view, "nativeChild.closed && popupOwner==='native opener'") == "true");
  g_assert_true(ephy_engine_view_browser(view)->IsSame(browser));
  g_signal_handler_disconnect(view, create_handler);
  g_signal_handler_disconnect(view, bounds_handler);
  g_print("Native %s popup: JavaScript close closes child and preserves opener: PASS\n",
          ephemeral ? "private" : "regular");
  return window;
}

int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  const int child = ephy_engine_execute_process(argc, argv);
  if (child >= 0) return child;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.PopupTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error));
  g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-native-popup-XXXXXX";
  g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true));
  disable_diagnostic_core_dumps();
  const char *url = g_getenv("JUPITER_TEST_URL");
  g_assert_nonnull(url);
  auto *regular = ephy_engine_session_get_default();
  auto *normal_window = exercise_popup(GTK_APPLICATION(app), regular, url, nullptr);
  auto *private_session = ephy_engine_session_new(true);
  auto *private_window = exercise_popup(GTK_APPLICATION(app), private_session, url, ephy_engine_session_context(regular));
  auto *normal_view = EPHY_ENGINE_VIEW(adw_application_window_get_content(ADW_APPLICATION_WINDOW(normal_window)));
  g_assert_true(evaluate(normal_view, "localStorage.getItem('jupiterPopupBoundary')") == "regular");
  gtk_window_destroy(private_window);
  gtk_window_destroy(normal_window);
  wait_for([] { return Browser::active_count == 0; });
  g_object_unref(private_session);
  ephy_engine_shutdown();
  g_object_unref(app);
  std::filesystem::remove_all(profile);
  return 0;
}
