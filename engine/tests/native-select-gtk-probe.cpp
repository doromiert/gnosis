/* Test only: exercise real HTML selects in the packaged GTK/Wayland backend. */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static std::shared_ptr<GnosisBrave::Page> page;
static GtkWidget *host;
static gint64 started, changed;
static int stage;
static bool busy;
static double popup_x, popup_y;
static void fail(const char *s) { g_printerr("SELECT_FAIL stage=%d %s\n",stage,s); _exit(77); }
static GtkWidget *find(GtkWidget *w) {
 if (!strcmp(G_OBJECT_TYPE_NAME(w),"GnosisCompositorView")) return w;
 for(auto *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c)) if(auto *r=find(c)) return r;
 return nullptr;
}
static void capture(const char *path, bool check_native = false) {
 auto *p=gtk_widget_paintable_new(host); auto *s=gtk_snapshot_new();
 gdk_paintable_snapshot(p,GDK_SNAPSHOT(s),gtk_widget_get_width(host),gtk_widget_get_height(host));
 if(auto *n=gtk_snapshot_free_to_node(s)) {
  auto *t=gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(host)),n,nullptr);
  if(check_native) {
   const int width=gdk_texture_get_width(t),height=gdk_texture_get_height(t);
   auto *pixels=g_new(guchar,width*height*4);gdk_texture_download(t,pixels,width*4);
   const int x=popup_x,y=popup_y;
   bool visible=x>=0&&y>=0&&x<width&&y<height&&pixels[(y*width+x)*4]<200;
   g_free(pixels);if(!visible)fail("native popup did not paint below its button");
  }
  gdk_texture_save_to_png(t,path); g_object_unref(t); gsk_render_node_unref(n);
 }
 g_object_unref(p);
}
static void command(const char *method,const std::string& params) {
 busy=true; page->command(method,params,[](JsonObject*,const std::string& e){busy=false;if(!e.empty())fail(e.c_str());});
}
static void key(guint value) {
 const guint code=value==GDK_KEY_Down?116:value==GDK_KEY_Return?36:9;
 auto *controllers=gtk_widget_observe_controllers(host);
 for(guint i=0;i<g_list_model_get_n_items(controllers);i++) {
  auto *c=G_OBJECT(g_list_model_get_item(controllers,i));
  if(GTK_IS_EVENT_CONTROLLER_KEY(c)) { gboolean handled=false; g_signal_emit_by_name(c,"key-pressed",value,code,0,&handled);g_signal_emit_by_name(c,"key-released",value,code,0); }
  g_object_unref(c);
 }
 g_object_unref(controllers);
}
static void click(const char *id) {
 busy=true;
 page->command("Runtime.evaluate",std::string("{\"expression\":\"(()=>{let r=document.getElementById('")+id+"').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,px:r.x+4,py:r.bottom+30}})()\",\"returnByValue\":true}",[](JsonObject*r,const std::string&e){
  if(!r||!e.empty())fail("coordinates");
  auto *o=json_object_get_object_member(json_object_get_object_member(r,"result"),"value");
  auto x=json_object_get_double_member(o,"x"),y=json_object_get_double_member(o,"y");
  popup_x=json_object_get_double_member(o,"px");popup_y=json_object_get_double_member(o,"py");
  std::string pos="\"x\":"+std::to_string(x)+",\"y\":"+std::to_string(y)+",\"button\":\"left\",\"clickCount\":1";
  page->command("Input.dispatchMouseEvent","{\"type\":\"mousePressed\","+pos+"}",[pos](JsonObject*,const std::string&e){if(!e.empty())fail(e.c_str());command("Input.dispatchMouseEvent","{\"type\":\"mouseReleased\","+pos+"}");});
 });
}
static gboolean tick(gpointer) {
 auto now=g_get_monotonic_time();if(now-started>50*G_USEC_PER_SEC)fail("timeout");
 if(busy||page->session.empty()||page->loading||now-changed<800000)return G_SOURCE_CONTINUE;
 if(stage==0) click("native");
 else if(stage==1) {capture("/tmp/gnosis-select-native.png",true);key(GDK_KEY_Down);}
 else if(stage==2) key(GDK_KEY_Return);
 else if(stage==3) {
  busy=true;page->command("Runtime.evaluate",R"JSON({"expression":"JSON.stringify({value:document.getElementById('native').value,changes})","returnByValue":true})JSON",[](JsonObject*r,const std::string&e){
   busy=false;auto *v=r?json_object_get_object_member(r,"result"):nullptr;const char *s=v?json_object_get_string_member_with_default(v,"value",""):"";
   g_print("SELECT_NATIVE %s\n",s);if(!e.empty()||!strstr(s,"Second")||!strstr(s,"\"trusted\":true"))fail("native selection/change");
  });
 } else if(stage==4) click("default");
 else if(stage==5) {capture("/tmp/gnosis-select-themed.png");key(GDK_KEY_Down);}
 else if(stage==6) key(GDK_KEY_Return);
 else if(stage==7) {
  busy=true;page->command("Runtime.evaluate",R"JSON({"expression":"JSON.stringify({value:document.getElementById('default').value,changes,authored:getComputedStyle(document.getElementById('authored')).backgroundColor,list:getComputedStyle(document.getElementById('list')).appearance,layered:getComputedStyle(document.getElementById('layered')).backgroundColor,appearance:getComputedStyle(document.getElementById('default')).appearance,nested:getComputedStyle(document.querySelector('iframe').contentDocument.querySelector('select')).appearance})","returnByValue":true})JSON",[](JsonObject*r,const std::string&e){busy=false;auto *v=r?json_object_get_object_member(r,"result"):nullptr;const char *s=v?json_object_get_string_member_with_default(v,"value",""):"";g_print("SELECT_THEME %s\n",s);if(!e.empty()||!strstr(s,"Vim-like")||!strstr(s,"210, 220, 230")||!strstr(s,"180, 200, 220")||!strstr(s,"\"appearance\":\"base-select\"")||!strstr(s,"\"nested\":\"base-select\"")||!strstr(s,"\"list\":\"auto\""))fail("themed select or authored preservation");});
 } else if(stage==8) click("edge");
 else if(stage==9) {capture("/tmp/gnosis-select-edge.png");key(GDK_KEY_Escape);}
 else if(stage==10) {
  busy=true;page->command("Runtime.evaluate",R"JSON({"expression":"!document.querySelector('select:open')&&changes.length===2","returnByValue":true})JSON",[](JsonObject*r,const std::string&e){busy=false;auto *v=r?json_object_get_object_member(r,"result"):nullptr;if(!e.empty()||!v||!json_object_get_boolean_member_with_default(v,"value",false))fail("Escape did not dismiss without changing value");});
 }
 else {page.reset();g_print("NATIVE_SELECT_PASS\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
 stage++;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
 auto *w=gtk_application_get_active_window(GTK_APPLICATION(app));if(!w)return G_SOURCE_CONTINUE;
 auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w));
 if(dialog&&ADW_IS_ALERT_DIALOG(dialog)){g_signal_emit_by_name(dialog,"response","no");adw_dialog_close(dialog);return G_SOURCE_CONTINUE;}
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto view=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 if(!embed||!view||!getpage||!embed(w))return G_SOURCE_CONTINUE;
 auto *v=view(embed(w));host=find(v);page=getpage(v);if(!host||!page)return G_SOURCE_CONTINUE;
 if(page->uri.find("native-select.html")==std::string::npos)return G_SOURCE_CONTINUE;
 gtk_widget_grab_focus(host);started=changed=g_get_monotonic_time();g_timeout_add(150,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
