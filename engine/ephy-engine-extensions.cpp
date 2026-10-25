/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-extensions.h"
#include "select/devtools.hpp"
#include "select/extensions.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
std::string quote(const std::string &text) {
  auto value = CefValue::Create();
  value->SetString(text);
  return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

struct Request {
  GTask *task = nullptr;
  std::string id;
  bool enabled = false;
  bool mutate = false;
  guint timeout = 0;
  gulong cancelled = 0;
  bool complete = false;
  guint64 context_generation = 0;
  ~Request() {
    if (timeout) g_source_remove(timeout);
    if (cancelled) g_cancellable_disconnect(g_task_get_cancellable(task), cancelled);
    g_object_unref(task);
  }
};

class ManagementService;
extern std::unique_ptr<ManagementService> service;
struct CancelDispatch {
  ManagementService *service;
  std::weak_ptr<Request> request;
};
struct Installation {
  std::weak_ptr<char> owner;
  std::string source, directory;
  GCancellable *cancellable = g_cancellable_new();
  GCancellable *caller = nullptr;
  gulong cancelled = 0;
  ~Installation() {
    if (cancelled) g_cancellable_disconnect(caller, cancelled);
    g_clear_object(&caller);
    g_clear_object(&cancellable);
  }
};
class ManagementClient final : public CefClient,
                               public CefLifeSpanHandler,
                               public CefLoadHandler,
                               public CefRenderHandler,
                               public CefDevToolsMessageObserver {
public:
  explicit ManagementClient(ManagementService *service) : service_(service) {}
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override { rect = CefRect(0, 0, 1, 1); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &, const void *, int, int) override {}
  void OnAfterCreated(CefRefPtr<CefBrowser>) override;
  void OnBeforeClose(CefRefPtr<CefBrowser>) override;
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int) override;
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int, bool, const void *, size_t) override;
  void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString&, const void *, size_t) override;
private:
  ManagementService *service_;
  IMPLEMENT_REFCOUNTING(ManagementClient);
};

class ManagementService {
public:
  JsonArray *catalog = nullptr;
  JsonArray *last_actual = nullptr;
  Extensions *store = nullptr;
  std::shared_ptr<char> lifetime = std::make_shared<char>();
  std::vector<std::weak_ptr<Installation>> installations;
  CefRefPtr<CefRequestContext> context;
  CefRefPtr<ManagementClient> client;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefRegistration> registration;
  std::function<void(JsonArray *)> changed;
  std::string helper_uri, helper_origin, root_frame, context_unique;
  struct Context { std::string unique, frame, origin; };
  std::vector<Context> contexts;
  int frame_tree_request = 0;
  guint dispatch_idle = 0;
  guint64 context_generation = 0;
  bool page_loaded = false, starting = false, closing = false, closed = true;
  std::unordered_map<int, std::shared_ptr<Request>> inflight;
  std::vector<std::shared_ptr<Request>> waiting;

  ~ManagementService() {
    fail_all(G_IO_ERROR_CLOSED, "The extension manager is closed");
    if (dispatch_idle) g_source_remove(dispatch_idle);
    if (catalog) json_array_unref(catalog);
    if (last_actual) json_array_unref(last_actual);
  }

