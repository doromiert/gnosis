/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "ephy-engine-scheme.h"
#include "select/application.hpp"
#include "select/clipboard.hpp"
#include "select/extension-runtime.hpp"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace {
using Reply = std::function<void(CefRefPtr<CefDictionaryValue>, const std::string&)>;
class Protocol final : public CefDevToolsMessageObserver {
public:
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefRegistration> registration;
  std::map<int, Reply> pending;
  int sequence = 0x4000000;
  void send(const char* method, CefRefPtr<CefDictionaryValue> args, Reply done = {}) {
    if (!browser) { if (done) done(nullptr, "Browser is not ready"); return; }
    int id = ++sequence;
    if (done) pending[id] = std::move(done);
    browser->GetHost()->ExecuteDevToolsMethod(id, method, args);
  }
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool ok, const void* json, size_t size) override {
    auto it = pending.find(id);
    if (it == pending.end()) return;
    Reply done = std::move(it->second); pending.erase(it);
    auto value = CefParseJSON(static_cast<const char*>(json), size, JSON_PARSER_RFC);
    auto result = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
    struct Delivery { Reply done; CefRefPtr<CefDictionaryValue> result; bool ok; };
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
      auto* d = static_cast<Delivery*>(p);
      d->done(d->result, d->ok ? "" : "Chromium rejected the operation"); return G_SOURCE_REMOVE;
    }, new Delivery{std::move(done), result, ok}, +[](gpointer p) { delete static_cast<Delivery*>(p); });
  }
  void stop() {
    registration = nullptr; browser = nullptr;
    auto calls = std::move(pending);
    for (auto& [id, reply] : calls) reply(nullptr, "Tab was closed");
  }
private:
  IMPLEMENT_REFCOUNTING(Protocol);
};
struct ViewData;
Application* runtime = nullptr;
std::vector<ViewData*> views;
CefRefPtr<CefApp> cef_app;
guint pump = 0;
}

