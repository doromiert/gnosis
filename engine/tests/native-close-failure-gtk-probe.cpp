/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fault-inject a missing native preflight reply in a disposable GTK profile.
 * The real browser remains sandboxed and its real frame handlers still run.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>
static GtkWindow* window;
static AdwTabView* tabs;
static AdwDialog* answered;
static std::shared_ptr<GnosisBrave::Page> page;
static int stage;
static bool testing, busy;
static gint64 started, changed;
static void fail(const char* reason) {
  g_printerr("CLOSE_FAILURE_GTK_FAIL stage=%d %s\n",stage,reason); _exit(77);
}
extern "C" ssize_t recv(int fd,void* buffer,size_t size,int flags) {
  auto real=reinterpret_cast<ssize_t(*)(int,void*,size_t,int)>(dlsym(RTLD_NEXT,"recv"));
  auto n=real(fd,buffer,size,flags);
  if (testing && n>0 && std::string(static_cast<char*>(buffer),std::min(size,size_t(n))).find("beforeunload-result")!=std::string::npos) {
    errno=EAGAIN; return -1;
  }
  return n;
}
static GtkWidget* find(GtkWidget* widget,const char* type,const char* label=nullptr) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget),type)) {
    if (!label) return widget;
    if (GTK_IS_BUTTON(widget)) {
      const auto* text=gtk_button_get_label(GTK_BUTTON(widget)); std::string clean;
      for (;text&&*text;++text) if (*text!='_') clean+=*text;
      if (clean==label) return widget;
    }
  }
  for (auto* child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))
    if (auto* result=find(child,type,label)) return result;
  return nullptr;
}
static void respond(AdwDialog* dialog,const char* label) {
  auto* button=find(GTK_WIDGET(dialog),"GtkButton",label);
  if (!button) fail("response button missing");
  g_set_weak_pointer(&answered,dialog); gtk_widget_activate(button);
}
static gboolean tick(gpointer) {
  if (g_get_monotonic_time()-started>100*G_USEC_PER_SEC) fail("timeout");
  if (!window) {
    if (stage!=4) fail("window closed without explicit approval");
    page.reset(); g_printerr("CLOSE_FAILURE_GTK_PASS missing native replies offer real Keep Open and Close Anyway\n");
    g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);
    return G_SOURCE_REMOVE;
  }
  if (busy) return G_SOURCE_CONTINUE;
  auto* dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if (dialog==answered) dialog=nullptr;
  if (dialog&&ADW_IS_ALERT_DIALOG(dialog)&&strstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)),"Default Browser")) {
    respond(dialog,"No"); return G_SOURCE_CONTINUE;
  }
  if (stage==0&&!page->session.empty()) {
    busy=true;
    page->command("Runtime.evaluate","{\"expression\":\"location.pathname==='/native-find.html'&&document.readyState==='complete'\",\"returnByValue\":true}",[](JsonObject* result,const std::string&) {
      busy=false; auto* value=result?json_object_get_object_member(result,"result"):nullptr;
      if (value&&json_object_get_boolean_member_with_default(value,"value",false)) {
        stage=1; adw_tab_view_close_page(tabs,adw_tab_view_get_selected_page(tabs));
      }
    });
  } else if ((stage==1||stage==3)&&dialog&&ADW_IS_ALERT_DIALOG(dialog)) {
    if (strcmp(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)),"Close This Page?")) {g_printerr("RECOVERY_HEADING %s\n",adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)));fail("wrong recovery dialog");}
    bool accept=stage==3; stage=accept?4:2; changed=g_get_monotonic_time();
    respond(dialog,accept?"Close Anyway":"Keep Open");
  } else if (stage==2&&g_get_monotonic_time()-changed>500000) {
    if (adw_tab_view_get_n_pages(tabs)!=1||page->is_closed()) fail("Keep Open lost the page");
    g_printerr("CLOSE_FAILURE_KEEP_OPEN_PASS\n");
    stage=3; adw_tab_view_close_page(tabs,adw_tab_view_get_selected_page(tabs));
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
  if (!window) return G_SOURCE_CONTINUE;
  g_object_add_weak_pointer(G_OBJECT(window),reinterpret_cast<gpointer*>(&window));
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if (!embed(window)) return G_SOURCE_CONTINUE;
  page=getpage(view(embed(window))); tabs=ADW_TAB_VIEW(find(GTK_WIDGET(window),"AdwTabView"));
  if (!tabs) fail("tab view missing");
  g_signal_connect(g_application_get_default(),"shutdown",G_CALLBACK(+[](GApplication*,gpointer) {
    if (stage!=4) fail("shutdown without explicit force-close approval");
    page.reset(); g_printerr("CLOSE_FAILURE_GTK_PASS missing native replies offer real Keep Open and Close Anyway\n");
  }),nullptr);
  started=g_get_monotonic_time(); g_timeout_add(100,tick,nullptr); return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char executable[2048]; auto n=readlink("/proc/self/exe",executable,sizeof executable-1);
  if (n<0) return; executable[n]=0;
  testing=strstr(executable,"/.gnosis-wrapped");
  if (testing) g_timeout_add_seconds(5,start,nullptr);
}