  bool usable(EphyEngineSession *session, GError **error) const {
    if (!EPHY_IS_ENGINE_SESSION(session) || ephy_engine_session_is_ephemeral(session)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "Extensions are unavailable in private windows");
      return false;
    }
    auto requested = ephy_engine_session_context(session);
    if (!context || !requested || !context->IsSame(requested)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          "The extension request is not in Jupiter's normal profile");
      return false;
    }
    if (closed || closing) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "The extension manager is closed");
      return false;
    }
    return true;
  }

  bool owned(const std::string &id) const {
    if (id.empty() || !catalog) return false;
    if (helper_origin == "chrome-extension://" + id || id == Extensions::id_for_path(JUPITER_BRIDGE_DIR)) return false;
    for (guint i = 0; i < json_array_get_length(catalog); ++i) {
      auto *item = json_array_get_object_element(catalog, i);
      if (item && id == json_object_get_string_member_with_default(item, "id", "")) return true;
    }
    return false;
  }

  void start() {
    if (closed || closing || browser || starting) return;
    starting = true;
    CefWindowInfo info; info.SetAsWindowless(0);
    CefBrowserSettings settings;
    if (!context || helper_uri.empty() || !CefBrowserHost::CreateBrowser(info, client, helper_uri, settings, nullptr, context)) {
      starting = false;
      closed = true;
      fail_all(G_IO_ERROR_FAILED, "Could not start Chromium's extension manager");
    }
  }

  void connected(CefRefPtr<CefBrowser> created) {
    starting = false;
    if (closing || closed || browser) { created->GetHost()->CloseBrowser(true); return; }
    browser = created;
    registration = browser->GetHost()->AddDevToolsMessageObserver(client);
    auto params = CefDictionaryValue::Create();
    send_devtools_method(browser, next_devtools_message_id(), "Runtime.enable", params);
    frame_tree_request = next_devtools_message_id();
    send_devtools_method(browser, frame_tree_request, "Page.getFrameTree", nullptr);
  }

  void loaded(CefRefPtr<CefBrowser> source) {
    if (!browser || !browser->IsSame(source) || !browser->GetMainFrame() ||
        browser->GetMainFrame()->GetURL().ToString() != helper_uri) return;
    page_loaded = true;
    schedule_dispatch();
  }

  void context_created(CefRefPtr<CefBrowser> source, const void *json, size_t size) {
    if (!browser || !browser->IsSame(source)) return;
    auto value = CefParseJSON(static_cast<const char *>(json), size, JSON_PARSER_RFC);
    auto root = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
    auto info = root ? root->GetDictionary("context") : nullptr;
    auto aux = info ? info->GetDictionary("auxData") : nullptr;
    if (!info || !aux || !aux->GetBool("isDefault")) return;
    Context candidate{info->GetString("uniqueId").ToString(), aux->GetString("frameId").ToString(),
                      info->GetString("origin").ToString()};
    if (candidate.unique.empty() || candidate.origin != helper_origin) return;
    contexts.push_back(std::move(candidate));
    select_context();
  }

  void context_lost(CefRefPtr<CefBrowser> source, const void *json, size_t size, bool all = false) {
    if (!browser || !browser->IsSame(source)) return;
    auto value = !all && json ? CefParseJSON(static_cast<const char *>(json), size, JSON_PARSER_RFC) : nullptr;
    auto root = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
    const auto unique = root ? root->GetString("executionContextUniqueId").ToString() : "";
    if (all) contexts.clear();
    else std::erase_if(contexts, [&](const auto &context) { return context.unique == unique; });
    if ((!all && (unique.empty() || unique != context_unique)) || context_unique.empty()) return;
    context_unique.clear();
    ++context_generation;
    auto sent = std::move(inflight); inflight.clear();
    for (auto &[id, request] : sent)
      fail(request, G_IO_ERROR_CLOSED, "The extension manager execution context changed");
  }

  void select_context() {
    if (root_frame.empty()) return;
    auto found = std::find_if(contexts.begin(), contexts.end(), [&](const auto &context) {
      return context.frame == root_frame && context.origin == helper_origin;
    });
    if (found == contexts.end()) return;
    if (context_unique != found->unique) ++context_generation;
    context_unique = found->unique;
    schedule_dispatch();
  }

  void schedule_dispatch() {
    if (dispatch_idle || closed || closing) return;
    dispatch_idle = g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
      auto *current = static_cast<ManagementService *>(data);
      current->dispatch_idle = 0;
      if (service.get() == current) current->dispatch();
      return G_SOURCE_REMOVE;
    }, this, nullptr);
  }

  static JsonArray *actual_inventory(CefRefPtr<CefDictionaryValue> result, std::string *error) {
    if (!result) { *error = "Chromium did not return an extension inventory"; return nullptr; }
    if (result->HasKey("exceptionDetails")) {
      auto details = result->GetDictionary("exceptionDetails");
      auto exception = details ? details->GetDictionary("exception") : nullptr;
      *error = exception ? exception->GetString("description").ToString() : "";
      if (error->empty()) *error = details ? details->GetString("text").ToString() : "";
      if (error->empty()) *error = "The extension manager helper failed";
      return nullptr;
    }
    auto runtime = result->GetDictionary("result");
    auto value = runtime ? runtime->GetValue("value") : nullptr;
    if (!value) { *error = "The extension manager helper returned no inventory"; return nullptr; }
    auto encoded = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, encoded.c_str(), encoded.size(), nullptr) ||
        !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
      *error = "The extension manager helper returned invalid data"; return nullptr;
    }
    auto *copy = json_array_new();
    auto *input = json_node_get_array(json_parser_get_root(parser));
    for (guint i = 0; i < json_array_get_length(input); ++i)
      json_array_add_element(copy, json_node_copy(json_array_get_element(input, i)));
    return copy;
  }

  static std::string chromium_error(CefRefPtr<CefDictionaryValue> result) {
    if (!result) return "Chromium rejected the extension operation";
    auto message = result->GetString("message").ToString();
    auto error = result->GetDictionary("error");
    if (message.empty() && error) message = error->GetString("message").ToString();
    return message.empty() ? "Chromium rejected the extension operation" : message;
  }

  JsonArray *merge(JsonArray *actual) const {
    std::unordered_map<std::string, bool> state;
    for (guint i = 0; actual && i < json_array_get_length(actual); ++i) {
      auto *item = json_array_get_object_element(actual, i);
      const char *id = item ? json_object_get_string_member_with_default(item, "id", "") : "";
      if (*id) state[id] = json_object_get_boolean_member_with_default(item, "enabled", false);
    }
    auto *merged = json_array_new();
    for (guint i = 0; catalog && i < json_array_get_length(catalog); ++i) {
      auto *source = json_array_get_object_element(catalog, i);
      if (!source) continue;
      auto *item = json_object_new();
      g_autoptr(GList) keys = json_object_get_members(source);
      for (auto *key = keys; key; key = key->next) {
        const char *name = static_cast<const char *>(key->data);
        json_object_set_member(item, name, json_node_copy(json_object_get_member(source, name)));
      }
      const char *id = json_object_get_string_member_with_default(source, "id", "");
      auto found = state.find(id);
      json_object_set_boolean_member(item, "available", found != state.end());
      json_object_set_boolean_member(item, "loaded", found != state.end());
      json_object_set_boolean_member(item, "enabled", found != state.end() && found->second);
      json_array_add_object_element(merged, item);
    }
    return merged;
  }

  void enqueue(std::shared_ptr<Request> request) {
    waiting.push_back(std::move(request));
    start();
    dispatch();
  }

  void dispatch() {
    if (closed || closing || !browser || !page_loaded || context_unique.empty()) return;
    while (!waiting.empty() && inflight.empty()) {
      auto request = waiting.front(); waiting.erase(waiting.begin());
      if (request->complete) continue;
      if (g_task_return_error_if_cancelled(request->task)) { complete(request); continue; }
      if (request->mutate && !owned(request->id)) {
        fail(request, G_IO_ERROR_PERMISSION_DENIED, "This extension is not managed by Jupiter"); continue;
      }
      auto params = CefDictionaryValue::Create();
      std::string expression = request->mutate
        ? "JupiterExtensionManager.setEnabled(" + quote(request->id) + "," + (request->enabled ? "true" : "false") + ")"
        : "JupiterExtensionManager.inventory()";
      params->SetString("expression", expression);
      params->SetBool("awaitPromise", true); params->SetBool("returnByValue", true);
      params->SetString("uniqueContextId", context_unique);
      int id = next_devtools_message_id();
      request->context_generation = context_generation;
      inflight.emplace(id, request);
      if (!send_devtools_method(browser, id, "Runtime.evaluate", params)) {
        inflight.erase(id); fail(request, G_IO_ERROR_FAILED, "Chromium rejected the extension operation");
      }
    }
  }

  void response(CefRefPtr<CefBrowser> source, int id, bool ok, const void *json, size_t size) {
    if (!browser || !browser->IsSame(source)) return;
    if (id == frame_tree_request) {
      frame_tree_request = 0;
      if (!ok) return;
      auto value = CefParseJSON(static_cast<const char *>(json), size, JSON_PARSER_RFC);
      auto result = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
      auto tree = result ? result->GetDictionary("frameTree") : nullptr;
      auto frame = tree ? tree->GetDictionary("frame") : nullptr;
      root_frame = frame ? frame->GetString("id").ToString() : "";
      select_context();
      return;
    }
    auto it = inflight.find(id);
    if (it == inflight.end()) return;
    auto request = it->second; inflight.erase(it);
    auto parsed = CefParseJSON(static_cast<const char *>(json), size, JSON_PARSER_RFC);
    auto result = parsed && parsed->GetType() == VTYPE_DICTIONARY ? parsed->GetDictionary() : nullptr;
    if (!ok || closing || request->context_generation != context_generation) {
      auto error = chromium_error(result);
      if (!request->complete) fail(request, G_IO_ERROR_FAILED, error.c_str());
      schedule_dispatch();
      return;
    }
    std::string error;
    g_autoptr(JsonArray) actual = actual_inventory(result, &error);
    if (!actual) { fail(request, G_IO_ERROR_FAILED, error.c_str()); return; }
    if (request->mutate) {
      bool verified = false;
      for (guint i = 0; i < json_array_get_length(actual); ++i) {
        auto *item = json_array_get_object_element(actual, i);
        if (item && request->id == json_object_get_string_member_with_default(item, "id", "")) {
          verified = json_object_get_boolean_member_with_default(item, "enabled", !request->enabled) == request->enabled;
          break;
        }
      }
      if (!verified) {
        if (!request->complete) fail(request, G_IO_ERROR_FAILED, "Chromium did not apply the extension setting");
        schedule_dispatch();
        return;
      }
    }
    if (last_actual) json_array_unref(last_actual);
    last_actual = json_array_ref(actual);
    auto *merged = merge(actual);
    // The notification is synchronous; the caller borrows this task-owned snapshot.
    if (changed) changed(merged);
    if (!request->complete) {
      g_task_return_pointer(request->task, merged, reinterpret_cast<GDestroyNotify>(json_array_unref));
      complete(request);
    } else json_array_unref(merged);
    schedule_dispatch();
  }

  void fail(std::shared_ptr<Request> request, GIOErrorEnum code, const char *message) {
    if (request->complete) return;
    g_task_return_new_error(request->task, G_IO_ERROR, code, "%s", message);
    complete(std::move(request));
  }
  void complete(std::shared_ptr<Request> request) {
    if (request->complete) return;
    request->complete = true;
    std::erase(waiting, request);
    schedule_dispatch();
  }
  void cancel(Request *raw) {
    auto find = [&](const auto &item) { return item.get() == raw; };
    auto waiting_it = std::find_if(waiting.begin(), waiting.end(), find);
    if (waiting_it != waiting.end()) {
      fail(*waiting_it, G_IO_ERROR_CANCELLED, "The extension operation was cancelled");
      schedule_dispatch();
      return;
    }
    for (auto &[id, request] : inflight) if (request.get() == raw) {
      // Chromium has no cancellation for Runtime.evaluate. Keep the slot until
      // its actual response, so a following operation cannot overtake a toggle.
      fail(request, G_IO_ERROR_CANCELLED, "The extension operation was cancelled"); return;
    }
    schedule_dispatch();
  }
  void timeout(Request *raw) {
    raw->timeout = 0;
    auto find = [&](const auto &item) { return item.get() == raw; };
    auto waiting_it = std::find_if(waiting.begin(), waiting.end(), find);
    if (waiting_it != waiting.end()) {
      fail(*waiting_it, G_IO_ERROR_TIMED_OUT, "The extension operation timed out");
      schedule_dispatch();
      return;
    }
    for (auto &[id, request] : inflight) if (request.get() == raw) {
      fail(request, G_IO_ERROR_TIMED_OUT, "The extension operation timed out");
      close();
      return;
    }
    schedule_dispatch();
  }
  void lost(CefRefPtr<CefBrowser> source) {
    if (browser && !browser->IsSame(source)) return;
    browser = nullptr; registration = nullptr; page_loaded = false; root_frame.clear(); context_unique.clear();
    closed = true;
    if (!closing)
      fail_all(G_IO_ERROR_CLOSED, "The extension manager helper closed unexpectedly");
    closing = false;
  }
  void fail_all(GIOErrorEnum code, const char *message) {
    auto queued = std::move(waiting); waiting.clear();
    for (auto &request : queued) fail(request, code, message);
    auto sent = std::move(inflight); inflight.clear();
    for (auto &[id, request] : sent) fail(request, code, message);
  }
  void close() {
    for (auto &weak : installations)
      if (auto installation = weak.lock()) g_cancellable_cancel(installation->cancellable);
    if (closed || closing) return;
    closing = true; fail_all(G_IO_ERROR_CLOSED, "The extension manager is closing");
    if (browser && browser->IsValid()) browser->GetHost()->CloseBrowser(true);
    // If creation is in flight, OnAfterCreated closes the helper and OnBeforeClose
    // is the terminal acknowledgement before the service may be discarded.
    else if (!starting) { closed = true; closing = false; }
  }
};

