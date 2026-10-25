/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <functional>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  if (!predicate()) g_error("External URI test timed out at %s:%u", location.file_name(), location.line());
}
static std::string evaluate(EphyEngineView *view, const std::string& script) {
  GAsyncResult *result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr,
    +[](GObject *, GAsyncResult *result, gpointer pointer) {
      *static_cast<GAsyncResult **>(pointer) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError *error = nullptr;
  auto *value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  g_assert_no_error(error); g_assert_nonnull(value);
  auto text = script_result_text(value);
  json_object_unref(value); g_object_unref(result);
  return text;
}
static void xdo(std::initializer_list<const char *> arguments) {
  std::vector<const char *> command{"xdotool"};
  command.insert(command.end(), arguments.begin(), arguments.end()); command.push_back(nullptr);
  GError *error = nullptr;
  auto *child = g_subprocess_newv(command.data(), G_SUBPROCESS_FLAGS_NONE, &error);
  g_assert_no_error(error); g_assert_nonnull(child);
  g_assert_true(g_subprocess_wait_check(child, nullptr, &error)); g_assert_no_error(error);
  g_object_unref(child);
}
static GtkWidget *surface(GtkWidget *widget) {
  if (GTK_IS_GL_AREA(widget)) return widget;
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto *found = surface(child)) return found;
  return nullptr;
}
static std::string quote(const std::string& text) {
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(node, text.c_str());
  g_autofree char *json = json_to_string(node, false);
  return json;
}
static constexpr const char *same_target = "jupiter-test-open://app/item?name=one%20two&value=%2F#section";
static constexpr const char *blank_target = "jupiter-test-open://app/other?request=second";
struct Request {
  EphyEngineView *view;
  std::string uri, context;
  bool initiator_known, private_mode;
  GCancellable *token;
  unsigned cancellations = 0;
  gulong cancelled_handler = 0;
  ~Request() { g_signal_handler_disconnect(token, cancelled_handler); g_object_unref(token); }
};
struct Requests {
  std::vector<std::unique_ptr<Request>> entries;
  std::map<EphyEngineView *, unsigned> starts;
  static gboolean received(EphyEngineView *view, const char *uri, const char *context, gboolean known,
                            GCancellable *token, Requests *self) {
    EphyEngineSession *session = nullptr;
    g_object_get(view, "network-session", &session, nullptr);
    auto request = std::make_unique<Request>();
    request->view = view; request->uri = uri; request->context = context;
    request->initiator_known = known;
    request->private_mode = ephy_engine_session_is_ephemeral(session);
    request->token = G_CANCELLABLE(g_object_ref(token));
    request->cancelled_handler = g_signal_connect(token, "cancelled", G_CALLBACK(+[](GCancellable *, Request *request) {
      ++request->cancellations;
    }), request.get());
    g_clear_object(&session);
    self->entries.push_back(std::move(request));
    // Hold the actual engine request; this test never launches an external application.
    return true;
  }
};
struct Page { AdwApplicationWindow *window; EphyEngineView *view; std::string title, uri; };
static void navigate(Page& page, const std::string& uri) {
  bool loaded = false;
  auto handler = g_signal_connect(page.view, "load-changed", G_CALLBACK(+[](EphyEngineView *, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(page.view, uri.c_str());
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(page.view, handler);
  page.uri = uri;
}
static Page page(GtkApplication *app, EphyEngineSession *session, const std::string& uri,
                 const char *title, Requests& requests) {
  Page page{ADW_APPLICATION_WINDOW(adw_application_window_new(app)),
    EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr)), title, uri};
  gtk_window_set_title(GTK_WINDOW(page.window), title);
  gtk_window_set_default_size(GTK_WINDOW(page.window), 760, 540);
  adw_application_window_set_content(page.window, GTK_WIDGET(page.view));
  g_signal_connect(page.view, "open-external-uri", G_CALLBACK(Requests::received), &requests);
  g_signal_connect(page.view, "load-changed", G_CALLBACK(+[](EphyEngineView *view, WebKitLoadEvent event, Requests *requests) {
    if (event == WEBKIT_LOAD_STARTED) ++requests->starts[view];
  }), &requests);
  gtk_window_present(GTK_WINDOW(page.window));
  navigate(page, uri);
  return page;
}
static void install_links(Page& page) {
  auto installed = evaluate(page.view, R"JS((async()=>{
    document.body.innerHTML='';window.externalClicks=[];window.childTrustedClicks=0;
    for(const [id,target,blank] of [['same',)JS" + quote(same_target) + R"JS(,false],['blank',)JS" + quote(blank_target) + R"JS(,true]]) {
      const link=document.createElement('a');link.id=id;link.href=target;link.textContent=id;
      link.style='display:block;padding:20px';if(blank)link.target='_blank';document.body.append(link);
    }
    document.addEventListener('click',event=>externalClicks.push({id:event.target.id,trusted:event.isTrusted}));
    const child=document.createElement('iframe');child.id='child';
    child.srcdoc='<a id="child-link" target="_blank" href="jupiter-test-open://app/iframe">Child</a>';
    const loaded=new Promise(resolve=>child.onload=resolve);document.body.append(child);await loaded;
    child.contentDocument.querySelector('a').onclick=event=>{if(event.isTrusted)window.childTrustedClicks++};
    return 'installed';
  })())JS");
  g_assert_true(installed == "installed");
}
static void focus(Page& page, const std::string& expression) {
  const auto pattern = "^" + page.title + "$";
  xdo({"search", "--sync", "--name", pattern.c_str(), "windowfocus", "--sync"});
  g_assert_nonnull(surface(GTK_WIDGET(page.view)));
  gtk_widget_grab_focus(surface(GTK_WIDGET(page.view)));
  g_assert_true(evaluate(page.view, expression + ";true") == "true");
}
static Request *activate(Page& page, Requests& requests, const char *id, const char *target, bool known, bool private_mode) {
  const auto count = requests.entries.size();
  const auto starts = requests.starts[page.view];
  auto *resource = ephy_engine_web_view_get_main_resource(page.view);
  g_object_ref(resource);
  focus(page, "document.getElementById(" + quote(id) + ").focus()");
  // X11 -> GTK -> CEF: the page receives a genuine trusted keyboard activation.
  xdo({"key", "Return"});
  wait_for([&] { return requests.entries.size() > count; });
  g_assert_cmpuint(requests.entries.size(), ==, count + 1);
  auto *request = requests.entries.back().get();
  g_assert_true(request->view == page.view);
  g_assert_true(request->uri == target);
  g_assert_true(request->context == page.uri);
  g_assert_cmpint(request->initiator_known, ==, known);
  g_assert_cmpint(request->private_mode, ==, private_mode);
  g_assert_false(g_cancellable_is_cancelled(request->token));
  g_assert_cmpuint(request->cancellations, ==, 0);
  g_assert_true(evaluate(page.view, "externalClicks.some(event=>event.id===" + quote(id) + "&&event.trusted)") == "true");
  g_assert_true(ephy_engine_web_view_get_main_resource(page.view) == resource);
  g_assert_true(std::string(ephy_engine_web_view_get_uri(page.view)) == page.uri);
  g_assert_cmpuint(requests.starts[page.view], ==, starts);
  g_object_unref(resource);
  return request;
}
static void cancelled(Request *request) {
  wait_for([&] { return g_cancellable_is_cancelled(request->token); });
  g_assert_cmpuint(request->cancellations, ==, 1);
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int child = ephy_engine_execute_process(argc, argv); if (child >= 0) return child;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.ExternalURITest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-external-uri-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  g_assert_nonnull(g_getenv("JUPITER_TEST_URL"));
  const std::string fixture = g_getenv("JUPITER_TEST_URL");
  Requests requests;
  auto normal = page(GTK_APPLICATION(app), ephy_engine_session_get_default(), fixture + "?external=normal",
                     "Jupiter external normal", requests);
  install_links(normal);
  const auto starts = requests.starts[normal.view];
  auto *resource = ephy_engine_web_view_get_main_resource(normal.view);
  g_object_ref(resource);
  // These run before any trusted gesture, so there is no inherited transient activation.
  evaluate(normal.view, "location.href=" + quote(same_target) + ";'scheduled'");
  evaluate(normal.view, "window.open(" + quote(blank_target) + ",'_blank');'scheduled'");
  evaluate(normal.view, "(()=>{const frame=document.createElement('iframe');frame.src='jupiter-test-open://app/automatic';document.body.append(frame);return true})()");
  evaluate(normal.view, "new Promise(resolve=>setTimeout(()=>resolve('settled'),500))");
  g_assert_true(requests.entries.empty());
  g_assert_true(ephy_engine_web_view_get_main_resource(normal.view) == resource);
  g_assert_cmpuint(requests.starts[normal.view], ==, starts);
  focus(normal, "document.querySelector('#child').contentDocument.querySelector('a').focus()");
  xdo({"key", "Return"});
  wait_for([&] { return evaluate(normal.view, "childTrustedClicks") == "1"; });
  evaluate(normal.view, "new Promise(resolve=>setTimeout(resolve,250))");
  g_assert_true(requests.entries.empty());
  g_assert_true(ephy_engine_web_view_get_main_resource(normal.view) == resource);
  g_object_unref(resource);
  g_print("Native external URI: no-gesture scripts, automatic iframe and trusted subframe popup denied: PASS\n");
  auto *first = activate(normal, requests, "same", same_target, false, false);
  auto *second = activate(normal, requests, "blank", blank_target, true, false);
  cancelled(first);
  g_print("Native external URI: trusted GTK activation, exact target/committed context, source flags and supersession: PASS\n");
  navigate(normal, fixture + "?external=navigated"); cancelled(second); install_links(normal);
  auto *hidden = activate(normal, requests, "same", same_target, false, false);
  gtk_widget_set_visible(GTK_WIDGET(normal.view), false); cancelled(hidden);
  gtk_widget_set_visible(GTK_WIDGET(normal.view), true);
  wait_for([&] { return gtk_widget_get_mapped(GTK_WIDGET(normal.view)); });
  auto *unrooted = activate(normal, requests, "same", same_target, false, false);
  g_object_ref(normal.view);
  adw_application_window_set_content(normal.window, nullptr); cancelled(unrooted);
  adw_application_window_set_content(normal.window, GTK_WIDGET(normal.view)); g_object_unref(normal.view);
  wait_for([&] { return gtk_widget_get_mapped(GTK_WIDGET(normal.view)); });
  auto *normal_pending = activate(normal, requests, "same", same_target, false, false);
  auto *private_session = ephy_engine_session_new(true);
  auto private_page = page(GTK_APPLICATION(app), private_session, fixture + "?external=private",
                           "Jupiter external private", requests);
  g_object_unref(private_session); install_links(private_page);
  auto *private_pending = activate(private_page, requests, "same", same_target, false, true);
  g_assert_false(g_cancellable_is_cancelled(normal_pending->token));
  auto *closing = activate(normal, requests, "blank", blank_target, true, false);
  cancelled(normal_pending); g_assert_false(g_cancellable_is_cancelled(private_pending->token));
  gtk_widget_set_visible(GTK_WIDGET(private_page.view), false); cancelled(private_pending);
  g_assert_false(g_cancellable_is_cancelled(closing->token));
  gtk_window_destroy(GTK_WINDOW(normal.window)); cancelled(closing);
  gtk_window_destroy(GTK_WINDOW(private_page.window));
  wait_for([] { return Browser::active_count == 0; });
  for (const auto& request : requests.entries) g_assert_cmpuint(request->cancellations, ==, 1);
  g_print("Native external URI: navigation/hide/unroot/close cancellation and private-session isolation: PASS\n");
  ephy_engine_shutdown(); g_object_unref(app);
  return 0;
}
