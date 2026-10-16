#include "diagnostic-process.hpp"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <cstring>
struct Test { GtkWindow* window; EphyEngineView* view; GMainLoop* loop; bool passed=false; bool failed=false; bool favicon=false; bool private_started=false; int stage=0; };
static void maybe_finish(Test* t) {
  if (!t->passed || (g_getenv("JUPITER_TEST_URL") && !t->favicon)) return;
  if (g_getenv("JUPITER_TEST_URL") && !t->private_started) {
    t->private_started=true;
    auto* session=ephy_engine_session_new(true);
    t->view=EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW,"network-session",session,nullptr));
    g_object_unref(session);
    g_signal_connect(t->view,"notify::favicon",G_CALLBACK(+[](EphyEngineView* view,GParamSpec*,Test* t){
      if (!ephy_engine_web_view_get_favicon(view)) return;
      char* uri=ephy_engine_favicon_database_get_favicon_uri(ephy_engine_favicon_database_get_default(),ephy_engine_web_view_get_uri(view));
      g_assert_null(uri);
      g_print("CEF private favicon excluded from shared cache: PASS\n");
      maybe_finish(t);
    }),t);
    std::string url=std::string(g_getenv("JUPITER_TEST_URL"))+"?private=1";
    ephy_engine_web_view_load_uri(t->view,url.c_str());
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(t->window),GTK_WIDGET(t->view));
    return;
  }
  gtk_window_destroy(t->window);
  g_timeout_add(50,+[](gpointer p)->gboolean{auto*t=static_cast<Test*>(p);if(Browser::active_count)return G_SOURCE_CONTINUE;g_main_loop_quit(t->loop);return G_SOURCE_REMOVE;},t);
}
int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub=ephy_engine_execute_process(argc,argv);if(sub>=0)return sub;
  auto* app=adw_application_new("io.github.doromiert.Jupiter.EngineTest",G_APPLICATION_NON_UNIQUE);
  GError* error=nullptr;if(!g_application_register(G_APPLICATION(app),nullptr,&error)){g_printerr("%s\n",error->message);return 1;}
  char profile[]="/tmp/jupiter-native-engine-XXXXXX";if(!mkdtemp(profile))return 1;
  if(!ephy_engine_initialize(argc,argv,profile,true))return 1;
  disable_diagnostic_core_dumps();
  Test test;
  test.loop=g_main_loop_new(nullptr,false);
  test.window=GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_default_size(test.window,900,650);
  test.view=EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW,nullptr));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(test.window),GTK_WIDGET(test.view));
  g_signal_connect(test.view,"notify::favicon",G_CALLBACK(+[](EphyEngineView* view,GParamSpec*,Test* t){
    if (!ephy_engine_web_view_get_favicon(view)) return;
    char* uri=ephy_engine_favicon_database_get_favicon_uri(ephy_engine_favicon_database_get_default(),ephy_engine_web_view_get_uri(view));
    g_assert_nonnull(uri);
    g_assert_true(g_str_has_suffix(uri,"/favicon.png"));
    g_free(uri);
    g_print("CEF favicon download and native database: PASS\n");
    t->favicon=true;
    maybe_finish(t);
  }),&test);
  g_signal_connect(test.view,"load-changed",G_CALLBACK(+[](EphyEngineView* view,WebKitLoadEvent event,Test* t){
    if(event!=WEBKIT_LOAD_FINISHED)return;
    if(t->stage==0){
      t->stage=1;
      ephy_engine_web_view_evaluate_javascript(view,"({title:document.title, text:document.querySelector('h1').textContent, webgl:!!document.createElement('canvas').getContext('webgl')})",-1,nullptr,nullptr,nullptr,
      +[](GObject* obj,GAsyncResult* result,gpointer user){
        auto* t=static_cast<Test*>(user);GError* error=nullptr;
        auto* value=ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(obj),result,&error);
        bool ok=value && json_object_has_member(value,"value") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(value,"value"));
        if(ok){auto* object=json_object_get_object_member(value,"value");ok=!g_strcmp0(json_object_get_string_member(object,"title"),"CEF native contract");}
        if(!ok){g_printerr("CEF DOM: FAIL %s\n",error?error->message:"");t->failed=true;gtk_window_destroy(t->window);g_main_loop_quit(t->loop);}
        else {g_print("CEF DOM: PASS\n");t->passed=true;maybe_finish(t);}
        g_clear_error(&error);g_clear_pointer(&value,json_object_unref);
      },t);
    }
  }),&test);
  const char* fixture=g_getenv("JUPITER_TEST_URL");
  ephy_engine_web_view_load_uri(test.view,fixture?fixture:"data:text/html,<title>CEF native contract</title><h1>Chromium in GTK</h1>");
  gtk_window_present(test.window);
  guint timeout=g_timeout_add_seconds(20,+[](gpointer p)->gboolean{auto*t=static_cast<Test*>(p);t->failed=true;g_printerr("CEF view test: TIMEOUT\n");g_main_loop_quit(t->loop);return G_SOURCE_REMOVE;},&test);
  g_main_loop_run(test.loop);
  if(!test.failed)g_source_remove(timeout);
  if(Browser::active_count)return 1;
  ephy_engine_shutdown();g_main_loop_unref(test.loop);g_object_unref(app);
  return test.passed&&!test.failed?0:1;
}