std::unique_ptr<ManagementService> service;

void ManagementClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) { if (service_ == service.get()) service_->connected(browser); }
void ManagementClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (service_ == service.get()) service_->lost(browser);
}
void ManagementClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) {
  if (frame->IsMain() && service_ == service.get()) service_->loaded(browser);
}
void ManagementClient::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int id, bool ok, const void *json, size_t size) {
  if (service_ != service.get()) return;
  struct Delivery { ManagementService *service; CefRefPtr<CefBrowser> browser; int id; bool ok; std::string json; };
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    auto *delivery = static_cast<Delivery *>(data);
    if (delivery->service == service.get())
      delivery->service->response(delivery->browser, delivery->id, delivery->ok, delivery->json.data(), delivery->json.size());
    return G_SOURCE_REMOVE;
  }, new Delivery{service_, browser, id, ok, std::string(static_cast<const char *>(json), size)},
  +[](gpointer data) { delete static_cast<Delivery *>(data); });
}
void ManagementClient::OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void *json, size_t size) {
  if (service_ != service.get()) return;
  if (method == "Runtime.executionContextCreated") service_->context_created(browser, json, size);
  else if (method == "Runtime.executionContextsCleared") service_->context_lost(browser, nullptr, 0, true);
  else if (method == "Runtime.executionContextDestroyed") service_->context_lost(browser, json, size);
}

