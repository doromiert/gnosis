#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
static GtkWidget *host;
static GtkWindow *window;
static GtkWidget *tabs, *source_embed;
static std::shared_ptr<GnosisBrave::Page> page;
static gint64 started, changed;
static int stage;
static bool busy;
static GdkCursor *before_scroll;
static int opened, initial_tabs;
static int tab_count() {
 return reinterpret_cast<int(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_get_n_pages"))(tabs);
}
static void fail(const char *message) { g_printerr("CURSOR_FAIL stage=%d %s\n",stage,message);_exit(77); }
static GtkWidget *find(GtkWidget *widget) {
 if (!strcmp(G_OBJECT_TYPE_NAME(widget),"GnosisCompositorView")) return widget;
 for(auto *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child)) if(auto *found=find(child)) return found;
 return nullptr;
}
static void physical(const std::vector<std::string>& arguments) {
 std::vector<char*> argv{const_cast<char*>(g_getenv("GNOSIS_XDOTOOL"))};
 for(auto& arg:arguments)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);
 int status=0;
 if(!g_spawn_sync(nullptr,argv.data(),nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest input failed");
}
static void motion(double x,double y) {
 if(g_getenv("GNOSIS_XDOTOOL")) {
  graphene_point_t point{float(x),float(y)},mapped;
  if(!gtk_widget_compute_point(host,GTK_WIDGET(window),&point,&mapped))fail("coordinate transform failed");
  auto id=std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
  physical({"windowfocus",id,"mousemove","--window",id,std::to_string(int(mapped.x)),std::to_string(int(mapped.y))});return;
 }
 auto *controllers=gtk_widget_observe_controllers(host);
 for(guint i=0;i<g_list_model_get_n_items(controllers);i++) {
  auto *controller=G_OBJECT(g_list_model_get_item(controllers,i));
  if(GTK_IS_EVENT_CONTROLLER_MOTION(controller)) g_signal_emit_by_name(controller,"motion",x,y);
  g_object_unref(controller);
 }
 g_object_unref(controllers);
}
static void command(const char *method,const char *params) {
 if(g_getenv("GNOSIS_XDOTOOL")) {
  if(!strcmp(method,"Input.dispatchKeyEvent")){physical({"key","Escape"});return;}
  g_autoptr(JsonParser) parser=json_parser_new();json_parser_load_from_data(parser,params,-1,nullptr);
  auto* value=json_node_get_object(json_parser_get_root(parser));
  motion(json_object_get_double_member(value,"x"),json_object_get_double_member(value,"y"));
  const char* type=json_object_get_string_member(value,"type");
  if(!strcmp(type,"mousePressed")||!strcmp(type,"mouseReleased"))
   physical({!strcmp(type,"mousePressed")?"mousedown":"mouseup",!strcmp(json_object_get_string_member(value,"button"),"middle")?"2":"1"});
  return;
 }
 busy=true;page->command(method,params,[](JsonObject*,const std::string& error){busy=false;if(!error.empty())fail(error.c_str());});
}
static void evaluate(const std::string& expression, bool check=false) {
 busy=true;page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(expression)+",\"returnByValue\":true}",
 [check](JsonObject* result,const std::string& error){
  busy=false;if(!result||!error.empty()||json_object_has_member(result,"exceptionDetails"))fail("evaluation failed");
  if(check&&!json_object_get_boolean_member_with_default(json_object_get_object_member(result,"result"),"value",false))fail("middle-click assertion failed");
 });
}
static gboolean tick(gpointer) {
 auto now=g_get_monotonic_time();if(now-started>60*G_USEC_PER_SEC)fail("timeout");
 if(busy||page->session.empty()||page->loading||now-changed<500000)return G_SOURCE_CONTINUE;
 auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
 if(dialog&&ADW_IS_ALERT_DIALOG(dialog)&&strstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)),"Default Browser")) {
  adw_dialog_force_close(dialog);return G_SOURCE_CONTINUE;
 }
 if(stage==0) {
  if(page->native_tab_id()<0)return G_SOURCE_CONTINUE;
  if(g_getenv("GNOSIS_SEED_PROFILE")) {
   page.reset();g_print("NATIVE_CURSOR_SEED_READY\n");
   g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  evaluate("document.body.innerHTML='<input style=\"position:absolute;left:20px;top:20px;width:200px;height:40px\"><a href=\"/middle-opened\" style=\"position:absolute;left:20px;top:100px;width:200px;height:40px\"><span>Link</span></a>';document.body.style.height='10000px'");
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
 } else if(stage==8) evaluate("scrollY>0",true);
 else if(stage==9) evaluate("scrollTo(0,0)");
 else if(stage==10||stage==16) {
  command("Input.dispatchMouseEvent",R"({"type":"mousePressed","x":40,"y":120,"button":"middle","buttons":4,"clickCount":1})");
 } else if(stage==11||stage==17) {
  command("Input.dispatchMouseEvent",R"({"type":"mouseReleased","x":40,"y":120,"button":"middle","buttons":0,"clickCount":1})");
 } else if(stage==12||stage==18||stage==24) {
  int expected=stage==12?1:stage==18?2:3;
  if(opened<expected||tab_count()<initial_tabs+expected)return G_SOURCE_CONTINUE;
  if(opened!=expected||tab_count()!=initial_tabs+expected)fail("link opened duplicate GTK tabs");
  reinterpret_cast<void(*)(void*,int)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_select_nth_page"))(tabs,tab_count()-1);
 } else if(stage==13||stage==19||stage==25) {
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto getview=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  auto child=getpage(getview(embed(window)));
  if(!child||child==page||child->loading||!child->uri.ends_with("/middle-opened"))return G_SOURCE_CONTINUE;
  reinterpret_cast<bool(*)(void*,void*)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_select_page"))(tabs,source_embed);
 } else if(stage==14||stage==20||stage==26) evaluate("scrollY===0 && location.pathname!=='/middle-opened'",true);
 else if(stage==15) evaluate("document.querySelector('a').rel='noopener';document.querySelector('a').target='_blank'");
 else if(stage==21) evaluate("document.querySelector('a').rel='opener'");
 else if(stage==22) command("Input.dispatchMouseEvent",R"({"type":"mousePressed","x":40,"y":120,"button":"left","buttons":1,"clickCount":1})");
 else if(stage==23) command("Input.dispatchMouseEvent",R"({"type":"mouseReleased","x":40,"y":120,"button":"left","buttons":0,"clickCount":1})");
 else {
  page.reset();g_print("NATIVE_CURSOR_PASS textbox/link/autoscroll GTK cursors, actual scrolling, middle-click links, noopener and target=_blank GTK tabs\n");
  g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
 }
 ++stage;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
 window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto getview=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 if(!embed(window))return G_SOURCE_CONTINUE;source_embed=GTK_WIDGET(embed(window));auto *view=getview(source_embed);host=find(view);if(!host)return G_SOURCE_CONTINUE;
 tabs=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_tab_view"))(window);initial_tabs=tab_count();
 page=getpage(view);if(!page)return G_SOURCE_CONTINUE;
 auto open=page->open_uri;page->open_uri=[open](const std::string& uri){if(!uri.ends_with("/middle-opened"))fail("unexpected link URL");++opened;return open&&open(uri);};
 started=changed=g_get_monotonic_time();g_timeout_add(150,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
