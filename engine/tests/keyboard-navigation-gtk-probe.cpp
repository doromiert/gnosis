/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual GTK/XTest keyboard input, isolated page adapters and live GSettings.
 * Run on an isolated X11 display, with GNOSIS_XDOTOOL pointing to xdotool.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
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
static GSettings* settings;
static std::shared_ptr<JupiterBrave::Page> page;
static std::vector<std::pair<std::string,std::function<void()>>> steps;
static size_t step;
static bool busy,ready,cleaning;
static gint64 started,next;
static guint ordinal;
static GtkWidget* entry;
static void fail(const char* reason) { g_printerr("KEYBOARD_GTK_FAIL step=%zu %s\n",step,reason); _exit(77); }
static GtkWidget* find(GtkWidget* node,const char* type) {
  if(!strcmp(G_OBJECT_TYPE_NAME(node),type))return node;
  for(auto*c=gtk_widget_get_first_child(node);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find(c,type))return r;
  return nullptr;
}
static GtkWidget* vim_row(GtkWidget* node) {
  if(ADW_IS_SWITCH_ROW(node)&&strstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(node)),"Vim Mode"))return node;
  for(auto*c=gtk_widget_get_first_child(node);c;c=gtk_widget_get_next_sibling(c))if(auto*r=vim_row(c))return r;
  return nullptr;
}
static void eval(const std::string& script,std::function<void(JsonObject*)> done) {
  busy=true;
  page->command("Runtime.evaluate","{\"expression\":"+JupiterBrave::quote(script)+",\"returnByValue\":true}",[done](JsonObject*r,const std::string&e){
    busy=false;if(!r||!e.empty()||json_object_has_member(r,"exceptionDetails"))fail("script evaluation");
    done(json_object_get_object_member(r,"result"));
  });
}
static void add(const char* name,std::function<void()> run){steps.emplace_back(name,run);}
static void script(const char* code){add(code,[code]{eval(code,[](JsonObject*){});});}
static void expect(const char* code){add(code,[code]{eval(code,[code](JsonObject*r){if(!r||!json_object_get_boolean_member_with_default(r,"value",false))fail(code);});});}
static void key(const char* keys) {
  add(keys,[keys]{
    const char* tool=g_getenv("GNOSIS_XDOTOOL");if(!tool)fail("GNOSIS_XDOTOOL missing");
    auto* surface=gtk_native_get_surface(GTK_NATIVE(window));
    g_autofree char* id=g_strdup_printf("%lu",gdk_x11_surface_get_xid(surface));
    char* focus[]={const_cast<char*>(tool),const_cast<char*>("windowfocus"),id,nullptr};
    int status=0;
    if(!g_spawn_sync(nullptr,focus,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest window focus");
    char* command[]={const_cast<char*>(tool),const_cast<char*>("key"),const_cast<char*>("--clearmodifiers"),const_cast<char*>(keys),nullptr};
    if(!g_spawn_sync(nullptr,command,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest key event");

  });
}
static void wait(){add("settle",[]{next=g_get_monotonic_time()+800000;});}
static gboolean tick(gpointer) {
  if(g_get_monotonic_time()-started>150*G_USEC_PER_SEC)fail("timeout");
  if(busy||g_get_monotonic_time()<next)return G_SOURCE_CONTINUE;
  if(!ready){if(!page->session.empty())eval("document.readyState==='complete'&&location.pathname==='/keyboard-navigation.html'",[](JsonObject*r){ready=json_object_get_boolean_member_with_default(r,"value",false);if(ready)page->focus();});return G_SOURCE_CONTINUE;}
  if(step==steps.size()) {
    if(!cleaning){cleaning=true;page->navigate("about:blank");next=g_get_monotonic_time()+500000;return G_SOURCE_CONTINUE;}
    if(page->uri!="about:blank"||page->loading)return G_SOURCE_CONTINUE;
    g_printerr("KEYBOARD_GTK_PASS hardware F3/Shift+F3, native find, Vim scroll/counts/hints, editing guards, insert mode, isolation and live setting changes\n");
    page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  g_printerr("KEYBOARD_STEP %zu %s\n",step,steps[step].first.c_str());
  next=g_get_monotonic_time()+250000;steps[step++].second();return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto get=reinterpret_cast<std::shared_ptr<JupiterBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  auto prefs=reinterpret_cast<GSettings*(*)(const char*)>(dlsym(RTLD_DEFAULT,"ephy_settings_get"));
  if(!embed(window))return G_SOURCE_CONTINUE;
  page=get(view(embed(window)));settings=prefs("io.github.doromiert.Jupiter.web");
  g_settings_set_boolean(prefs("io.github.doromiert.Jupiter"),"ask-for-default",false);
  if(auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))){
    g_printerr("KEYBOARD_STARTUP_DIALOG %s %s\n",adw_dialog_get_title(d),ADW_IS_ALERT_DIALOG(d)?adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(d)):"");
    adw_dialog_force_close(d);
  }

  if(g_getenv("GNOSIS_KEYBOARD_PREVIEW")){
    g_timeout_add(800,+[](gpointer)->gboolean{g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"preferences",nullptr);return G_SOURCE_REMOVE;},nullptr);
    return G_SOURCE_REMOVE;
  }

  key("F3");wait();
  add("native find opened",[]{auto getToolbar=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_current_find_toolbar"));auto*t=getToolbar(window);entry=t?find(t,"EphySearchEntry"):nullptr;if(!entry||!gtk_widget_get_mapped(entry))fail("F3 did not open GTK find");gtk_editable_set_text(GTK_EDITABLE(entry),"alpha");});wait();
  add("initial native matches",[]{guint count=0;g_object_get(entry,"n-matches",&count,"current-match",&ordinal,nullptr);if(count!=3||!ordinal)fail("initial find count");});
  key("F3");wait();
  add("one next match",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal%3+1)fail("F3 did not advance exactly once");});
  key("shift+F3");wait();
  add("previous match",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal)fail("Shift+F3 previous match");});
  key("Escape");add("focus page",[]{page->focus();});
  script("getSelection().selectAllChildren(document.querySelector('#hint-button'))");key("F3");wait();
  add("F3 preserves the last query",[]{g_printerr("KEYBOARD_QUERY_AFTER_F3 %s\n",gtk_editable_get_text(GTK_EDITABLE(entry)));if(strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)),"alpha"))fail("F3 replaced last query with selected page text");});key("Escape");add("page focus",[]{page->focus();});
  script("document.activeElement?.blur();scrollTo(0,0);pageKeys=[]");key("j");expect("scrollY===0&&pageKeys.includes('j')");
  add("open preferences",[]{g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"preferences",nullptr);});wait();
  add("enable Vim through its native switch",[]{auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));auto*row=d?vim_row(GTK_WIDGET(d)):nullptr;if(!row||adw_switch_row_get_active(ADW_SWITCH_ROW(row)))fail("Vim preference missing or enabled by default");adw_switch_row_set_active(ADW_SWITCH_ROW(row),true);if(!g_settings_get_boolean(settings,"enable-vim-mode"))fail("native switch binding");adw_dialog_force_close(d);});wait();
  add("restore page focus",[]{page->focus();});wait();
  script("pageKeys=[]");key("j");expect("scrollY===60&&!pageKeys.includes('j')");
  key("k");expect("scrollY===0");key("3");key("j");expect("scrollY===180");
  key("G");expect("scrollY>1000");key("g");key("g");expect("scrollY===0");
  script("document.querySelector('#text').focus()");key("j");expect("document.querySelector('#text').value==='j'&&scrollY===0");
  key("Escape");key("j");expect("scrollY===60");
  script("scrollTo(0,0);document.querySelector('#area').focus()");key("j");expect("document.querySelector('#area').value==='j'&&scrollY===0");
  script("document.querySelector('#editor').focus()");key("j");expect("document.querySelector('#editor').textContent.includes('j')&&scrollY===0");
  script("document.querySelector('#choice').focus();pageKeys=[]");key("j");expect("pageKeys.includes('j')&&scrollY===0");
  script("document.querySelector('#panel').focus()");key("j");expect("document.querySelector('#panel').scrollTop===60&&scrollY===0");
  script("focusClosed()");key("j");expect("closedValue()==='j'&&scrollY===0");key("Escape");
  script("document.querySelector('#shadow').shadowRoot.querySelector('input').focus()");key("j");expect("document.querySelector('#shadow').shadowRoot.querySelector('input').value==='j'&&scrollY===0");
  script("document.querySelector('#shadow').shadowRoot.activeElement.blur();document.activeElement?.blur();pageKeys=[]");
  key("i");key("j");expect("scrollY===0&&pageKeys.includes('j')");key("Escape");key("j");expect("scrollY===60");
  script("scrollTo(0,0);document.activeElement?.blur()");key("f");key("a");expect("hintClicks===1");
  key("slash");add("Vim search is native",[]{if(!gtk_widget_get_mapped(entry))fail("Vim / did not open GTK find");gtk_editable_set_text(GTK_EDITABLE(entry),"alpha");});wait();key("Escape");
  add("focus page again",[]{page->focus();});script("document.activeElement?.blur();scrollTo(0,0)");
  key("n");wait();add("Vim match focus",[]{g_object_get(entry,"current-match",&ordinal,nullptr);if(!ordinal||GTK_IS_TEXT(gtk_window_get_focus(window)))fail("Vim n stole page focus");});
  key("n");wait();add("repeated Vim next",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal%3+1)fail("repeated Vim n did not advance");});
  key("N");wait();add("Vim previous",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal)fail("Vim N did not return");});
  script("dispatchEvent(new KeyboardEvent('keydown',{key:'j',bubbles:true}))");expect("scrollY===0");
  expect("typeof gnosisVim==='undefined'&&typeof __jupiterDocumentMetadata==='undefined'");
  script("document.querySelector('#frame').contentWindow.focus()");key("j");expect("childScroll===60");
  key("slash");add("child frame Vim action",[]{if(!gtk_widget_get_mapped(entry)||!GTK_IS_TEXT(gtk_window_get_focus(window)))fail("child Vim search did not reach native GTK");});key("Escape");
  script("document.activeElement?.blur();document.body.tabIndex=-1;document.body.focus();scrollTo(0,0)");
  add("disable Vim live",[]{g_settings_set_boolean(settings,"enable-vim-mode",false);});wait();
  script("pageKeys=[]");key("j");expect("scrollY===0&&pageKeys.includes('j')");
  started=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(4,start,nullptr);}