std::shared_ptr<Request> make_request(EphyEngineSession *session, GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data, gpointer source_tag) {
  auto request = std::make_shared<Request>();
  request->task = g_task_new(session, cancellable, callback, user_data);
  g_task_set_source_tag(request->task, source_tag);
  request->timeout = g_timeout_add_full(G_PRIORITY_DEFAULT, 30000, +[](gpointer data) -> gboolean {
    auto *raw = static_cast<Request *>(data);
    if (service) service->timeout(raw);
    return G_SOURCE_REMOVE;
  }, request.get(), nullptr);
  if (cancellable) {
    auto *dispatch = new CancelDispatch{service.get(), request};
    request->cancelled = g_cancellable_connect(cancellable, G_CALLBACK((+[](GCancellable *, gpointer data) {
      auto *source = static_cast<CancelDispatch *>(data);
      auto *delivery = new CancelDispatch{source->service, source->request};
      g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer idle_data) -> gboolean {
        std::unique_ptr<CancelDispatch> pending(static_cast<CancelDispatch *>(idle_data));
        auto request = pending->request.lock();
        if (request && service.get() == pending->service) service->cancel(request.get());
        return G_SOURCE_REMOVE;
      }, delivery, nullptr);
    })), dispatch, +[](gpointer data) { delete static_cast<CancelDispatch *>(data); });
  }
  return request;
}