struct _EphyEngineHistoryItem { GObject parent; char* uri; char* title; int id; };
typedef struct _EphyEngineHistoryItemClass { GObjectClass parent; } EphyEngineHistoryItemClass;
G_DEFINE_TYPE(EphyEngineHistoryItem, ephy_engine_history_item, G_TYPE_OBJECT)
static void ephy_engine_history_item_class_init(EphyEngineHistoryItemClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    auto* item = reinterpret_cast<EphyEngineHistoryItem*>(object);
    g_free(item->uri); g_free(item->title);
    G_OBJECT_CLASS(ephy_engine_history_item_parent_class)->finalize(object);
  };
}
static void ephy_engine_history_item_init(EphyEngineHistoryItem*) {}
struct _EphyEngineHistory { GObject parent; ViewData* view; std::vector<EphyEngineHistoryItem*>* items; int current; };
typedef struct _EphyEngineHistoryClass { GObjectClass parent; } EphyEngineHistoryClass;
G_DEFINE_TYPE(EphyEngineHistory, ephy_engine_history, G_TYPE_OBJECT)
static void ephy_engine_history_class_init(EphyEngineHistoryClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    auto* h = reinterpret_cast<EphyEngineHistory*>(object);
    for (auto* item : *h->items) g_object_unref(item);
    delete h->items;
    G_OBJECT_CLASS(ephy_engine_history_parent_class)->finalize(object);
  };
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
    G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
}
static void ephy_engine_history_init(EphyEngineHistory* h) { h->items = new std::vector<EphyEngineHistoryItem*>; h->current = -1; }
struct _EphyEngineFind { GObject parent; ViewData* view; char* text; guint options; };
typedef struct _EphyEngineFindClass { GObjectClass parent; } EphyEngineFindClass;
G_DEFINE_TYPE(EphyEngineFind, ephy_engine_find, G_TYPE_OBJECT)
static void ephy_engine_find_class_init(EphyEngineFindClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    g_free(reinterpret_cast<EphyEngineFind*>(object)->text);
    G_OBJECT_CLASS(ephy_engine_find_parent_class)->finalize(object);
  };
  for (auto* name : {"found-text", "counted-matches"})
    g_signal_new(name, G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_UINT);
  g_signal_new("failed-to-find-text", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void ephy_engine_find_init(EphyEngineFind*) {}
struct _EphyEngineInspector { GObject parent; ViewData* view; };
typedef struct _EphyEngineInspectorClass { GObjectClass parent; } EphyEngineInspectorClass;
G_DEFINE_TYPE(EphyEngineInspector, ephy_engine_inspector, G_TYPE_OBJECT)
static void ephy_engine_inspector_class_init(EphyEngineInspectorClass* klass) {
  for (auto* name : {"attach", "closed"})
    g_signal_new(name, G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
}
static void ephy_engine_inspector_init(EphyEngineInspector*) {}
struct _EphyEngineResource { GObject parent; ViewData* view; };
typedef struct _EphyEngineResourceClass { GObjectClass parent; } EphyEngineResourceClass;
G_DEFINE_TYPE(EphyEngineResource, ephy_engine_resource, G_TYPE_OBJECT)
static void ephy_engine_resource_class_init(EphyEngineResourceClass*) {}
static void ephy_engine_resource_init(EphyEngineResource*) {}
struct _EphyEngineSessionState { gatomicrefcount refs; GBytes* bytes; };
namespace {
struct ViewData {
  EphyEngineView* self;
  Window host;
  CefRefPtr<Browser> browser;
  CefRefPtr<Protocol> protocol;
  EphyEngineHistory* history;
  EphyEngineFind* find;
  EphyEngineInspector* inspector;
  EphyEngineResource* resource;
  WebKitWebContext* context = nullptr;
  WebKitNetworkSession* session = nullptr;
  WebKitUserContentManager* ucm = nullptr;
  WebKitSettings* settings = nullptr;
  WebKitWindowProperties* window_properties = nullptr;
  WebKitEditorState* editor = nullptr;
  GdkTexture* favicon = nullptr;
  std::string uri, title, charset;
  bool loading = false, muted = false, disposed = false;
  double progress = 0, zoom = 1;
  explicit ViewData(EphyEngineView* view) : self(view), host(*runtime) {
    browser = new Browser("about:blank", g_getenv("JUPITER_SOFTWARE_RENDERING"));
    browser->host = &host;
    protocol = new Protocol;
    history = reinterpret_cast<EphyEngineHistory*>(g_object_new(ephy_engine_history_get_type(), nullptr)); history->view = this;
    find = reinterpret_cast<EphyEngineFind*>(g_object_new(ephy_engine_find_get_type(), nullptr)); find->view = this;
    inspector = reinterpret_cast<EphyEngineInspector*>(g_object_new(ephy_engine_inspector_get_type(), nullptr)); inspector->view = this;
    resource = reinterpret_cast<EphyEngineResource*>(g_object_new(ephy_engine_resource_get_type(), nullptr)); resource->view = this;
    window_properties = WEBKIT_WINDOW_PROPERTIES(g_object_new(WEBKIT_TYPE_WINDOW_PROPERTIES, nullptr));
    editor = WEBKIT_EDITOR_STATE(g_object_new(WEBKIT_TYPE_EDITOR_STATE, nullptr));
    ucm = webkit_user_content_manager_new();
    browser->on_changed = [this] { changed(); };
    browser->on_favicon = [this](const std::string& page, const std::string& icon, GdkTexture* texture) {
      if (disposed) return;
      if (texture && !host.private_mode)
        ephy_engine_favicon_database_store(ephy_engine_favicon_database_get_default(), page.c_str(), icon.c_str(), texture);
      if (page == browser->url && g_set_object(&favicon, texture))
        g_object_notify(G_OBJECT(self), "favicon");
    };
    browser->on_closed = [this] { gboolean handled = FALSE; g_signal_emit_by_name(self, "close", &handled); };
    browser->on_load_start = [this] { g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_STARTED); };
    browser->on_load_end = [this](int) {
      g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_COMMITTED);
      update_history();
      g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_FINISHED);
    };
    host.open_tab = [this](const std::string& url, bool active, bool defer) -> CefRefPtr<Browser> {
      GtkWidget* view = nullptr;
      g_signal_emit_by_name(self, "open-link", url.c_str(), active, defer, &view);
      if (!view || !EPHY_IS_ENGINE_VIEW(view)) return nullptr;
      return data(EPHY_ENGINE_VIEW(view))->browser;
    };
    gtk_widget_set_parent(browser->widget(), GTK_WIDGET(self));
    views.push_back(this);
  }
  static ViewData* data(EphyEngineView*);
  void changed() {
    if (disposed) return;
    if (browser->cef_browser() && !protocol->browser) {
      protocol->browser = browser->cef_browser();
      protocol->registration = protocol->browser->GetHost()->AddDevToolsMessageObserver(protocol);
    }
    if (uri != browser->url) { uri = browser->url; g_object_notify(G_OBJECT(self), "uri"); }
    if (title != browser->name) { title = browser->name; g_object_notify(G_OBJECT(self), "title"); }
    if (loading != browser->loading) { loading = browser->loading; g_object_notify(G_OBJECT(self), "is-loading"); }
    if (progress != browser->progress) { progress = browser->progress; g_object_notify(G_OBJECT(self), "estimated-load-progress"); }
    g_object_notify(G_OBJECT(self), "can-go-back"); g_object_notify(G_OBJECT(self), "can-go-forward");
  }
  void update_history() {
    protocol->send("Page.getNavigationHistory", nullptr, [this](auto reply, const auto& error) {
      if (!error.empty() || !reply || !reply->HasKey("entries")) return;
      for (auto* item : *history->items) g_object_unref(item);
      history->items->clear();
      history->current = reply->GetInt("currentIndex");
      auto entries = reply->GetList("entries");
      for (size_t i = 0; i < entries->GetSize(); i++) {
        auto entry = entries->GetDictionary(i);
        auto* item = reinterpret_cast<EphyEngineHistoryItem*>(g_object_new(ephy_engine_history_item_get_type(), nullptr));
        item->uri = g_strdup(entry->GetString("url").ToString().c_str());
        item->title = g_strdup(entry->GetString("title").ToString().c_str());
        item->id = entry->GetInt("id"); history->items->push_back(item);
      }
      g_signal_emit_by_name(history, "changed", nullptr, nullptr);
    });
  }
  ~ViewData() {
    std::erase(views, this);
    protocol->stop();
    for (GObject* object : {G_OBJECT(history), G_OBJECT(find), G_OBJECT(inspector), G_OBJECT(resource)}) g_object_unref(object);
    g_clear_object(&context); g_clear_object(&session); g_clear_object(&ucm); g_clear_object(&settings);
    g_clear_object(&favicon); g_clear_object(&window_properties); g_clear_object(&editor);
  }
};
}
typedef struct { ViewData* data; } EphyEngineViewPrivate;
G_DEFINE_TYPE_WITH_PRIVATE(EphyEngineView, ephy_engine_web_view, GTK_TYPE_WIDGET)
ViewData* ViewData::data(EphyEngineView* view) { return static_cast<EphyEngineViewPrivate*>(ephy_engine_web_view_get_instance_private(view))->data; }
namespace {
ViewData* data(EphyEngineView* view) { return ViewData::data(view); }
enum { P_NONE, P_TITLE, P_URI, P_LOADING, P_PROGRESS, P_ZOOM, P_MUTED, P_AUDIO, P_FAVICON, P_BACK, P_FORWARD,
 P_RESPONSIVE, P_CONTEXT, P_SESSION, P_UCM, P_SETTINGS, P_RELATED, P_AUTOMATION, P_EXTENSION_MODE, P_CSP };
void unsupported(GTask* task, const char* message) {
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s", message); g_object_unref(task);
}
void set_object(GObject** slot, const GValue* value) { g_set_object(slot, G_OBJECT(g_value_get_object(value))); }
}
static void ephy_engine_web_view_class_init(EphyEngineViewClass* klass) {
  auto* object_class = G_OBJECT_CLASS(klass); auto* widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->grab_focus = +[](GtkWidget* widget) -> gboolean { return gtk_widget_grab_focus(data(EPHY_ENGINE_VIEW(widget))->browser->surface()); };
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  object_class->dispose = +[](GObject* object) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    if (d && !d->disposed) {
      d->disposed = true; d->protocol->stop(); d->browser->detach_callbacks(); d->browser->close(); d->browser->host = nullptr;
      if (gtk_widget_get_parent(d->browser->widget())) gtk_widget_unparent(d->browser->widget());
    }
    G_OBJECT_CLASS(ephy_engine_web_view_parent_class)->dispose(object);
  };
  object_class->finalize = +[](GObject* object) { delete data(EPHY_ENGINE_VIEW(object)); G_OBJECT_CLASS(ephy_engine_web_view_parent_class)->finalize(object); };
  object_class->get_property = +[](GObject* object, guint id, GValue* value, GParamSpec* spec) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    switch(id) {
    case P_TITLE: g_value_set_string(value, d->title.c_str()); break;
    case P_URI: g_value_set_string(value, d->uri.c_str()); break;
    case P_LOADING: g_value_set_boolean(value, d->loading); break;
    case P_PROGRESS: g_value_set_double(value, d->progress); break;
    case P_ZOOM: g_value_set_double(value, d->zoom); break;
    case P_MUTED: g_value_set_boolean(value, d->muted); break;
    case P_AUDIO: g_value_set_boolean(value, false); break;
    case P_RESPONSIVE: g_value_set_boolean(value, true); break;
    case P_BACK: g_value_set_boolean(value, d->browser->can_back()); break;
    case P_FORWARD: g_value_set_boolean(value, d->browser->can_forward()); break;
    case P_FAVICON: g_value_set_object(value, d->favicon); break;
    case P_CONTEXT: g_value_set_object(value, d->context); break;
    case P_SESSION: g_value_set_object(value, d->session); break;
    case P_UCM: g_value_set_object(value, d->ucm); break;
    case P_SETTINGS: g_value_set_object(value, d->settings); break;
    case P_RELATED: g_value_set_object(value, nullptr); break;
    case P_AUTOMATION: g_value_set_boolean(value, false); break;
    case P_EXTENSION_MODE: g_value_set_enum(value, WEBKIT_WEB_EXTENSION_MODE_NONE); break;
    case P_CSP: g_value_set_string(value, nullptr); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
  };
  object_class->set_property = +[](GObject* object, guint id, const GValue* value, GParamSpec* spec) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    switch(id) {
    case P_CONTEXT: set_object(reinterpret_cast<GObject**>(&d->context), value); break;
    case P_SESSION:
      set_object(reinterpret_cast<GObject**>(&d->session), value);
      if (d->session && webkit_network_session_is_ephemeral(d->session)) {
        d->host.private_mode = true; CefRequestContextSettings settings;
        d->host.context = CefRequestContext::CreateContext(settings, nullptr);
      } break;
    case P_UCM: set_object(reinterpret_cast<GObject**>(&d->ucm), value); break;
    case P_SETTINGS: set_object(reinterpret_cast<GObject**>(&d->settings), value); break;
    case P_RELATED: {
      auto* related = EPHY_ENGINE_VIEW(g_value_get_object(value));
      if (related) { d->host.context = data(related)->host.context; d->host.private_mode = data(related)->host.private_mode; }
      break;
    }
    case P_AUTOMATION: case P_EXTENSION_MODE: case P_CSP: break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
  };
  auto flags = GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  auto construct = GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  for (auto [id, name] : {std::pair{P_TITLE,"title"}, {P_URI,"uri"}})
    g_object_class_install_property(object_class, id, g_param_spec_string(name, nullptr, nullptr, "", flags));
  for (auto [id, name] : {std::pair{P_LOADING,"is-loading"}, {P_MUTED,"is-muted"}, {P_AUDIO,"is-playing-audio"}, {P_BACK,"can-go-back"}, {P_FORWARD,"can-go-forward"}, {P_RESPONSIVE,"is-web-process-responsive"}})
    g_object_class_install_property(object_class, id, g_param_spec_boolean(name, nullptr, nullptr, id==P_RESPONSIVE, flags));
  g_object_class_install_property(object_class, P_PROGRESS, g_param_spec_double("estimated-load-progress", nullptr, nullptr, 0, 1, 0, flags));
  g_object_class_install_property(object_class, P_ZOOM, g_param_spec_double("zoom-level", nullptr, nullptr, .1, 10, 1, flags));
  g_object_class_install_property(object_class, P_FAVICON, g_param_spec_object("favicon", nullptr, nullptr, GDK_TYPE_TEXTURE, flags));
  for (auto [id, name, type] : {std::tuple{P_CONTEXT,"web-context",WEBKIT_TYPE_WEB_CONTEXT}, {P_SESSION,"network-session",WEBKIT_TYPE_NETWORK_SESSION}, {P_UCM,"user-content-manager",WEBKIT_TYPE_USER_CONTENT_MANAGER}, {P_SETTINGS,"settings",WEBKIT_TYPE_SETTINGS}, {P_RELATED,"related-view",EPHY_TYPE_ENGINE_VIEW}})
    g_object_class_install_property(object_class, id, g_param_spec_object(name, nullptr, nullptr, type, construct));
  g_object_class_install_property(object_class, P_AUTOMATION, g_param_spec_boolean("is-controlled-by-automation", nullptr, nullptr, false, construct));
  g_object_class_install_property(object_class, P_EXTENSION_MODE, g_param_spec_enum("web-extension-mode", nullptr, nullptr, WEBKIT_TYPE_WEB_EXTENSION_MODE, WEBKIT_WEB_EXTENSION_MODE_NONE, construct));
  g_object_class_install_property(object_class, P_CSP, g_param_spec_string("default-content-security-policy", nullptr, nullptr, nullptr, construct));
  g_signal_new("open-link", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, GTK_TYPE_WIDGET, 3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  g_signal_new("load-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_changed), nullptr, nullptr, nullptr, G_TYPE_NONE, 1, WEBKIT_TYPE_LOAD_EVENT);
  g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_failed), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 3, WEBKIT_TYPE_LOAD_EVENT, G_TYPE_STRING, G_TYPE_ERROR);
  g_signal_new("create", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, create), nullptr, nullptr, nullptr, GTK_TYPE_WIDGET, 1, WEBKIT_TYPE_NAVIGATION_ACTION);
  g_signal_new("ready-to-show", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, ready_to_show), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("run-as-modal", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, run_as_modal), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("close", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, close), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("script-dialog", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, script_dialog), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_SCRIPT_DIALOG);
  g_signal_new("decide-policy", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, decide_policy), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 2, WEBKIT_TYPE_POLICY_DECISION, WEBKIT_TYPE_POLICY_DECISION_TYPE);
  g_signal_new("permission-request", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, permission_request), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_PERMISSION_REQUEST);
  g_signal_new("mouse-target-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, mouse_target_changed), nullptr, nullptr, nullptr, G_TYPE_NONE, 2, WEBKIT_TYPE_HIT_TEST_RESULT, G_TYPE_UINT);
  g_signal_new("print", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, print), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_PRINT_OPERATION);
  g_signal_new("resource-load-started", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, resource_load_started), nullptr, nullptr, nullptr, G_TYPE_NONE, 2, ephy_engine_resource_get_type(), WEBKIT_TYPE_URI_REQUEST);
  g_signal_new("enter-fullscreen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, enter_fullscreen), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
  g_signal_new("leave-fullscreen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, leave_fullscreen), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
  g_signal_new("run-file-chooser", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, run_file_chooser), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_FILE_CHOOSER_REQUEST);
  g_signal_new("context-menu", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, context_menu), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 2, WEBKIT_TYPE_CONTEXT_MENU, WEBKIT_TYPE_HIT_TEST_RESULT);
  g_signal_new("context-menu-dismissed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, context_menu_dismissed), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("submit-form", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, submit_form), nullptr, nullptr, nullptr, G_TYPE_NONE, 1, WEBKIT_TYPE_FORM_SUBMISSION_REQUEST);
  g_signal_new("insecure-content-detected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, insecure_content_detected), nullptr, nullptr, nullptr, G_TYPE_NONE, 1, WEBKIT_TYPE_INSECURE_CONTENT_EVENT);
  g_signal_new("web-process-crashed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, web_process_crashed), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
  g_signal_new("authenticate", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, authenticate), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_AUTHENTICATION_REQUEST);
  g_signal_new("load-failed-with-tls-errors", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_failed_with_tls_errors), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 3, G_TYPE_STRING, G_TYPE_TLS_CERTIFICATE, G_TYPE_TLS_CERTIFICATE_FLAGS);
  g_signal_new("show-notification", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, show_notification), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_NOTIFICATION);
  g_signal_new("run-color-chooser", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, run_color_chooser), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_COLOR_CHOOSER_REQUEST);
  g_signal_new("show-option-menu", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, show_option_menu), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 2, WEBKIT_TYPE_OPTION_MENU, GDK_TYPE_RECTANGLE);
  g_signal_new("web-process-terminated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, web_process_terminated), nullptr, nullptr, nullptr, G_TYPE_NONE, 1, WEBKIT_TYPE_WEB_PROCESS_TERMINATION_REASON);
  g_signal_new("user-message-received", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, user_message_received), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_USER_MESSAGE);
  g_signal_new("query-permission-state", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, query_permission_state), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, WEBKIT_TYPE_PERMISSION_STATE_QUERY);
}
static void ephy_engine_web_view_init(EphyEngineView* view) {
  static_cast<EphyEngineViewPrivate*>(ephy_engine_web_view_get_instance_private(view))->data = new ViewData(view);
}

