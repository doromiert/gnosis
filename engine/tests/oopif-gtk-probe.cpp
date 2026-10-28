/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
extern "C" GSubprocess* g_subprocess_launcher_spawnv(GSubprocessLauncher*l,const gchar*const*argv,GError**error) {
  auto real=reinterpret_cast<GSubprocess*(*)(GSubprocessLauncher*,const gchar*const*,GError**)>(dlsym(RTLD_NEXT,"g_subprocess_launcher_spawnv"));
  if(!argv||!*argv||!strstr(*argv,"brave"))return real(l,argv,error);
  std::vector<const char*> args;for(auto*p=argv;*p;++p)args.push_back(*p);args.push_back("--site-per-process");args.push_back(nullptr);
  return real(l,args.data(),error);
}
static GtkWindow* window;
static std::shared_ptr<GnosisBrave::Page> page;
static std::string child, grandchild;
static GtkWidget* old_menu;
static std::string old_action;
static int stage;
static bool busy;
static gint64 started, changed;
static void fail(const char* message) {g_printerr("OOPIF_GTK_FAIL stage=%d %s\n",stage,message);_exit(77);}
static bool has_label(GtkWidget* w,const char* text) {
  if(GTK_IS_LABEL(w)&&!strcmp(gtk_label_get_text(GTK_LABEL(w)),text))return true;
  for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(has_label(c,text))return true;
  return false;
}
static GtkWidget* find(GtkWidget* w,const char* type,const char* label=nullptr) {
  if(!strcmp(G_OBJECT_TYPE_NAME(w),type)&&(!label||has_label(w,label)))return w;
  for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find(c,type,label))return r;
  return nullptr;
}
static JsonObject* value(JsonObject* result) {auto*r=result?json_object_get_object_member(result,"result"):nullptr;return r&&json_object_has_member(r,"value")?json_object_get_object_member(r,"value"):nullptr;}
static void eval(const std::string& frame,const char* script,std::function<void(JsonObject*)> next) {
  busy=true;
  auto callback=[next](JsonObject*r,const std::string&e){busy=false;if(!e.empty()||!r||json_object_has_member(r,"exceptionDetails"))return;next(value(r));};
  auto params="{\"expression\":"+GnosisBrave::quote(script)+",\"returnByValue\":true}";
  if(frame.empty())page->command("Runtime.evaluate",params,callback);
  else page->command_for_frame(frame,"Runtime.evaluate",params,callback);
}
static void click(double x,double y,bool right) {
  const auto button=right?"right":"left";const auto buttons=right?2:1;
  for(auto type:{"mousePressed","mouseReleased"})page->command("Input.dispatchMouseEvent","{\"type\":"+GnosisBrave::quote(type)+",\"button\":"+GnosisBrave::quote(button)+",\"buttons\":"+std::to_string(!strcmp(type,"mousePressed")?buttons:0)+",\"x\":"+std::to_string(x)+",\"y\":"+std::to_string(y)+",\"clickCount\":1}");
}
static void hit(bool grand,const char* element,bool right,std::function<void()> next) {
  const auto script="(()=>{const r=document.getElementById("+GnosisBrave::quote(element)+").getBoundingClientRect();return{x:r.x+8,y:r.y+8}})()";
  eval(grand?grandchild:child,script.c_str(),[grand,right,next](JsonObject*o){if(!o)return;const double x=json_object_get_double_member(o,"x"),y=json_object_get_double_member(o,"y");if(grand){eval(child,"(()=>{const r=nested.getBoundingClientRect();return{x:r.x,y:r.y}})()",[x,y,right,next](JsonObject*r){if(!r)return;click(x+json_object_get_double_member(r,"x"),y+json_object_get_double_member(r,"y"),right);next();});}else{click(x,y,right);next();}});
}
static std::string action_for(GMenuModel* model) {
  for(int i=0;i<g_menu_model_get_n_items(model);++i){g_autofree char*label=nullptr;g_autofree char*action=nullptr;g_menu_model_get_item_attribute(model,i,G_MENU_ATTRIBUTE_LABEL,"s",&label);g_menu_model_get_item_attribute(model,i,G_MENU_ATTRIBUTE_ACTION,"s",&action);if(label&&strstr(label,"Copy Image")&&!strstr(label,"Address")&&action)return action;for(auto name:{G_MENU_LINK_SECTION,G_MENU_LINK_SUBMENU}){auto*sub=g_menu_model_get_item_link(model,i,name);if(sub){auto result=action_for(sub);g_object_unref(sub);if(!result.empty())return result;}}}
  return {};
}
static void read_image(int expected,int next) {
  busy=true;auto*data=new std::pair<int,int>{expected,next};
  gdk_clipboard_read_texture_async(gtk_widget_get_clipboard(GTK_WIDGET(window)),nullptr,+[](GObject*source,GAsyncResult*result,gpointer pointer){std::unique_ptr<std::pair<int,int>>data(static_cast<std::pair<int,int>*>(pointer));g_autoptr(GError)error=nullptr;auto*texture=gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source),result,&error);if(!texture||error||gdk_texture_get_width(texture)!=2||gdk_texture_get_height(texture)!=2)fail("image copy did not produce original pixels");unsigned char pixels[16];gdk_texture_download(texture,pixels,8);g_object_unref(texture);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  const int red=pixels[2],blue=pixels[0];
#else
  const int red=pixels[1],blue=pixels[3];
#endif
  if(data->first==0?(red!=255||blue!=0):(red!=0||blue!=255))fail("copied pixels came from the wrong frame");busy=false;stage=data->second;changed=g_get_monotonic_time();g_printerr("OOPIF_IMAGE_PASS %s\n",data->first==0?"child":"nested grandchild");},data);
}
static gboolean tick(gpointer) {
  if(g_get_monotonic_time()-started>110*G_USEC_PER_SEC)fail("timeout");
  static int previous=-1;if(previous!=stage){g_printerr("OOPIF_STAGE %d\n",stage);previous=stage;}
  if(busy)return G_SOURCE_CONTINUE;
  auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if(d&&ADW_IS_ALERT_DIALOG(d)&&strstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(d)),"Default Browser")){auto*b=find(GTK_WIDGET(d),"GtkButton","No");if(b)gtk_widget_activate(b);return G_SOURCE_CONTINUE;}
  if(stage==0&&!page->session.empty()) {
    busy=true;GnosisBrave::browser_command(false,"Target.getTargets","{}",[](JsonObject*r,const std::string&e){
      busy=false;auto*targets=r?json_object_get_array_member(r,"targetInfos"):nullptr;
      static gint64 last=0;if(g_get_monotonic_time()-last>5*G_USEC_PER_SEC){guint frames=0;for(guint i=0;targets&&i<json_array_get_length(targets);++i)frames+=!strcmp(json_object_get_string_member_with_default(json_array_get_object_element(targets,i),"type",""),"iframe");g_printerr("OOPIF_TARGETS total=%u iframe=%u parent_fixture=%d error=%s\n",targets?json_array_get_length(targets):0,frames,page->uri.find("oopif-parent.html")!=std::string::npos,e.c_str());last=g_get_monotonic_time();}
      for(guint i=0;targets&&i<json_array_get_length(targets);++i){auto*t=json_array_get_object_element(targets,i);if(strcmp(json_object_get_string_member_with_default(t,"type",""),"iframe"))continue;std::string url=json_object_get_string_member_with_default(t,"url","");if(url.find("oopif-child.html")!=std::string::npos)child=json_object_get_string_member(t,"targetId");if(url.find("oopif-grandchild.html")!=std::string::npos)grandchild=json_object_get_string_member(t,"targetId");}
      if(!child.empty()&&!grandchild.empty())stage=1;
    });
  } else if(stage==1)eval(child,"({ready:frameMarker==='child'&&document.readyState==='complete',hidden:typeof __gnosisContextMenu==='undefined'&&typeof __gnosisDocumentMetadata==='undefined'})",[](JsonObject*o){if(!o||!json_object_get_boolean_member_with_default(o,"ready",false))return;if(!json_object_get_boolean_member(o,"hidden"))fail("private binding exposed to page scripts");GnosisBrave::set_user_content("body{color:rgb(1,2,3)!important}","");stage=23;});
  else if(stage==23)eval(child,"({styled:getComputedStyle(document.body).color==='rgb(1, 2, 3)'})",[](JsonObject*o){if(o&&json_object_get_boolean_member_with_default(o,"styled",false))stage=24;});
  else if(stage==24)eval(grandchild,"({styled:getComputedStyle(document.body).color==='rgb(1, 2, 3)'})",[](JsonObject*o){if(o&&json_object_get_boolean_member_with_default(o,"styled",false)){g_printerr("OOPIF_STYLE_PASS custom CSS reaches child and nested renderer sessions\n");GnosisBrave::set_user_content("","");stage=2;}});
  else if(stage==2)hit(false,"image",true,[]{stage=3;});
  else if(stage==3){auto*b=find(GTK_WIDGET(window),"GtkModelButton","Copy Image");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){gtk_widget_activate(b);read_image(0,4);}}
  else if(stage==4)hit(false,"audio",true,[]{stage=5;});
  else if(stage==5){auto*b=find(GTK_WIDGET(window),"GtkModelButton","Mute");if(b&&gtk_widget_get_mapped(b)){gtk_widget_activate(b);stage=6;}}
  else if(stage==6)eval(child,"({muted:audio.muted})",[](JsonObject*o){if(o&&json_object_get_boolean_member_with_default(o,"muted",false)){g_printerr("OOPIF_MEDIA_PASS native action routed to child session\n");stage=7;}});
  else if(stage==7)hit(true,"image",true,[]{stage=8;});
  else if(stage==8){auto*b=find(GTK_WIDGET(window),"GtkModelButton","Copy Image");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){gtk_widget_activate(b);read_image(1,9);}}
  else if(stage==9){busy=true;page->request_close([](bool approved,bool,const std::string&e){busy=false;if(!approved||!e.empty()||!page->close_is_approved())fail("native preflight approval");stage=10;});}
  else if(stage==10)hit(false,"activity",false,[]{stage=11;changed=g_get_monotonic_time();});
  else if(stage==11&&g_get_monotonic_time()-changed>300000){if(page->close_is_approved())fail("child trusted interaction retained stale close approval");g_printerr("OOPIF_ACTIVITY_PASS child input invalidates parent close approval\n");stage=12;}
  else if(stage==12){busy=true;page->request_close([](bool approved,bool,const std::string&e){busy=false;if(!approved||!e.empty())fail("repeat preflight");stage=13;});}
  else if(stage==13)hit(true,"activity",false,[]{stage=14;changed=g_get_monotonic_time();});
  else if(stage==14&&g_get_monotonic_time()-changed>300000){if(page->close_is_approved())fail("grandchild retained stale close approval");g_printerr("OOPIF_ACTIVITY_PASS nested child input invalidates parent close approval\n");stage=15;}
  else if(stage==15)hit(false,"image",true,[]{stage=16;});
  else if(stage==16){auto*b=find(GTK_WIDGET(window),"GtkModelButton","Copy Image");auto*menu=b?gtk_widget_get_ancestor(b,GTK_TYPE_POPOVER_MENU):nullptr;if(menu&&b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){old_menu=GTK_WIDGET(g_object_ref(menu));old_action=action_for(gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu)));if(old_action.empty())fail("old menu action missing");gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(window)),"stale-action-sentinel");page->command_for_frame(child,"Page.navigate","{\"url\":\"http://localhost:18473/oopif-child.html?again\"}");stage=17;changed=g_get_monotonic_time();}}
  else if(stage==17&&g_get_monotonic_time()-changed>500000)eval(child,"({ready:location.search==='?again'&&document.readyState==='complete'})",[](JsonObject*o){if(!o||!json_object_get_boolean_member_with_default(o,"ready",false))return;gtk_widget_activate_action(old_menu,old_action.c_str(),nullptr);g_object_unref(old_menu);old_menu=nullptr;busy=true;gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(window)),nullptr,+[](GObject*source,GAsyncResult*r,gpointer){g_autoptr(GError)e=nullptr;g_autofree char*text=gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source),r,&e);busy=false;if(e||g_strcmp0(text,"stale-action-sentinel"))fail("stale frame menu action still operates");g_printerr("OOPIF_STALE_MENU_PASS navigation retires old frame actions\n");stage=18;},nullptr);});
  else if(stage==18)eval("","({title:document.title,parentMuted:parentAudio.muted,uri:location.pathname})",[](JsonObject*o){if(!o||strcmp(json_object_get_string_member(o,"title"),"Gnosis OOPIF fixture")||strcmp(json_object_get_string_member(o,"uri"),"/oopif-parent.html")||json_object_get_boolean_member(o,"parentMuted"))fail("iframe changed top-level document or parent media");busy=true;page->request_close([](bool approved,bool,const std::string&e){busy=false;if(!approved||!e.empty())fail("navigation preflight");stage=21;});});
  else if(stage==21){page->command_for_frame(child,"Page.navigate","{\"url\":\"http://localhost:18473/oopif-child.html?final\"}");stage=22;changed=g_get_monotonic_time();}
  else if(stage==22&&g_get_monotonic_time()-changed>500000)eval(child,"({ready:location.search==='?final'&&document.readyState==='complete'})",[](JsonObject*o){if(!o||!json_object_get_boolean_member_with_default(o,"ready",false))return;if(page->close_is_approved())fail("child navigation retained old approval");g_printerr("OOPIF_NAVIGATION_PASS child navigation invalidates close approval\n");eval("","({removed:(frame.remove(),true)})",[](JsonObject*){stage=19;changed=g_get_monotonic_time();});});
  else if(stage==19&&g_get_monotonic_time()-changed>500000){busy=true;page->command_for_frame(child,"Runtime.evaluate","{\"expression\":\"true\"}",[](JsonObject*,const std::string&e){busy=false;if(e.empty())fail("detached frame remains routable");g_printerr("OOPIF_GTK_PASS child/nested menus, original pixels, media session, trusted input, navigation, detach and top-level isolation\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);stage=20;});}
  return stage==20?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto p=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));if(!e(window))return G_SOURCE_CONTINUE;page=p(v(e(window)));started=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(5,start,nullptr);}
