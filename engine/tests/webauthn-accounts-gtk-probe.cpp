/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real WebAuthn assertion, virtual CTAP authenticator and actual GTK consent.
 * The two deterministic keys are public synthetic localhost test fixtures.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWindow* window;
static std::shared_ptr<JupiterBrave::Page> page;
static std::string authenticator;
static JsonArray* fixtures;
static int stage;
static bool busy;
static gint64 started;
static void fail(const char* reason){g_printerr("ACCOUNT_GTK_FAIL stage=%d %s\n",stage,reason);_exit(77);}
static GtkWidget* row(GtkWidget*w,const char*name){if(ADW_IS_ACTION_ROW(w)&&!g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(w)),name))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=row(c,name))return r;return nullptr;}
static GtkWidget* button(GtkWidget*w,const char*name){if(GTK_IS_BUTTON(w)){const char*t=gtk_button_get_label(GTK_BUTTON(w));std::string clean;for(;t&&*t;t++)if(*t!='_')clean+=*t;if(clean==name)return w;}for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=button(c,name))return r;return nullptr;}
static void check_layout(GtkWidget* dialog,GtkWidget* account){
  auto* cancel=button(dialog,"Cancel");
  if(!cancel||!gtk_widget_get_mapped(cancel))fail("Cancel is not visible");
  graphene_rect_t account_bounds,cancel_bounds;
  if(!gtk_widget_compute_bounds(account,dialog,&account_bounds)||!gtk_widget_compute_bounds(cancel,dialog,&cancel_bounds))fail("dialog layout unavailable");
  const int height=gtk_widget_get_height(dialog);
  if(gtk_widget_get_height(adw_dialog_get_child(ADW_DIALOG(dialog)))>600)fail("small account list makes an excessively tall dialog");
  if(account_bounds.origin.y<0||account_bounds.origin.y+account_bounds.size.height>height||cancel_bounds.origin.y+cancel_bounds.size.height>height)fail("account or footer is clipped");
  for(auto*w=gtk_widget_get_parent(account);w&&w!=dialog;w=gtk_widget_get_parent(w))if(GTK_IS_SCROLLED_WINDOW(w)){
    graphene_rect_t bounds;if(!gtk_widget_compute_bounds(account,w,&bounds)||bounds.origin.y<0||bounds.origin.y+bounds.size.height>gtk_widget_get_height(w))fail("second account clipped by scroll viewport");
  }
  g_printerr("ACCOUNT_LAYOUT_PASS content height=%d, both accounts and Cancel visible\n",gtk_widget_get_height(adw_dialog_get_child(ADW_DIALOG(dialog))));
}
static GtkWidget* qr(GtkWidget*w){if(GTK_IS_DRAWING_AREA(w))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=qr(c))return r;return nullptr;}
static void command(const char*method,const std::string&params,std::function<void(JsonObject*)> next){busy=true;page->command(method,params,[next](JsonObject*r,const std::string&e){busy=false;if(!e.empty()||!r){g_printerr("ACCOUNT_PROTOCOL_ERROR %s\n",e.c_str());fail("native test setup");}if(json_object_has_member(r,"exceptionDetails"))fail("fixture script error");next(r);});}
static void add(unsigned i,int next){auto*c=json_object_get_object_member(json_array_get_object_element(fixtures,i),"credential");g_autoptr(JsonNode)n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,c);g_autofree char*json=json_to_string(n,false);command("WebAuthn.addCredential","{\"authenticatorId\":"+JupiterBrave::quote(authenticator)+",\"credential\":"+json+"}",[next](JsonObject*){stage=next;});}
static void request(int round,int next){auto*pub=json_object_get_object_member(json_array_get_object_element(fixtures,1),"publicKey");g_autoptr(JsonNode)n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,pub);g_autofree char*json=json_to_string(n,false);std::string expression="startAccountRequest("+std::string(json)+","+std::to_string(round)+")";command("Runtime.evaluate","{\"expression\":"+JupiterBrave::quote(expression)+",\"userGesture\":true,\"returnByValue\":true}",[next](JsonObject*){stage=next;});}
static gboolean tick(gpointer){if(g_get_monotonic_time()-started>110*G_USEC_PER_SEC)fail("timeout");static int previous=-1;if(previous!=stage){g_printerr("ACCOUNT_STAGE %d\n",stage);previous=stage;}if(busy)return G_SOURCE_CONTINUE;
if(stage==0&&!page->session.empty())command("Runtime.evaluate","{\"expression\":\"({ready:typeof startAccountRequest==='function'&&document.readyState==='complete'})\",\"returnByValue\":true}",[](JsonObject*r){auto*v=json_object_get_object_member(r,"result");auto*o=v?json_object_get_object_member(v,"value"):nullptr;if(o&&json_object_get_boolean_member_with_default(o,"ready",false))stage=1;});
else if(stage==1)command("WebAuthn.enable","{\"enableUI\":true}",[](JsonObject*){stage=2;});
else if(stage==2)command("WebAuthn.addVirtualAuthenticator","{\"options\":{\"protocol\":\"ctap2\",\"transport\":\"usb\",\"hasResidentKey\":true,\"hasUserVerification\":true,\"automaticPresenceSimulation\":true,\"isUserVerified\":true}}",[](JsonObject*r){authenticator=json_object_get_string_member(r,"authenticatorId");stage=3;});
else if(stage==3)add(0,4);
else if(stage==4)add(1,5);
else if(stage==5)request(1,6);
else if(stage==6){auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(d){auto*b=row(GTK_WIDGET(d),"bob@example.test");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){check_layout(GTK_WIDGET(d),b);gtk_widget_activate(b);stage=7;}}}
else if(stage==7)command("Runtime.evaluate","{\"expression\":\"({result:accountResult,error:accountError})\",\"returnByValue\":true}",[](JsonObject*r){auto*o=json_object_get_object_member(json_object_get_object_member(r,"result"),"value");auto*error=json_object_get_member(o,"error");if(error&&!JSON_NODE_HOLDS_NULL(error))fail("assertion rejected");auto*node=json_object_get_member(o,"result");if(!node||JSON_NODE_HOLDS_NULL(node))return;auto*v=json_node_get_object(node);if(g_strcmp0(json_object_get_string_member(v,"user"),"bob")||!json_object_get_boolean_member(v,"verified")||!json_object_get_boolean_member(v,"uv")||json_object_get_int_member(v,"counter")<1)fail("GTK selection did not return Bob's valid native assertion");g_printerr("ACCOUNT_ASSERTION_PASS selected GTK row returns Bob, real verified ECDSA signature, UV and counter\n");stage=8;});
else if(stage==8)request(2,9);
else if(stage==9){auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(d&&row(GTK_WIDGET(d),"alice@example.test")){auto*b=button(GTK_WIDGET(d),"Cancel");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){gtk_widget_activate(b);stage=10;}}}
else if(stage==10)command("Runtime.evaluate","{\"expression\":\"({result:accountResult,error:accountError})\",\"returnByValue\":true}",[](JsonObject*r){auto*o=json_object_get_object_member(json_object_get_object_member(r,"result"),"value");auto*error=json_object_get_member(o,"error");if(!error||JSON_NODE_HOLDS_NULL(error))return;if(g_strcmp0(json_node_get_string(error),"NotAllowedError"))fail("Cancel did not reject actual WebAuthn request");stage=12;});
else if(stage==12)request(3,13);
else if(stage==13){auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(d&&row(GTK_WIDGET(d),"alice@example.test")){if(!row(GTK_WIDGET(d),"Use a Phone or Tablet"))fail("native phone mechanism missing");command("WebAuthn.setAutomaticPresenceSimulation","{\"authenticatorId\":"+JupiterBrave::quote(authenticator)+",\"enabled\":false}",[](JsonObject*){stage=14;});}}
else if(stage==14){auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(d){auto*b=row(GTK_WIDGET(d),"Use a Phone or Tablet");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){gtk_widget_activate(b);stage=15;}}}
else if(stage==15){auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(d&&qr(GTK_WIDGET(d))){auto*b=button(GTK_WIDGET(d),"Cancel");if(b&&gtk_widget_get_mapped(b)&&gtk_widget_is_sensitive(b)){g_printerr("ACCOUNT_PHONE_PASS account choice restarts native discovery and opens real phone QR\n");gtk_widget_activate(b);stage=16;}}}
else if(stage==16)command("Runtime.evaluate","{\"expression\":\"({error:accountError})\",\"returnByValue\":true}",[](JsonObject*r){auto*o=json_object_get_object_member(json_object_get_object_member(r,"result"),"value");auto*error=json_object_get_member(o,"error");if(!error||JSON_NODE_HOLDS_NULL(error))return;if(g_strcmp0(json_node_get_string(error),"NotAllowedError"))fail("Cancel from QR did not reject actual request");g_printerr("ACCOUNT_GTK_PASS verified assertion, unclipped account rows, account cancellation and phone QR cancellation\n");stage=11;page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);});
return stage==11?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;}
static gboolean start(gpointer){window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto p=reinterpret_cast<std::shared_ptr<JupiterBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));if(!e(window))return G_SOURCE_CONTINUE;page=p(v(e(window)));g_autoptr(JsonParser)parser=json_parser_new();if(!json_parser_load_from_file(parser,"engine/tests/fixtures/webauthn-accounts.json",nullptr))fail("fixture keys missing");fixtures=json_array_ref(json_node_get_array(json_parser_get_root(parser)));started=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(5,start,nullptr);}
