/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "ephy-engine-scheme.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "page-messages.hpp"
#include "include/cef_process_message.h"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);

static void wait_for(const std::function<bool()>& predicate) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  g_assert_true(predicate());
}
static std::string evaluate(EphyEngineView* view, const char* script, const char* world = nullptr) {
  GAsyncResult* result = nullptr;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, world, nullptr, nullptr,
    +[](GObject*, GAsyncResult* result, gpointer data) {
      *static_cast<GAsyncResult**>(data) = G_ASYNC_RESULT(g_object_ref(result));
    }, &result);
  wait_for([&] { return result; });
  GError* error = nullptr;
  auto* value = ephy_engine_web_view_evaluate_javascript_finish(view, result, &error);
  if (error) g_error("Message test script failed: %s; %s", script, error->message);
  auto text = script_result_text(value);
  json_object_unref(value); g_object_unref(result);
  return text;
}
static void load(EphyEngineView* view, const std::function<void()>& navigate) {
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool* loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  navigate();
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, handler);
}
static bool send(EphyEngineView* view, const std::string& uri, const char* json) {
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, json, -1, nullptr));
  return ephy_engine_web_view_send_page_message(view, uri.c_str(), "probe-reply", json_parser_get_root(parser));
}
static void event(CefRefPtr<CefFrame> frame, const std::string& token, const std::string& uri,
                  const std::string& name = "probe-reply", const std::string& json = "null") {
  auto message = CefProcessMessage::Create("gnosis.internal-page.event");
  auto values = message->GetArgumentList();
  values->SetString(0, token); values->SetString(1, uri);
  values->SetString(2, name); values->SetString(3, json);
  frame->SendProcessMessage(PID_RENDERER, message);
}
static EphyEngineView* page(GtkApplication* app, EphyEngineSession* session) {
  auto* window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  auto* view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, "network-session", session, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  gtk_window_present(GTK_WINDOW(window));
  return view;
}
struct Messages {
  std::vector<std::string> received;
  void connect(EphyEngineView* view) {
    g_signal_connect(view, "page-message::probe", G_CALLBACK(+[](EphyEngineView*, JsonNode* message, Messages* self) {
      g_autofree char* json = json_to_string(message, false);
      self->received.emplace_back(json);
    }), this);
    ephy_engine_web_view_register_page_message_handler(view, "probe");
  }
};
int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  const char* mime_dirs[] = {nullptr};
  g_content_type_set_mime_dirs(mime_dirs);
  g_autofree char* unknown_svg = g_content_type_guess("probe.svg", nullptr, 0, nullptr);
  g_assert_true(g_content_type_is_unknown(unknown_svg));
  auto* app = adw_application_new("com.neg-zero.Gnosis.PageMessagesTest", G_APPLICATION_NON_UNIQUE);
  GError* error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/gnosis-page-messages-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  ephy_engine_register_uri_scheme("ephy-resource", +[](EphyEngineSchemeRequest* request, gpointer) {
    const char* content = g_str_has_suffix(ephy_engine_uri_scheme_request_get_path(request), ".svg")
      ? "<svg xmlns='http://www.w3.org/2000/svg' width='17' height='19'></svg>"
      : "body { color: rgb(12, 34, 56); }";
    auto* stream = g_memory_input_stream_new_from_data(g_strdup(content), -1, g_free);
    ephy_engine_uri_scheme_request_finish(request, stream, -1, nullptr);
    g_object_unref(stream);
  }, nullptr, nullptr);
  auto* view = page(GTK_APPLICATION(app), ephy_engine_session_get_default());
  Messages normal; normal.connect(view);
  auto http = std::string(g_getenv("GNOSIS_TEST_URL")) + "script-fixture";
  load(view, [&] { ephy_engine_web_view_load_uri(view, http.c_str()); });
  g_assert_true(evaluate(view, "typeof Gnosis") == "undefined");
  g_assert_false(send(view, http, "null"));
  g_assert_true(evaluate(view, "typeof Gnosis", "private-world") == "undefined");
  g_print("Ordinary websites and isolated worlds have no internal-page API: PASS\n");

  const char* html = R"HTML(<!doctype html><title>Native internal messages</title>
    <script>Gnosis.postMessage('probe',{text:'Unicode: ąß🪐',items:[null,42,false]});</script>
    <img id=icon src="ephy-resource:///probe.svg">
    <iframe src="data:text/html,%3Cscript%3Edocument.title=typeof%20Gnosis%3C/script%3E#gnosis-alternate-child"></iframe>)HTML";
  load(view, [&] { ephy_engine_web_view_load_alternate_html(view, html, "https://failed.example.test/", nullptr); });
  wait_for([&] { return normal.received.size() == 1; });
  g_assert_true(normal.received[0] == "{\"text\":\"Unicode: ąß🪐\",\"items\":[null,42,false]}");
  g_assert_true(evaluate(view, "Object.isFrozen(Gnosis)&&typeof __gnosisInternalMessage==='undefined'") == "true");
  g_assert_true(evaluate(view, "typeof Gnosis", "private-world") == "undefined");
  g_assert_true(evaluate(view, "typeof webkit") == "undefined");
  g_assert_true(evaluate(view, "icon.complete&&icon.naturalWidth===17&&icon.naturalHeight===19") == "true");
  g_assert_true(evaluate(view, "(()=>{try{Gnosis.postMessage('probe',1n)}catch(e){return e.name}})()") == "TypeError");
  g_assert_true(evaluate(view, "(()=>{try{Gnosis.postMessage('probe','x'.repeat(1024*1024+1))}catch(e){return e.message}})()") == "Internal page message is too large.");
  evaluate(view, "Gnosis.postMessage('unregistered','ignored');true");
  evaluate(view, "Gnosis.postMessage('probe',{another:true});true");
  wait_for([&] { return normal.received.size() == 2; });
  g_assert_true(normal.received.back() == "{\"another\":true}");
  g_print("Native context-created messaging runs before page scripts, preserves JSON/Unicode, isolates worlds, and bounds payloads: PASS\n");

  auto cef = ephy_engine_view_browser(view);
  CefRefPtr<Browser> native = static_cast<Browser*>(cef->GetHost()->GetClient().get());
  const auto alternate_uri = cef->GetMainFrame()->GetURL().ToString();
  const auto alternate_token = native->page_message_context.substr(0, 36);
  evaluate(view, R"JS(globalThis.nativeEvents=[];
    globalThis.nativeListener=e=>nativeEvents.push({detail:e.detail,custom:e instanceof CustomEvent});
    Gnosis.addEventListener('probe-reply',nativeListener);
    dispatchEvent(new CustomEvent('probe-reply',{detail:'public-event'}));true)JS");
  g_assert_false(send(view, "https://failed.example.test/", "null"));
  g_assert_false(send(view, "ephy-about:overview", "null"));
  g_assert_false(PageMessages::send(*native, alternate_uri, std::string(129, 'x'), "null"));
  g_assert_false(PageMessages::send(*native, alternate_uri, "probe-reply", std::string(1024*1024+1, 'x')));
  g_assert_false(PageMessages::send(*native, alternate_uri, "probe-reply", "not-json"));
  // Malformed, foreign-document and oversized renderer deliveries must not reach the private target.
  event(cef->GetMainFrame(), "00000000-0000-4000-8000-000000000000", alternate_uri);
  event(cef->GetMainFrame(), alternate_token, "ephy-about:overview");
  event(cef->GetMainFrame(), alternate_token, alternate_uri, std::string(129, 'x'));
  event(cef->GetMainFrame(), alternate_token, alternate_uri, "probe-reply", std::string(1024*1024+1, 'x'));
  std::vector<CefString> identifiers;
  cef->GetFrameIdentifiers(identifiers);
  bool child_checked = false;
  for (const auto& identifier : identifiers) {
    auto child = cef->GetFrameByIdentifier(identifier);
    if (child && !child->IsMain()) { event(child, alternate_token, alternate_uri); child_checked = true; }
  }
  g_assert_true(child_checked);
  g_assert_true(send(view, alternate_uri, R"JSON({"text":"Native: ąß🪐","items":[null,42,false]})JSON"));
  wait_for([&] { return evaluate(view, "nativeEvents.length") == "1"; });
  g_assert_true(evaluate(view, "JSON.stringify(nativeEvents)") ==
    "[{\"detail\":{\"text\":\"Native: ąß🪐\",\"items\":[null,42,false]},\"custom\":true}]");
  evaluate(view, "Gnosis.removeEventListener('probe-reply',nativeListener);Gnosis.addEventListener('probe-reply',nativeListener,{once:true});true");
  g_assert_true(send(view, alternate_uri, "1"));
  g_assert_true(send(view, alternate_uri, "2"));
  wait_for([&] { return evaluate(view, "nativeEvents.length") == "2"; });
  g_assert_true(evaluate(view, "nativeEvents[1].detail") == "1");
  g_assert_true(evaluate(view, "typeof Gnosis", "private-world") == "undefined");
  g_print("Native replies deliver private CustomEvent details; URI, token, frame and payload checks reject foreign events: PASS\n");

  // A forged alternate-page marker does not grant access to the browser's own document registry.
  const char* forged = "data:text/html;charset=utf-8;base64,PHNjcmlwdD5KdXBpdGVyLnBvc3RNZXNzYWdlKCdwcm9iZScsJ2ZvcmdlZCcpPC9zY3JpcHQ+#gnosis-alternate-forged";
  load(view, [&] { ephy_engine_web_view_load_uri(view, forged); });
  evaluate(view, "Gnosis.postMessage('probe','forged-again');true");
  g_assert_false(send(view, forged, "null"));
  g_assert_false(send(view, alternate_uri, "null"));
  // Wait for a subsequent authorized message to drain earlier renderer IPC.
  load(view, [&] { ephy_engine_web_view_load_alternate_html(view, "<title>Second internal page</title>", "https://second.example.test/", nullptr); });
  ephy_engine_web_view_unregister_page_message_handler(view, "probe");
  evaluate(view, "Gnosis.postMessage('probe','unregistered');true");
  ephy_engine_web_view_register_page_message_handler(view, "probe");
  evaluate(view, "Gnosis.postMessage('probe','registered-again');true");
  wait_for([&] { return normal.received.size() >= 3; });
  g_assert_cmpuint(normal.received.size(), ==, 3);
  g_assert_true(normal.received.back() == "\"registered-again\"");
  g_print("Forged data URLs cannot invoke internal handlers; unregister/re-register applies to the current document: PASS\n");

  ephy_engine_register_uri_scheme("ephy-about", +[](EphyEngineSchemeRequest* request, gpointer) {
    const char* content = "<!doctype html><title>Internal scheme</title><link rel=stylesheet href='ephy-resource:///probe.css'><script>Gnosis.postMessage('probe','scheme');</script>";
    auto* stream = g_memory_input_stream_new_from_data(g_strdup(content), -1, g_free);
    ephy_engine_uri_scheme_request_finish(request, stream, -1, "text/html");
    g_object_unref(stream);
  }, nullptr, nullptr);
  load(view, [&] { ephy_engine_web_view_load_uri(view, "ephy-about:bridge-test"); });
  wait_for([&] { return normal.received.size() == 4; });
  g_assert_true(normal.received.back() == "\"scheme\"");
  g_assert_true(evaluate(view, "getComputedStyle(document.body).color") == "rgb(12, 34, 56)");
  g_print("Without a system MIME database, native resource replies infer SVG/CSS MIME types and render alternate-page images/internal stylesheets: PASS\n");

  auto stale_token = native->page_message_context.substr(0, 36);
  load(view, [&] { ephy_engine_web_view_reload(view); });
  wait_for([&] { return normal.received.size() == 5; });
  evaluate(view, "globalThis.nativeEvents=[];Gnosis.addEventListener('probe-reply',e=>nativeEvents.push(e.detail));true");
  event(cef->GetMainFrame(), stale_token, "ephy-about:bridge-test");
  event(cef->GetMainFrame(), alternate_token, alternate_uri);
  g_assert_true(send(view, "ephy-about:bridge-test", "\"current-document\""));
  wait_for([&] { return evaluate(view, "JSON.stringify(nativeEvents)") == "[\"current-document\"]"; });
  g_print("Reloading the same internal URI rejects the released document token and delivers only to the current context: PASS\n");

  load(view, [&] { ephy_engine_web_view_load_uri(view, http.c_str()); });
  load(view, [&] { ephy_engine_web_view_go_back(view); });
  evaluate(view, "Gnosis.postMessage('probe','history');true");
  wait_for([&] { return normal.received.back() == "\"history\""; });
  auto normal_count = normal.received.size();
  auto* private_session = ephy_engine_session_new(true);
  auto* private_view = page(GTK_APPLICATION(app), private_session);
  Messages private_messages; private_messages.connect(private_view);
  load(private_view, [&] { ephy_engine_web_view_load_uri(private_view, "ephy-about:bridge-test"); });
  wait_for([&] { return private_messages.received.size() == 1; });
  g_assert_cmpuint(normal.received.size(), ==, normal_count);
  g_assert_true(private_messages.received.back() == "\"scheme\"");

  evaluate(private_view, "globalThis.nativeEvents=[];Gnosis.addEventListener('probe-reply',e=>nativeEvents.push(e.detail));true");
  auto private_cef = ephy_engine_view_browser(private_view);
  event(private_cef->GetMainFrame(), native->page_message_context.substr(0, 36), "ephy-about:bridge-test");
  g_assert_true(send(private_view, "ephy-about:bridge-test", "\"private-document\""));
  wait_for([&] { return evaluate(private_view, "JSON.stringify(nativeEvents)") == "[\"private-document\"]"; });

  g_print("Actual internal scheme scripts and history restoration work; private/tab message deliveries are isolated: PASS\n");

  // Subframes get no API, including data URLs with the reserved marker.
  auto frames = evaluate(view, R"JS(new Promise(resolve=>{
    const f=document.createElement('iframe');
    addEventListener('message',e=>resolve(e.data),{once:true});
    f.src='data:text/html;charset=utf-8;base64,'+btoa('<script>parent.postMessage(typeof Gnosis,"*")<\/script>')+'#gnosis-alternate-child';
    document.body.append(f);
  }))JS");
  g_assert_true(frames == "undefined");
  auto active = Browser::active_count;
  auto* closing_view = page(GTK_APPLICATION(app), ephy_engine_session_get_default());
  bool closed_by_message = false;
  ephy_engine_web_view_register_page_message_handler(closing_view, "close");
  g_signal_connect(closing_view, "page-message::close", G_CALLBACK(+[](EphyEngineView* view, JsonNode*, bool* closed) {
    *closed = true;
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(view))));
  }), &closed_by_message);
  ephy_engine_web_view_load_alternate_html(closing_view, "<script>Gnosis.postMessage('close',null)</script>", "about:blank", nullptr);
  wait_for([&] { return closed_by_message; });
  wait_for([&] { return Browser::active_count == active; });
  g_print("A native message callback may close its own window without retaining the view or invalidating the in-flight callback: PASS\n");
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(private_view))));
  while (auto* windows = gtk_application_get_windows(GTK_APPLICATION(app))) gtk_window_destroy(GTK_WINDOW(windows->data));
  wait_for([] { return Browser::active_count == 0; });
  g_assert_false(PageMessages::send(*native, "ephy-about:bridge-test", "probe-reply", "null"));
  g_print("Subframes cannot acquire the internal API, and normal window closure releases its native handlers: PASS\n");
  private_cef = nullptr; cef = nullptr; native = nullptr;
  g_object_unref(private_session);
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
