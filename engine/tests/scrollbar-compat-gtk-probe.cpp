/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
static GtkWindow* window;
static std::shared_ptr<JupiterBrave::Page> page;
static std::vector<std::pair<std::string,std::string>> steps;
static unsigned step;
static bool busy, ready, vim;
static gint64 started, next;
static void fail(const char* message) { g_printerr("SCROLLBAR_GTK_FAIL %u %s\n",step,message); _exit(77); }
static void add(const char* description,const char* expression) { steps.emplace_back(description,expression); }
static void capture(const char* path) {
  auto* paintable=gtk_widget_paintable_new(GTK_WIDGET(window));auto* snapshot=gtk_snapshot_new();
  gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(snapshot),gtk_widget_get_width(GTK_WIDGET(window)),gtk_widget_get_height(GTK_WIDGET(window)));
  auto* node=gtk_snapshot_free_to_node(snapshot);
  if(node){auto* renderer=gsk_renderer_new_for_surface(gtk_native_get_surface(GTK_NATIVE(window)));auto* texture=gsk_renderer_render_texture(renderer,node,nullptr);gdk_texture_save_to_png(texture,path);g_object_unref(texture);gsk_renderer_unrealize(renderer);g_object_unref(renderer);gsk_render_node_unref(node);}g_object_unref(paintable);
}
static gboolean tick(gpointer) {
  if(g_get_monotonic_time()-started>90*G_USEC_PER_SEC)fail("timeout");
  if(busy||g_get_monotonic_time()<next||page->session.empty())return G_SOURCE_CONTINUE;
  if(!ready){busy=true;page->command("Runtime.evaluate","{\"expression\":\"document.readyState==='complete'&&location.pathname==='/scrollbar-compat.html'\",\"returnByValue\":true}",[](JsonObject*r,const std::string&){busy=false;auto*v=r?json_object_get_object_member(r,"result"):nullptr;ready=v&&json_object_get_boolean_member_with_default(v,"value",false);});return G_SOURCE_CONTINUE;}
  if(step==steps.size()) {
    capture(vim?"/tmp/gnosis-scrollbar-vim.png":"/tmp/gnosis-scrollbar-compat.png");
    g_printerr("SCROLLBAR_GTK_PASS thin normalization, colors, hidden state, dynamic classes, inline styles, new nodes and hover; vim=%d\n",vim);
    page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  auto [description,expression]=steps[step++];g_printerr("SCROLLBAR_STEP %u %s\n",step,description.c_str());
  busy=true;page->command("Runtime.evaluate","{\"expression\":"+JupiterBrave::quote(expression)+",\"returnByValue\":true}",[description](JsonObject*r,const std::string&e){busy=false;auto*v=r?json_object_get_object_member(r,"result"):nullptr;if(!r||!e.empty()||json_object_has_member(r,"exceptionDetails")||!v)fail(description.c_str());
    if(description=="prepare hover") {
      auto*point=json_object_get_object_member(v,"value");if(!point)fail("pointer coordinates");
      busy=true;page->command("Input.dispatchMouseEvent","{\"type\":\"mouseMoved\",\"x\":"+std::to_string(json_object_get_double_member(point,"x"))+",\"y\":"+std::to_string(json_object_get_double_member(point,"y"))+"}",[](JsonObject*,const std::string&){busy=false;});
    } else if(!json_object_get_boolean_member_with_default(v,"value",false))fail(description.c_str());});
  next=g_get_monotonic_time()+450000;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto get=reinterpret_cast<std::shared_ptr<JupiterBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(!embed(window))return G_SOURCE_CONTINUE;page=get(view(embed(window)));
  auto prefs=reinterpret_cast<GSettings*(*)(const char*)>(dlsym(RTLD_DEFAULT,"ephy_settings_get"));
  g_settings_set_boolean(prefs("io.github.doromiert.Jupiter"),"ask-for-default",false);
  if(auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)))adw_dialog_force_close(d);
  vim=g_getenv("GNOSIS_SCROLLBAR_VIM")!=nullptr;
  if(vim)g_settings_set_boolean(prefs("io.github.doromiert.Jupiter.web"),"enable-vim-mode",true);
  add("normal and hidden widths", "getComputedStyle(auto).scrollbarWidth==='auto'&&getComputedStyle(hidden).scrollbarWidth==='none'");
  add("thin uses normal theme", "getComputedStyle(thin).scrollbarWidth==='auto'&&getComputedStyle(thin,'::-webkit-scrollbar').width==='24px'");
  add("author colors on thin", "thin.style.scrollbarColor='red yellow';true");
  add("thin color reset", "getComputedStyle(thin).scrollbarColor==='auto'");
  add("class toggles hidden", "thin.className='scroller hidden';true");
  add("hidden survives normalization", "getComputedStyle(thin).scrollbarWidth==='none'");
  add("class toggles back", "thin.className='scroller thin';true");
  add("thin restored", "getComputedStyle(thin).scrollbarWidth==='auto'");
  add("inline hidden", "thin.style.setProperty('scrollbar-width','none','important');true");
  add("inline hidden preserved", "getComputedStyle(thin).scrollbarWidth==='none'");
  add("inline important thin", "thin.style.setProperty('scrollbar-width','thin','important');true");
  add("inline thin normalized", "getComputedStyle(thin).scrollbarWidth==='auto'");
  add("dynamic node", "window.dynamic=thin.cloneNode(true);dynamic.id='dynamic';dynamic.style.cssText='scrollbar-width:thin!important';document.body.append(dynamic);true");
  add("new thin scroller normalized", "getComputedStyle(dynamic).scrollbarWidth==='auto'");
  add("variable width", "dynamic.style.cssText='--fixture-width:thin;scrollbar-width:var(--fixture-width)';true");
  add("variable thin normalized", "getComputedStyle(dynamic).scrollbarWidth==='auto'");
  add("variable hides scrollbar", "dynamic.style.setProperty('--fixture-width','none');true");
  add("variable none preserved", "getComputedStyle(dynamic).scrollbarWidth==='none'");
  add("selection and hover setup", "control.focus();window.hit=thin.getBoundingClientRect();true");
  add("scrollbar visible collapsed", "parseFloat(getComputedStyle(thin,'::-webkit-scrollbar-thumb').borderLeftWidth)===14");
  // Real pointer events, routed by Chromium to the browser-owned isolated adapter.
  add("prepare hover", "({x:hit.right-10,y:hit.top+24})");
  add("hover expands the styled thumb", "parseFloat(getComputedStyle(thin,'::-webkit-scrollbar-thumb').borderLeftWidth)===7");
  started=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(4,start,nullptr);}