namespace {
class Engine final : public CefApp, public CefBrowserProcessHandler {
public:
  std::string extension_paths;
  bool diagnostic = false;
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return Clipboard::renderer_handler(); }
  void OnBeforeCommandLineProcessing(const CefString& process, CefRefPtr<CefCommandLine> command) override {
    if (!process.empty()) return;
    configure_extensions(command, extension_paths);
    if (diagnostic) command->AppendSwitchWithValue("password-store", "basic");
    command->AppendSwitchWithValue("ozone-platform", g_getenv("WAYLAND_DISPLAY") ? "wayland" : "x11");
    command->AppendSwitchWithValue("use-angle", "gl-egl");
  }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { ephy_engine_register_schemes(registrar); }
  void OnContextInitialized() override { enable_extension_developer_mode(); ephy_engine_install_scheme_handlers(); }
private:
  IMPLEMENT_REFCOUNTING(Engine);
};
CefRefPtr<Engine> engine;
}
int ephy_engine_execute_process(int argc, char** argv) {
  engine = new Engine;
  return CefExecuteProcess(CefMainArgs(argc, argv), engine, nullptr);
}
gboolean ephy_engine_initialize(int argc, char** argv, const char* profile, gboolean self_test) {
  try { runtime = new Application(profile); }
  catch (const std::exception& error) { g_printerr("Extensions: %s\n", error.what()); return false; }
  runtime->gtk_app = g_application_get_default();
  engine->extension_paths = runtime->extensions.load_paths();
  for (const auto& ext : runtime->extensions.installed) if (ext.enabled) {
    runtime->extension_host->extension_ids.insert(ext.id);
    runtime->extension_host->permissions[ext.id] = ext.permissions;
    runtime->extension_host->extension_paths[ext.id] = ext.path;
  }
  runtime->extension_host->tab_bridge_id = Extensions::id_for_path(JUPITER_BRIDGE_DIR);
  runtime->extension_host->tabs = [] {
    std::vector<ExtensionTab> tabs;
    int index = 0;
    for (auto* view : views) {
      auto b = view->browser;
      if (view->disposed || view->host.private_mode || !b->cef_browser() || b->native_tab_id < 0) continue;
      auto info = CefDictionaryValue::Create();
      info->SetInt("id", b->native_tab_id); info->SetInt("index", index++);
      info->SetString("url", b->url); info->SetString("title", b->name);
      info->SetBool("active", gtk_widget_get_mapped(GTK_WIDGET(view->self)));
      info->SetBool("incognito", false);
      tabs.push_back({b->native_tab_id, b->cef_browser(), info});
    }
    return tabs;
  };
  runtime->extension_host->update = [](int id, const std::string& url, bool activate) {
    for (auto* view : views) if (view->browser->native_tab_id == id && !view->host.private_mode) {
      if (!url.empty()) ephy_engine_web_view_load_uri(view->self, url.c_str());
      if (activate) {
        auto* widget = GTK_WIDGET(view->self);
        for (auto* parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent)) {
          if (!ADW_IS_TAB_VIEW(parent)) continue;
          for (int i=0;i<adw_tab_view_get_n_pages(ADW_TAB_VIEW(parent));i++) {
            auto* page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(parent), i);
            if (gtk_widget_is_ancestor(widget, adw_tab_page_get_child(page))) adw_tab_view_set_selected_page(ADW_TAB_VIEW(parent), page);
          }
          break;
        }
      }
      break;
    }
  };
  CefSettings settings;
  settings.external_message_pump = true; settings.windowless_rendering_enabled = true;
  settings.no_sandbox = self_test;
  engine->diagnostic = self_test;
  settings.persist_session_cookies = true;
  std::string path = std::string(profile) + "/chromium";
  g_mkdir_with_parents(path.c_str(), 0700);
  CefString(&settings.root_cache_path) = path;
  CefString(&settings.cache_path) = path + "/profile";
  CefString(&settings.resources_dir_path) = CEF_RESOURCE_DIR;
  CefString(&settings.locales_dir_path) = std::string(CEF_RESOURCE_DIR) + "/locales";
  CefString(&settings.log_file) = std::string(profile) + "/chromium.log";
  settings.log_severity = LOGSEVERITY_WARNING;
  if (!CefInitialize(CefMainArgs(argc, argv), settings, engine, nullptr)) return false;
  pump = g_timeout_add(8, +[](gpointer) -> gboolean { CefDoMessageLoopWork(); return G_SOURCE_CONTINUE; }, nullptr);
  return true;
}
void ephy_engine_shutdown() {
  if (pump) { g_source_remove(pump); pump = 0; }
  if (runtime) { runtime->extension_host->stop(); delete runtime; runtime = nullptr; CefShutdown(); }
  ephy_engine_clear_scheme_handlers();
  engine = nullptr;
}

