#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWidget *host;
static std::shared_ptr<GnosisBrave::Page> page;
static gint64 started, changed;
static int stage;
static bool busy;
static GdkCursor *before_scroll;
static void fail(const char *message) { g_printerr("CURSOR_FAIL stage=%d %s\n",stage,message);_exit(77); }
static GtkWidget *find(GtkWidget *widget) {
 if (!strcmp(G_OBJECT_TYPE_NAME(widget),"GnosisCompositorView")) return widget;
 for(auto *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child)) if(auto *found=find(child)) return found;
 return nullptr;
}
static void motion(double x,double y) {
 auto *controllers=gtk_widget_observe_controllers(host);
 for(guint i=0;i<g_list_model_get_n_items(controllers);i++) {
  auto *controller=G_OBJECT(g_list_model_get_item(controllers,i));
  if(GTK_IS_EVENT_CONTROLLER_MOTION(controller)) g_signal_emit_by_name(controller,"motion",x,y);
  g_object_unref(controller);
 }
 g_object_unref(controllers);
}
static void command(const char *method,const char *params) {
 busy=true;page->command(method,params,[](JsonObject*,const std::string& error){busy=false;if(!error.empty())fail(error.c_str());});
}
static gboolean tick(gpointer) {
 auto now=g_get_monotonic_time();if(now-started>30*G_USEC_PER_SEC)fail("timeout");
 if(busy||page->session.empty()||page->loading||now-changed<300000)return G_SOURCE_CONTINUE;
 if(stage==0) {
  command("Runtime.evaluate",R"({"expression":"document.body.innerHTML='<input style=\"position:absolute;left:20px;top:20px;width:200px;height:40px\"><a href=\"#test\" style=\"position:absolute;left:20px;top:100px;width:200px;height:40px\">Link</a>';document.body.style.height='10000px'"})");
 } else if(stage==1)motion(40,40);
 else if(stage==2) {
  auto *cursor=gtk_widget_get_cursor(host);const char *name=cursor?gdk_cursor_get_name(cursor):nullptr;
  g_print("CURSOR_INPUT name=%s\n",name?name:"custom");if(!name||strcmp(name,"text"))fail("textbox cursor was not forwarded");
  motion(40,120);
 } else if(stage==3) {
  auto *cursor=gtk_widget_get_cursor(host);const char *name=cursor?gdk_cursor_get_name(cursor):nullptr;
  g_print("CURSOR_LINK name=%s\n",name?name:"custom");if(!name||strcmp(name,"pointer"))fail("link cursor was not forwarded");motion(400,200);
 } else if(stage==4) {
  auto *cursor=gtk_widget_get_cursor(host);before_scroll=cursor?GDK_CURSOR(g_object_ref(cursor)):nullptr;
  command("Input.dispatchMouseEvent",R"({"type":"mousePressed","x":400,"y":200,"button":"middle","buttons":4,"clickCount":1})");
 } else if(stage==5)command("Input.dispatchMouseEvent",R"({"type":"mouseReleased","x":400,"y":200,"button":"middle","buttons":0,"clickCount":1})");
 else if(stage==6)command("Input.dispatchMouseEvent",R"({"type":"mouseMoved","x":400,"y":300,"buttons":0})");
 else if(stage==7) {
  auto *cursor=gtk_widget_get_cursor(host);const char *name=cursor?gdk_cursor_get_name(cursor):nullptr;
  g_print("CURSOR_AUTOSCROLL name=%s changed=%d\n",name?name:"custom",cursor!=before_scroll);
  if(!cursor||cursor==before_scroll||(name&&(!strcmp(name,"default")||!strcmp(name,"pointer"))))fail("autoscroll cursor was not forwarded");
  g_clear_object(&before_scroll);
  command("Input.dispatchKeyEvent",R"({"type":"keyDown","key":"Escape","code":"Escape","windowsVirtualKeyCode":27})");
 } else {
  page.reset();g_print("NATIVE_CURSOR_PASS textbox, link, autoscroll GTK cursors\n");g_application_quit(g_application_get_default());return G_SOURCE_REMOVE;
 }
 ++stage;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
 auto *window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto getview=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 if(!embed(window))return G_SOURCE_CONTINUE;auto *view=getview(embed(window));host=find(view);if(!host)return G_SOURCE_CONTINUE;
 page=getpage(view);if(!page)return G_SOURCE_CONTINUE;started=changed=g_get_monotonic_time();g_timeout_add(150,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