void begin(EphyEngineSession *session, std::shared_ptr<Request> request) {
  if (g_task_return_error_if_cancelled(request->task)) {
    request->complete = true;
    return;
  }
  if (!service) {
    g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CLOSED, "The extension manager is unavailable");
    request->complete = true;
    return;
  }
  GError *error = nullptr;
  if (!service->usable(session, &error)) {
    g_task_return_error(request->task, error);
    request->complete = true;
    return;
  }
  service->enqueue(std::move(request));
}
} // namespace

void ephy_engine_extensions_initialize(JsonArray *owned, CefRefPtr<CefRequestContext> context,
                                       std::function<void(JsonArray *)> changed, Extensions *store) {
  ephy_engine_extensions_shutdown();
  if (service) {
    g_warning("Extension manager helper is still closing");
    return;
  }
  if (!owned || !context) return;
  service = std::make_unique<ManagementService>();
  service->catalog = json_array_ref(owned);
  service->store = store;
  service->context = context;
  service->changed = std::move(changed);
  try {
    auto manager = Extensions::read_manifest(JUPITER_MANAGER_DIR);
    service->helper_origin = "chrome-extension://" + manager.id;
    service->helper_uri = service->helper_origin + "/helper.html";
    service->client = new ManagementClient(service.get());
    service->closed = false;
    service->start();
  } catch (const std::exception &error) {
    g_warning("Extension manager helper: %s", error.what());
    service->closed = true;
  }
}

