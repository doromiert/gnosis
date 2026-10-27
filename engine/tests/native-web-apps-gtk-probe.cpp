#include "browser.hpp"
#include "ephy-web-app-dialog.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <glib/gstdio.h>
#include "web-app-config.h"
static GtkWindow *window;
static std::shared_ptr<GnosisBrave::Page> page;
static EphyWebAppDialog *setup;
static GtkWidget *grid;
static gint64 started, changed;
static int stage;
static bool busy;
static guint icon_count;
static GtkWidget *find(GtkWidget *widget,const char *type) {
 if(!strcmp(G_OBJECT_TYPE_NAME(widget),type))return widget;
 for(auto* child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))if(auto* found=find(child,type))return found;
 return nullptr;
}
static GtkWidget *button(GtkWidget*widget,const char*label) {
 if(GTK_IS_BUTTON(widget)&&!g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)),label))return widget;
 for(auto* child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))if(auto* found=button(child,label))return found;
 return nullptr;
}
static void fail(const char *message){g_printerr("WEBAPPS_FAIL stage=%d %s\n",stage,message);_exit(77);}
static void check_launcher() {
 auto* app=g_new0(EphyWebApplication,1);app->id=g_compute_checksum_for_string(G_CHECKSUM_SHA1,"gnosis-owned-launcher-check",-1);
 auto* profile=ephy_web_application_get_profile_directory(app->id);g_mkdir_with_parents(profile,0700);
 app->name=g_strdup("Launcher check");app->url=g_strdup("http://example.test/path");app->icon_path=g_strdup("folder");app->desktop_path=g_build_filename(profile,"fixture.desktop",nullptr);
 const char* original="[Desktop Entry]\nType=Application\nName=Launcher check\nIcon=folder\nExec=\"/nix/store/old-gnosis/bin/gnosis\" --application-mode --profile=/tmp/fixture --custom-test-flag http://example.test/path\n";
 if(!g_file_set_contents(app->desktop_path,original,-1,nullptr)||!ephy_web_application_save(app))fail("could not update old launcher");
 auto* key=g_key_file_new();if(!g_key_file_load_from_file(key,app->desktop_path,G_KEY_FILE_NONE,nullptr))fail("updated launcher unreadable");
 auto* command=g_key_file_get_string(key,"Desktop Entry","Exec",nullptr);char exe[2048];auto size=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(size<0)fail("no executable path");exe[size]=0;
 auto current=std::string(exe);current=current.substr(0,current.find_last_of('/'))+"/gnosis";
 auto expected="\""+current+"\" --application-mode --profile=/tmp/fixture --custom-test-flag http://example.test/path";
 if(expected!=command)fail("launcher did not refresh its executable while keeping arguments");
 if(!ephy_web_application_save(app))fail("unchanged launcher save failed");
 auto* config=g_key_file_new();g_key_file_set_string_list(config,"Extensions","enabled",nullptr,0);GError*error=nullptr;
 if(!ephy_web_application_set_extensions(app->id,config,&error))fail(error?error->message:"could not persist app extension choice");
 auto* saved=gnosis_web_app_config_load(profile,&error);if(!saved||!g_key_file_has_group(saved,"Extensions"))fail("saved extension choice missing");
 auto* settings=g_build_filename(profile,GNOSIS_WEB_APP_CONFIG,nullptr);g_unlink(settings);g_unlink(app->desktop_path);g_rmdir(profile);
 g_free(settings);g_free(profile);g_free(command);g_key_file_unref(key);g_key_file_unref(saved);g_key_file_unref(config);ephy_web_application_free(app);
 g_print("WEBAPP_LAUNCHER_PASS current Nix executable, preserved arguments, idempotent save, per-app config persistence\n");
}
static gboolean tick(gpointer) {
 auto now=g_get_monotonic_time();if(now-started>35*G_USEC_PER_SEC)fail("timeout");
 if(busy||page->session.empty()||page->loading||now-changed<300000)return G_SOURCE_CONTINUE;
 if(g_getenv("GNOSIS_TEST_WEBAPP_LAUNCHER")){check_launcher();page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
 if(g_getenv("GNOSIS_TEST_WEBAPP_POLICY")) {
  if(page->title!="Native web app test")return G_SOURCE_CONTINUE;
  busy=true;
  if(!GnosisBrave::extensions_call("inventory","",[](JsonObject* result,const std::string& error){
   busy=false;if(!error.empty())fail(error.c_str());
   auto* remote=result?json_object_get_object_member(result,"result"):nullptr;
   auto* inventory=remote?json_object_get_array_member(remote,"value"):nullptr;
   if(!inventory)return;
   const char* expected=g_getenv("GNOSIS_TEST_WEBAPP_EXTENSION");bool found=false;
   for(guint i=0;i<json_array_get_length(inventory);i++) {
    auto* row=json_array_get_object_element(inventory,i);
    const char* id=json_object_get_string_member_with_default(row,"id","");
    bool enabled=json_object_get_boolean_member_with_default(row,"enabled",false);
    if(expected&&!strcmp(expected,id)&&enabled)found=true;
    else if(enabled)fail("an unselected extension is enabled");
   }
   if(expected&&!found)fail("selected extension was not loaded");
   g_print("WEBAPP_POLICY_PASS selected=%s\n",expected?expected:"none");
   page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);
  }))busy=false;
  return G_SOURCE_CONTINUE;
 }
 if(stage==0) {
  auto* config=g_key_file_new();g_key_file_set_string_list(config,"Extensions","enabled",nullptr,0);
  auto* icon=g_themed_icon_new("folder");setup=EPHY_WEB_APP_DIALOG(ephy_web_app_dialog_new("Fixture Web App","http://localhost:5173",icon,config,TRUE));g_object_unref(icon);g_key_file_unref(config);
  adw_dialog_present(ADW_DIALOG(setup),GTK_WIDGET(window));
 } else if(stage==1) {
  auto* choice=button(GTK_WIDGET(setup),"Choose from Icon Theme…");if(!choice)fail("theme choice absent");
  auto* popover=gtk_widget_get_ancestor(choice,GTK_TYPE_POPOVER);
  auto* menu=gtk_widget_get_parent(popover);gtk_menu_button_popup(GTK_MENU_BUTTON(menu));g_signal_emit_by_name(choice,"clicked");
 } else if(stage==2) {
  grid=find(GTK_WIDGET(window),"GtkGridView");if(!grid)return G_SOURCE_CONTINUE;
  auto* entry=find(gtk_widget_get_ancestor(grid,ADW_TYPE_DIALOG),"GtkSearchEntry");if(!entry)fail("theme search absent");
  icon_count=g_list_model_get_n_items(G_LIST_MODEL(gtk_grid_view_get_model(GTK_GRID_VIEW(grid))));
  if(!icon_count)fail("theme has no icons");gtk_editable_set_text(GTK_EDITABLE(entry),"folder");
 } else if(stage==3) {
  auto* model=G_LIST_MODEL(gtk_grid_view_get_model(GTK_GRID_VIEW(grid)));auto count=g_list_model_get_n_items(model);
  g_print("WEBAPP_ICON_SEARCH total=%u filtered=%u\n",icon_count,count);
  if(!count||count>=icon_count)fail("theme search did not filter");
  g_signal_emit_by_name(grid,"activate",0);
 } else if(stage==4) {
  GError* error=nullptr;auto* bytes=ephy_web_app_dialog_dup_icon_bytes(setup,&error);
  if(!bytes)fail(error?error->message:"no icon bytes");gsize size=0;auto* png=(const char*)g_bytes_get_data(bytes,&size);
  if(size<8||memcmp(png,"\211PNG\r\n\032\n",8))fail("selected theme icon did not produce a portal-compatible PNG");
  auto* icon=ephy_web_app_dialog_get_icon(setup);if(!G_IS_THEMED_ICON(icon))fail("theme icon choice lost");
  auto* config=ephy_web_app_dialog_dup_config(setup);gsize selected=99;auto** list=g_key_file_get_string_list(config,"Extensions","enabled",&selected,nullptr);
  if(selected!=0)fail("empty per-app extension choice lost");g_strfreev(list);g_key_file_unref(config);g_bytes_unref(bytes);
  g_print("WEBAPP_THEME_ICON_PASS searchable XDG theme, PNG export, explicit no extensions\n");
  adw_dialog_force_close(ADW_DIALOG(setup));
 } else if(stage==5) {
  if(!gtk_widget_activate_action(GTK_WIDGET(window),"win.open-application-manager",nullptr))fail("manager action missing");
 } else if(stage==6) {
  if(!find(GTK_WIDGET(window),"PrefsWebAppsPage"))fail("native management page missing");
  auto* dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if(!ADW_IS_PREFERENCES_DIALOG(dialog)||g_strcmp0(adw_preferences_dialog_get_visible_page_name(ADW_PREFERENCES_DIALOG(dialog)),"web-apps-page"))fail("manager did not open Settings Web Apps");
  g_print("WEBAPP_NATIVE_MANAGER_PASS menu action opens GTK settings\n");
  adw_dialog_force_close(dialog);page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
 }
 ++stage;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto* app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
 window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto view=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 if(!embed(window))return G_SOURCE_CONTINUE;page=getpage(view(embed(window)));if(!page)return G_SOURCE_CONTINUE;
 started=changed=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup_probe(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
