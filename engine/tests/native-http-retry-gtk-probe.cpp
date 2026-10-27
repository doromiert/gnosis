#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWindow* window;static std::shared_ptr<GnosisBrave::Page> page;static gint64 started;static int stage;static bool busy;
static void fail(const char* message){g_printerr("HTTP_RETRY_FAIL stage=%d %s\n",stage,message);_exit(77);}
static gboolean tick(gpointer){
 if(g_get_monotonic_time()-started>30*G_USEC_PER_SEC)fail("timeout");if(busy||page->session.empty())return G_SOURCE_CONTINUE;
 if(stage==0){if(page->title!="Native web app test")return G_SOURCE_CONTINUE;auto normalize=reinterpret_cast<char*(*)(const char*)>(dlsym(RTLD_DEFAULT,"ephy_embed_utils_normalize_address"));auto* address=normalize("localhost:5173");if(strcmp(address,"http://localhost:5173"))fail("normalizer still forces local HTTPS");g_free(address);page->navigate(g_getenv("GNOSIS_HTTP_TEST_URL"));stage=1;}
 else if(stage==1||stage==2){auto* dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(!ADW_IS_ALERT_DIALOG(dialog)||g_strcmp0(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)),"Use HTTP for Local Site?"))return G_SOURCE_CONTINUE;
  if(stage==1){g_signal_emit_by_name(dialog,"response","cancel");adw_dialog_force_close(dialog);page->navigate(g_getenv("GNOSIS_HTTP_TEST_URL"));stage=2;}
  else{g_print("HTTP_RETRY_NATIVE_PROMPT_PASS cancellation and explicit retry\n");g_signal_emit_by_name(dialog,"response","http");adw_dialog_force_close(dialog);stage=3;}}
 else if(stage==3){if(page->loading)return G_SOURCE_CONTINUE;busy=true;page->command("Runtime.evaluate",R"({"expression":"location.protocol==='http:'&&location.pathname==='/retry-path'&&location.search==='?q=one'","returnByValue":true})",[](JsonObject* result,const std::string& error){busy=false;auto* remote=result?json_object_get_object_member(result,"result"):nullptr;if(!error.empty())fail(error.c_str());if(!remote||!json_object_get_boolean_member_with_default(remote,"value",false))return;g_print("HTTP_RETRY_PASS localhost defaults to HTTP; TLS failure offers native opt-in HTTP preserving path and query\n");page.reset();stage=4;g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);});}
 return stage==4?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
}
static gboolean start(gpointer){auto* app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto view=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));if(!embed(window))return G_SOURCE_CONTINUE;page=getpage(view(embed(window)));if(!page)return G_SOURCE_CONTINUE;started=g_get_monotonic_time();g_timeout_add(150,tick,nullptr);return G_SOURCE_REMOVE;}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
