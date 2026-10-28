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
static std::shared_ptr<GnosisBrave::Page> page;
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
  page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(script)+",\"returnByValue\":true}",[done](JsonObject*r,const std::string&e){
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
static GtkWidget* command_entry(GtkWidget* node) {
  if(GTK_IS_ENTRY(node)&&gtk_widget_get_mapped(node)&&gtk_editable_get_text(GTK_EDITABLE(node))[0]==':')return node;
  for(auto*c=gtk_widget_get_first_child(node);c;c=gtk_widget_get_next_sibling(c))if(auto*r=command_entry(c))return r;
  return nullptr;
}
static void ex(const char* text,bool submit=true){key("colon");wait();add(text,[text]{auto*entry=command_entry(GTK_WIDGET(window));if(!entry)fail("native colon command entry missing");std::string command=":"+std::string(text);gtk_editable_set_text(GTK_EDITABLE(entry),command.c_str());});key("Return");wait();if(submit)add("native command completed",[]{if(command_entry(GTK_WIDGET(window)))fail("command did not complete");});}
static gboolean tick(gpointer) {
  if(g_get_monotonic_time()-started>240*G_USEC_PER_SEC)fail("timeout");
  if(busy||g_get_monotonic_time()<next)return G_SOURCE_CONTINUE;
  if(!ready){if(!page->session.empty())eval("document.readyState==='complete'&&location.pathname==='/keyboard-navigation.html'",[](JsonObject*r){ready=json_object_get_boolean_member_with_default(r,"value",false);if(ready)page->focus();});return G_SOURCE_CONTINUE;}
  if(step==steps.size()) {
    if(!cleaning){cleaning=true;page->navigate("about:blank");next=g_get_monotonic_time()+500000;return G_SOURCE_CONTINUE;}
    if(page->uri!="about:blank"||page->loading)return G_SOURCE_CONTINUE;
    g_printerr("KEYBOARD_GTK_PASS native F3, page control selection, nested field normal/insert, Unicode editing, undo/redo, safe Ctrl shortcuts, colon commands and isolation\n");
    page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  g_printerr("KEYBOARD_STEP %zu %s\n",step,steps[step].first.c_str());
  next=g_get_monotonic_time()+250000;steps[step++].second();return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto get=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  auto prefs=reinterpret_cast<GSettings*(*)(const char*)>(dlsym(RTLD_DEFAULT,"ephy_settings_get"));
  if(!embed(window))return G_SOURCE_CONTINUE;
  page=get(view(embed(window)));settings=prefs("com.neg-zero.Gnosis.web");
  auto* toolbarGroup=reinterpret_cast<GActionMap*(*)(void*,const char*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_action_group"))(window,"toolbar");
  if(!g_getenv("GNOSIS_KEYBOARD_PREVIEW"))g_signal_connect(g_action_map_lookup_action(toolbarGroup,"reload"),"activate",G_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){fail("field Ctrl+R reached browser reload");}),nullptr);

  g_settings_set_boolean(prefs("com.neg-zero.Gnosis"),"ask-for-default",false);
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
  key("j");expect("document.activeElement.id==='hint-button'");key("l");expect("document.activeElement.id==='right-button'");key("h");expect("document.activeElement.id==='hint-button'");
  key("j");key("i");key("a");expect("document.querySelector('#text').value===''");
  key("Return");expect("document.activeElement.id==='text'");key("z");expect("document.querySelector('#text').value===''");
  key("i");key("a");key("b");key("c");expect("document.querySelector('#text').value==='abc'&&trustedInputs.text==='abc'");
  key("Escape");key("h");key("x");expect("document.querySelector('#text').value==='ab'");key("u");expect("document.querySelector('#text').value==='abc'");
  key("ctrl+r");expect("document.querySelector('#text').value==='ab'&&location.pathname==='/keyboard-navigation.html'");
  key("A");key("space");key("w");key("o");key("r");key("d");expect("document.querySelector('#text').value==='ab word'");
  key("ctrl+w");expect("document.querySelector('#text').value==='ab '");key("Escape");
  ex("undo");expect("document.querySelector('#text').value==='ab word'");ex("redo");expect("document.querySelector('#text').value==='ab '");
  key("d");key("d");expect("document.querySelector('#text').value===''");key("p");expect("document.querySelector('#text').value==='ab '");
  key("0");key("3");
  ex("commands",false);key("Escape");key("x");expect("document.querySelector('#text').value==='b '");key("u");expect("document.querySelector('#text').value==='ab '");ex("not-a-command",false);key("Escape");
  key("Escape");key("k");key("Return");expect("hintClicks===1");key("d");expect("scrollY>0");key("u");
  script("scrollTo(0,0);document.activeElement?.blur()");key("f");key("a");expect("hintClicks===1");key("Return");expect("hintClicks===2");
  script("document.querySelector('#area').focus()");key("Return");key("i");key("a");key("Return");key("b");expect("document.querySelector('#area').value==='a\\nb'");
  key("Escape");key("0");key("k");key("x");expect("document.querySelector('#area').value==='\\nb'");key("u");expect("document.querySelector('#area').value==='a\\nb'");key("Escape");
  script("document.querySelector('#editor').focus()");key("Return");key("i");key("j");expect("document.querySelector('#editor').textContent.includes('j')");key("Escape");key("Escape");
  script("document.querySelector('#shadow').shadowRoot.querySelector('input').focus()");key("Return");key("i");key("j");expect("document.querySelector('#shadow').shadowRoot.querySelector('input').value==='j'");key("Escape");key("Escape");
  script("focusClosed()");key("a");expect("closedValue()===''");key("Return");key("i");key("j");expect("closedValue()==='j'");key("Escape");key("Escape");
  script("document.querySelector('#hint-button').focus();scrollTo(0,0)");
  key("G");expect("Math.abs(scrollY-(document.scrollingElement.scrollHeight-innerHeight))<2");
  key("g");key("g");expect("scrollY===0");
  script("document.querySelector('#panel').focus()");key("G");expect("document.querySelector('#panel').scrollTop===document.querySelector('#panel').scrollHeight-document.querySelector('#panel').clientHeight");
  key("g");key("g");expect("document.querySelector('#panel').scrollTop===0");
  script("document.querySelector('#text').value='🧪x';document.querySelector('#text').setSelectionRange(0,0);document.querySelector('#text').focus()");key("Return");key("x");expect("document.querySelector('#text').value==='x'");key("Escape");
  key("slash");wait();add("Vim search is native",[]{if(!gtk_widget_get_mapped(entry))fail("Vim / did not open GTK find");gtk_editable_set_text(GTK_EDITABLE(entry),"alpha");});wait();key("Escape");
  expect("getSelection().toString()==='alpha'");
  add("focus page again",[]{page->focus();});script("document.activeElement?.blur();scrollTo(0,0)");
  key("n");wait();add("Vim match focus",[]{g_object_get(entry,"current-match",&ordinal,nullptr);if(!ordinal||GTK_IS_TEXT(gtk_window_get_focus(window)))fail("Vim n stole page focus");});
  key("n");wait();add("repeated Vim next",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal%3+1)fail("repeated Vim n did not advance");});
  key("N");wait();add("Vim previous",[]{guint current=0;g_object_get(entry,"current-match",&current,nullptr);if(current!=ordinal)fail("Vim N did not return");});
  key("slash");wait();add("search a control",[]{gtk_editable_set_text(GTK_EDITABLE(entry),"Right action");});wait();key("Return");
  add("confirmed search focuses page",[]{if(gtk_widget_get_mapped(entry)||GTK_IS_TEXT(gtk_window_get_focus(window)))fail("Vim Enter did not return to page");});expect("getSelection().toString()==='Right action'");key("Return");expect("rightClicks===1");
  key("slash");wait();add("search a cross-process control",[]{gtk_editable_set_text(GTK_EDITABLE(entry),"OOPIF search control");});wait();key("Return");key("Return");expect("childActivated===true");
  expect("typeof gnosisVim==='undefined'&&typeof gnosisVimField==='undefined'&&typeof __gnosisDocumentMetadata==='undefined'");
  script("document.querySelector('#frame').contentWindow.focus()");key("g");key("g");key("Return");key("i");key("a");key("b");expect("childValue==='ab'");
  key("Escape");key("h");key("x");expect("childValue==='a'");key("u");expect("childValue==='ab'");key("ctrl+r");expect("childValue==='a'");key("Escape");
  script("document.activeElement?.blur();document.body.tabIndex=-1;document.body.focus()");
  ex("tabopen http://localhost:18473/keyboard-child.html");
  ex("set novim");expect("document.querySelector('#text').value==='x'");
  add("Vim disabled through command",[]{if(g_settings_get_boolean(settings,"enable-vim-mode"))fail("set novim did not update settings");});
  script("document.activeElement?.blur();scrollTo(0,0);pageKeys=[]");key("j");expect("pageKeys.includes('j')");
  started=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(4,start,nullptr);}