void ephy_engine_extensions_close() { if (service) service->close(); }
bool ephy_engine_extensions_is_closed() { return !service || service->closed; }
void ephy_engine_extensions_shutdown() {
  if (!service) return;
  service->close();
  if (service->closed) service.reset();
}

void ephy_engine_extensions_query_async(EphyEngineSession *session, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data) {
  begin(session, make_request(session, cancellable, callback, user_data,
                              reinterpret_cast<gpointer>(ephy_engine_extensions_query_async)));
}
JsonArray *ephy_engine_extensions_query_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), nullptr);
  if (!g_task_is_valid(result, session) ||
      g_task_get_source_tag(G_TASK(result)) != reinterpret_cast<gpointer>(ephy_engine_extensions_query_async)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid extension query result");
    return nullptr;
  }
  return static_cast<JsonArray *>(g_task_propagate_pointer(G_TASK(result), error));
}
void ephy_engine_extensions_set_enabled_async(EphyEngineSession *session, const char *id, gboolean enabled,
                                              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  auto request = make_request(session, cancellable, callback, user_data,
                               reinterpret_cast<gpointer>(ephy_engine_extensions_set_enabled_async));
  request->mutate = true; request->enabled = enabled; request->id = id ? id : "";
  begin(session, std::move(request));
}
JsonArray *ephy_engine_extensions_set_enabled_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), nullptr);
  if (!g_task_is_valid(result, session) ||
      g_task_get_source_tag(G_TASK(result)) != reinterpret_cast<gpointer>(ephy_engine_extensions_set_enabled_async)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid extension operation result");
    return nullptr;
  }
  return static_cast<JsonArray *>(g_task_propagate_pointer(G_TASK(result), error));
}

namespace {
JsonObject *installation_metadata(const Extension &extension) {
  auto *item = json_object_new();
  for (const auto &[key, value] : {std::pair{"id", extension.id}, {"name", extension.name},
       {"version", extension.version}, {"path", extension.path}, {"popup", extension.popup},
       {"options", extension.options}})
    json_object_set_string_member(item, key, value.c_str());
  for (const char *key : {"description", "author", "homepage", "icon"})
    json_object_set_string_member(item, key, "");
  json_object_set_int_member(item, "manifest_version", extension.manifest);
  json_object_set_boolean_member(item, "has_action", extension.has_action);
  json_object_set_boolean_member(item, "builtin", false);
  json_object_set_boolean_member(item, "enabled", false);
  json_object_set_boolean_member(item, "available", false);
  json_object_set_boolean_member(item, "loaded", false);
  json_object_set_boolean_member(item, "restart_required", true);
  return item;
}

void installation_prepared(GObject *source, GAsyncResult *result, gpointer user_data) {
  g_autoptr(GTask) task = G_TASK(user_data);
  auto installation = *static_cast<std::shared_ptr<Installation> *>(g_task_get_task_data(G_TASK(result)));
  GError *error = nullptr;
  std::unique_ptr<Extensions::PendingInstall> pending(static_cast<Extensions::PendingInstall *>(
    g_task_propagate_pointer(G_TASK(result), &error)));
  if (!pending) { g_task_return_error(task, error); return; }
  auto owner = installation->owner.lock();
  if (!owner || !service || service->lifetime != owner || !service->store) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "The extension manager is closed");
    return;
  }
  if (!service->usable(EPHY_ENGINE_SESSION(source), &error)) {
    g_task_return_error(task, error);
    return;
  }
  if (g_cancellable_is_cancelled(installation->cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The extension installation was cancelled");
    return;
  }
  const auto &id = pending->extension.id;
  if (service->owned(id) || service->helper_origin == "chrome-extension://" + id ||
      id == Extensions::id_for_path(JUPITER_BRIDGE_DIR)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS, "An extension with this ID is already installed");
    return;
  }
  try {
    // Cancellation is honored before this atomic publication. Once committed,
    // report the real staged result even if the initiating preferences closes.
    auto *metadata = installation_metadata(pending->extension);
    g_autoptr(JsonObject) metadata_owner = metadata;
    service->store->commit_install(*pending);
    json_array_add_object_element(service->catalog, json_object_ref(metadata));
    g_autoptr(JsonArray) snapshot = service->merge(service->last_actual);
    if (service->changed) service->changed(snapshot);
    g_task_return_pointer(task, json_object_ref(metadata), reinterpret_cast<GDestroyNotify>(json_object_unref));
  } catch (const std::exception &failure) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", failure.what());
  }
}
}