EphyEngineHistoryItem* ephy_engine_back_forward_list_get_current_item(EphyEngineHistory* h) {
  return h->current >= 0 && h->current < int(h->items->size()) ? h->items->at(h->current) : nullptr;
}
EphyEngineHistoryItem* ephy_engine_back_forward_list_get_back_item(EphyEngineHistory* h) {
  return h->current > 0 ? h->items->at(h->current-1) : nullptr;
}
EphyEngineHistoryItem* ephy_engine_back_forward_list_get_forward_item(EphyEngineHistory* h) {
  return h->current+1 < int(h->items->size()) ? h->items->at(h->current+1) : nullptr;
}
GList* ephy_engine_back_forward_list_get_back_list_with_limit(EphyEngineHistory* h, guint limit) {
  GList* list = nullptr;
  for (int i=h->current-1; i>=0 && limit--; i--) list = g_list_append(list, h->items->at(i));
  return list;
}
GList* ephy_engine_back_forward_list_get_forward_list_with_limit(EphyEngineHistory* h, guint limit) {
  GList* list = nullptr;
  for (int i=h->current+1; i<int(h->items->size()) && limit--; i++) list = g_list_append(list, h->items->at(i));
  return list;
}
const gchar* ephy_engine_back_forward_list_item_get_uri(EphyEngineHistoryItem* i) { return i->uri; }
const gchar* ephy_engine_back_forward_list_item_get_original_uri(EphyEngineHistoryItem* i) { return i->uri; }
const gchar* ephy_engine_back_forward_list_item_get_title(EphyEngineHistoryItem* i) { return i->title; }
gboolean ephy_engine_web_view_is_controlled_by_automation(EphyEngineView*) { return false; }
WebKitWebContext* ephy_engine_web_view_get_context(EphyEngineView* v) { return data(v)->context; }
void ephy_engine_web_view_try_close(EphyEngineView* v) { data(v)->browser->close(); }
void ephy_engine_web_view_load_uri(EphyEngineView* v, const gchar* uri) { data(v)->browser->navigate(uri ? uri : "about:blank"); }
void ephy_engine_web_view_load_alternate_html(EphyEngineView* v, const gchar* content, const gchar*, const gchar*) {
  char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(content), strlen(content));
  std::string url = "data:text/html;charset=utf-8;base64," + std::string(encoded); g_free(encoded);
  data(v)->browser->navigate(url);
}
void ephy_engine_web_view_load_request(EphyEngineView* v, WebKitURIRequest* r) { ephy_engine_web_view_load_uri(v, webkit_uri_request_get_uri(r)); }
void ephy_engine_web_view_stop_loading(EphyEngineView* v) { if (auto b=data(v)->browser->cef_browser()) b->StopLoad(); }
gboolean ephy_engine_web_view_is_loading(EphyEngineView* v) { return data(v)->loading; }
gboolean ephy_engine_web_view_is_playing_audio(EphyEngineView* v) { auto b=data(v)->browser->cef_browser(); return b && b->GetHost()->IsAudioMuted()==false && false; }
void ephy_engine_web_view_set_is_muted(EphyEngineView* v, gboolean value) { data(v)->muted=value; if(auto b=data(v)->browser->cef_browser()) b->GetHost()->SetAudioMuted(value); g_object_notify(G_OBJECT(v), "is-muted"); }
gboolean ephy_engine_web_view_get_is_muted(EphyEngineView* v) { return data(v)->muted; }
guint64 ephy_engine_web_view_get_page_id(EphyEngineView* v) { return reinterpret_cast<guintptr>(v); }
const gchar* ephy_engine_web_view_get_title(EphyEngineView* v) { return data(v)->title.c_str(); }
void ephy_engine_web_view_reload(EphyEngineView* v) { data(v)->browser->reload(); }
void ephy_engine_web_view_reload_bypass_cache(EphyEngineView* v) { if(auto b=data(v)->browser->cef_browser()) b->ReloadIgnoreCache(); else data(v)->browser->reload(); }
gdouble ephy_engine_web_view_get_estimated_load_progress(EphyEngineView* v) { return data(v)->progress; }
void ephy_engine_web_view_go_back(EphyEngineView* v) { data(v)->browser->back(); }
void ephy_engine_web_view_go_forward(EphyEngineView* v) { data(v)->browser->forward(); }
gboolean ephy_engine_web_view_can_go_back(EphyEngineView* v) { return data(v)->browser->can_back(); }
gboolean ephy_engine_web_view_can_go_forward(EphyEngineView* v) { return data(v)->browser->can_forward(); }
EphyEngineHistory* ephy_engine_web_view_get_back_forward_list(EphyEngineView* v) { return data(v)->history; }
void ephy_engine_web_view_go_to_back_forward_list_item(EphyEngineView* v, EphyEngineHistoryItem* item) {
  auto args=CefDictionaryValue::Create(); args->SetInt("entryId", item->id);
  data(v)->protocol->send("Page.navigateToHistoryEntry", args);
}
const gchar* ephy_engine_web_view_get_uri(EphyEngineView* v) { return data(v)->uri.empty() ? nullptr : data(v)->uri.c_str(); }
GdkTexture* ephy_engine_web_view_get_favicon(EphyEngineView* v) { return data(v)->favicon; }
const gchar* ephy_engine_web_view_get_custom_charset(EphyEngineView* v) { return data(v)->charset.empty()?nullptr:data(v)->charset.c_str(); }
void ephy_engine_web_view_set_custom_charset(EphyEngineView* v, const gchar* charset) {
  auto* d=data(v); d->charset=charset?charset:"";
  g_warning("Chromium does not expose a per-tab encoding override");
}
WebKitWindowProperties* ephy_engine_web_view_get_window_properties(EphyEngineView* v) { return data(v)->window_properties; }
void ephy_engine_web_view_set_zoom_level(EphyEngineView* v, gdouble zoom) {
  auto* d=data(v); d->zoom=std::clamp(zoom,.1,10.);
  if(auto b=d->browser->cef_browser()) b->GetHost()->SetZoomLevel(std::log(d->zoom)/std::log(1.2));
  g_object_notify(G_OBJECT(v),"zoom-level");
}
gdouble ephy_engine_web_view_get_zoom_level(EphyEngineView* v) { return data(v)->zoom; }
void ephy_engine_web_view_execute_editing_command(EphyEngineView* v, const gchar* command) {
  auto* d=data(v); auto b=d->browser->cef_browser(); if(!b) return;
  auto frame=b->GetFocusedFrame(); if(!frame)frame=b->GetMainFrame();
  if(!g_ascii_strcasecmp(command,"Copy"))Clipboard::copy(*d->browser,false);
  else if(!g_ascii_strcasecmp(command,"Cut"))Clipboard::copy(*d->browser,true);
  else if(!g_ascii_strcasecmp(command,"Paste"))Clipboard::paste(*d->browser,false);
  else if(!g_ascii_strcasecmp(command,"PasteAsPlainText"))Clipboard::paste(*d->browser,true);
  else if(!g_ascii_strcasecmp(command,"SelectAll"))frame->SelectAll();
  else if(!g_ascii_strcasecmp(command,"Undo"))frame->Undo();
  else if(!g_ascii_strcasecmp(command,"Redo"))frame->Redo();
}
EphyEngineFind* ephy_engine_web_view_get_find_controller(EphyEngineView* v) { return data(v)->find; }
void ephy_engine_find_controller_search(EphyEngineFind* f,const gchar* text,guint32 options,guint) {
  g_free(f->text); f->text=g_strdup(text); f->options=options;
  if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(text,!(options&WEBKIT_FIND_OPTIONS_BACKWARDS),!(options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),false);
}
void ephy_engine_find_controller_search_finish(EphyEngineFind* f) { if(auto b=f->view->browser->cef_browser())b->GetHost()->StopFinding(true); }
void ephy_engine_find_controller_search_next(EphyEngineFind* f) { if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(f->text?f->text:"",true,!(f->options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),true); }
void ephy_engine_find_controller_search_previous(EphyEngineFind* f) { if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(f->text?f->text:"",false,!(f->options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),true); }
void ephy_engine_find_controller_count_matches(EphyEngineFind* f,const gchar* text,guint32 options,guint limit) { ephy_engine_find_controller_search(f,text,options,limit); }
guint32 ephy_engine_find_controller_get_options(EphyEngineFind* f) { return f->options; }
void ephy_engine_web_inspector_show(EphyEngineInspector* i) {
  if(auto b=i->view->browser->cef_browser()) { CefWindowInfo info; CefBrowserSettings settings; b->GetHost()->ShowDevTools(info,nullptr,settings,CefPoint()); }
}
void ephy_engine_web_inspector_close(EphyEngineInspector* i) { if(auto b=i->view->browser->cef_browser())b->GetHost()->CloseDevTools(); }
void ephy_engine_web_view_evaluate_javascript(EphyEngineView* v,const char* script,gssize length,const char* world,const char*,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(v,cancel,cb,user);
  auto* d=data(v);
  auto args=CefDictionaryValue::Create();
  args->SetString("expression",std::string(script,length<0?strlen(script):size_t(length)));
  args->SetBool("returnByValue",true); args->SetBool("awaitPromise",true);
  // Epiphany's named worlds must never be silently evaluated in a site's main world.
  if(world && *world) { unsupported(task,"The named JavaScript world has not been initialized");return; }
  d->protocol->send("Runtime.evaluate",args,[task](auto reply,const auto& error) {
    if(!error.empty()||!reply||reply->HasKey("exceptionDetails")) {
      g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"%s",error.empty()?"JavaScript evaluation failed":error.c_str());
    } else {
      auto result=reply->GetDictionary("result"); auto* context=jsc_context_new(); JSCValue* value=nullptr;
      if(result && result->HasKey("value")) {
        auto json=CefWriteJSON(result->GetValue("value"),JSON_WRITER_DEFAULT).ToString();
        value=jsc_value_new_from_json(context,json.c_str());
      }
      if(!value)value=jsc_value_new_undefined(context);
      g_task_return_pointer(task,value,g_object_unref); g_object_unref(context);
    }
    g_object_unref(task);
  });
}
JSCValue* ephy_engine_web_view_evaluate_javascript_finish(EphyEngineView*,GAsyncResult* r,GError** e) { return JSC_VALUE(g_task_propagate_pointer(G_TASK(r),e)); }
EphyEngineResource* ephy_engine_web_view_get_main_resource(EphyEngineView* v) { return data(v)->resource; }
EphyEngineInspector* ephy_engine_web_view_get_inspector(EphyEngineView* v) { return data(v)->inspector; }
const gchar* ephy_engine_web_resource_get_uri(EphyEngineResource* r) { return r->view->uri.c_str(); }
WebKitURIResponse* ephy_engine_web_resource_get_response(EphyEngineResource*) { return nullptr; }
namespace {
class Source final : public CefStringVisitor {
public:
  explicit Source(GTask* t):task(t){}
  void Visit(const CefString& string) override {
    auto text=string.ToString(); auto* bytes=g_bytes_new(text.data(),text.size());
    g_task_return_pointer(task,bytes,reinterpret_cast<GDestroyNotify>(g_bytes_unref));g_object_unref(task);task=nullptr;
  }
private:
  GTask* task;
  IMPLEMENT_REFCOUNTING(Source);
};
}
void ephy_engine_web_resource_get_data(EphyEngineResource* r,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(r,cancel,cb,user);
  if(auto b=r->view->browser->cef_browser())b->GetMainFrame()->GetSource(new Source(task));
  else unsupported(task,"Browser is not ready");
}
guchar* ephy_engine_web_resource_get_data_finish(EphyEngineResource*,GAsyncResult* result,gsize* length,GError** error) {
  auto* bytes=static_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result),error));if(!bytes)return nullptr;
  auto* copy=static_cast<guchar*>(g_bytes_unref_to_data(bytes,length));return copy;
}
void ephy_engine_web_view_save_to_file(EphyEngineView* v,GFile* file,WebKitSaveMode,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(v,cancel,cb,user); g_object_ref(file);
  auto args=CefDictionaryValue::Create(); args->SetString("format","mhtml");
  data(v)->protocol->send("Page.captureSnapshot",args,[task,file](auto reply,const auto& error) {
    if(!error.empty()||!reply)g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"%s",error.c_str());
    else {
      auto text=reply->GetString("data").ToString(); GError* failure=nullptr;
      if(g_file_replace_contents(file,text.data(),text.size(),nullptr,false,G_FILE_CREATE_PRIVATE,nullptr,g_task_get_cancellable(task),&failure))g_task_return_boolean(task,true);
      else g_task_return_error(task,failure);
    }
    g_object_unref(file);g_object_unref(task);
  });
}
gboolean ephy_engine_web_view_get_tls_info(EphyEngineView* v,GTlsCertificate** certificate,GTlsCertificateFlags* errors) {
  if(certificate)*certificate=nullptr;if(errors)*errors=GTlsCertificateFlags(0);
  auto b=data(v)->browser->cef_browser();if(!b)return false;
  auto entry=b->GetHost()->GetVisibleNavigationEntry();auto ssl=entry?entry->GetSSLStatus():nullptr;
  auto cert=ssl?ssl->GetX509Certificate():nullptr;if(!cert)return false;
  if(errors && ssl->GetCertStatus())*errors=G_TLS_CERTIFICATE_GENERIC_ERROR;
  auto pem=cert->GetPEMEncoded(); if(certificate && pem) {
    std::string text(pem->GetSize(),'\0');pem->GetData(text.data(),text.size(),0);
    *certificate=g_tls_certificate_new_from_pem(text.c_str(),text.size(),nullptr);
  }
  return true;
}
void ephy_engine_web_view_get_snapshot(EphyEngineView* v,WebKitSnapshotRegion,WebKitSnapshotOptions,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(v,cancel,cb,user);auto* d=data(v);auto* image=d->browser->thumbnail();
  auto* native=gtk_widget_get_native(GTK_WIDGET(v));auto* renderer=native?gtk_native_get_renderer(native):nullptr;
  if(!image||!renderer) { if(image)g_object_unref(image);unsupported(task,"The tab has no rendered frame");return; }
  auto* snapshot=gtk_snapshot_new();float w=gdk_paintable_get_intrinsic_width(image),h=gdk_paintable_get_intrinsic_height(image);
  gdk_paintable_snapshot(image,GDK_SNAPSHOT(snapshot),w,h);g_object_unref(image);
  auto* node=gtk_snapshot_free_to_node(snapshot);if(!node){unsupported(task,"The tab has no rendered frame");return;}
  graphene_rect_t bounds=GRAPHENE_RECT_INIT(0,0,w,h);auto* texture=gsk_renderer_render_texture(renderer,node,&bounds);gsk_render_node_unref(node);
  g_task_return_pointer(task,texture,g_object_unref);g_object_unref(task);
}
GdkTexture* ephy_engine_web_view_get_snapshot_finish(EphyEngineView*,GAsyncResult* r,GError** e) { return GDK_TEXTURE(g_task_propagate_pointer(G_TASK(r),e)); }
WebKitUserContentManager* ephy_engine_web_view_get_user_content_manager(EphyEngineView* v) { return data(v)->ucm; }
WebKitEditorState* ephy_engine_web_view_get_editor_state(EphyEngineView* v) { return data(v)->editor; }
EphyEngineSessionState* ephy_engine_web_view_get_session_state(EphyEngineView* v) {
  auto* d=data(v);auto* value=g_variant_ref_sink(g_variant_new("(ss)",d->uri.c_str(),d->title.c_str()));auto* bytes=g_variant_get_data_as_bytes(value);
  auto* state=ephy_engine_web_view_session_state_new(bytes);g_bytes_unref(bytes);g_variant_unref(value);return state;
}
void ephy_engine_web_view_restore_session_state(EphyEngineView* v,EphyEngineSessionState* state) {
  auto* value=g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("(ss)"),state->bytes,false));
  if(g_variant_is_normal_form(value)) { const char *uri,*title;g_variant_get(value,"(&s&s)",&uri,&title);data(v)->browser->url=uri;data(v)->browser->name=title;data(v)->changed(); }
  g_variant_unref(value);
}
EphyEngineSessionState* ephy_engine_web_view_session_state_new(GBytes* bytes) { auto* state=new EphyEngineSessionState;g_atomic_ref_count_init(&state->refs);state->bytes=g_bytes_ref(bytes);return state; }
EphyEngineSessionState* ephy_engine_web_view_session_state_ref(EphyEngineSessionState* s) { g_atomic_ref_count_inc(&s->refs);return s; }
void ephy_engine_web_view_session_state_unref(EphyEngineSessionState* s) { if(g_atomic_ref_count_dec(&s->refs)){g_bytes_unref(s->bytes);delete s;} }
GBytes* ephy_engine_web_view_session_state_serialize(EphyEngineSessionState* s) { return g_bytes_ref(s->bytes); }
void ephy_engine_web_view_send_message_to_page(EphyEngineView* v,WebKitUserMessage*,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) { unsupported(g_task_new(v,cancel,cb,user),"WebKit web-process messages are unavailable on Chromium"); }
WebKitUserMessage* ephy_engine_web_view_send_message_to_page_finish(EphyEngineView*,GAsyncResult* r,GError** e) { return WEBKIT_USER_MESSAGE(g_task_propagate_pointer(G_TASK(r),e)); }
void ephy_engine_web_view_set_cors_allowlist(EphyEngineView*,const gchar* const*) { /* Internal schemes enforce their own access policy. */ }
gboolean ephy_engine_web_view_get_is_web_process_responsive(EphyEngineView* v) { return !data(v)->disposed; }
void ephy_engine_web_view_terminate_web_process(EphyEngineView* v) { data(v)->protocol->send("Page.crash",nullptr); }

EphyEngineView* ephy_engine_view_for_browser(int id) {
  for(auto* view:views) if(auto b=view->browser->cef_browser();b&&b->GetIdentifier()==id&&!view->disposed)return view->self;
  return nullptr;
}

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView* view) {return data(view)->browser->cef_browser();}