void ephy_engine_extensions_install_unpacked_async(EphyEngineSession *session, GFile *directory,
                                                  GCancellable *cancellable, GAsyncReadyCallback callback,
                                                  gpointer user_data) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session));
  auto *task = g_task_new(session, cancellable, callback, user_data);
  g_task_set_source_tag(task, reinterpret_cast<gpointer>(ephy_engine_extensions_install_unpacked_async));
  g_task_set_check_cancellable(task, false);
  GError *error = nullptr;
  if (!service || !service->store) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "The extension manager is unavailable");
    g_object_unref(task);
    return;
  }
  if (!service->usable(session, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_autofree char *path = G_IS_FILE(directory) ? g_file_get_path(directory) : nullptr;
  if (!path || !*path) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Choose a local unpacked extension directory");
    g_object_unref(task);
    return;
  }
  auto installation = std::make_shared<Installation>();
  installation->owner = service->lifetime;
  installation->source = path;
  installation->directory = service->store->directory();
  if (cancellable) {
    installation->caller = G_CANCELLABLE(g_object_ref(cancellable));
    installation->cancelled = g_cancellable_connect(cancellable, G_CALLBACK((+[](GCancellable *, gpointer data) {
      g_cancellable_cancel(G_CANCELLABLE(data));
    })), g_object_ref(installation->cancellable), g_object_unref);
  }
  std::erase_if(service->installations, [](const auto &weak) { return weak.expired(); });
  service->installations.push_back(installation);
  auto *worker = g_task_new(session, installation->cancellable, installation_prepared, task);
  g_task_set_check_cancellable(worker, false);
  g_task_set_task_data(worker, new std::shared_ptr<Installation>(installation), +[](gpointer data) {
    delete static_cast<std::shared_ptr<Installation> *>(data);
  });
  g_task_run_in_thread(worker, +[](GTask *worker, gpointer, gpointer data, GCancellable *token) {
    const auto &installation = *static_cast<std::shared_ptr<Installation> *>(data);
    try {
      auto pending = Extensions::prepare_install(installation->source, installation->directory, token);
      g_task_return_pointer(worker, pending.release(), +[](gpointer data) {
        delete static_cast<Extensions::PendingInstall *>(data);
      });
    } catch (const std::system_error &failure) {
      g_task_return_new_error(worker, G_IO_ERROR, g_io_error_from_errno(failure.code().value()), "%s", failure.what());
    } catch (const std::exception &failure) {
      g_task_return_new_error(worker, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s", failure.what());
    }
  });
  g_object_unref(worker);
}

JsonObject *ephy_engine_extensions_install_unpacked_finish(EphyEngineSession *session, GAsyncResult *result,
                                                         GError **error) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), nullptr);
  if (!g_task_is_valid(result, session) || g_task_get_source_tag(G_TASK(result)) !=
      reinterpret_cast<gpointer>(ephy_engine_extensions_install_unpacked_async)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid extension installation result");
    return nullptr;
  }
  return static_cast<JsonObject *>(g_task_propagate_pointer(G_TASK(result), error));
}
