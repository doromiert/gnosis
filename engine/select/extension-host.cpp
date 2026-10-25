#include "extension-permissions.hpp"
#include "devtools.hpp"
extern "C" {
#include <gio/gio.h>
GResource* jupiter_engine_get_resource(void);
}
#include "extension-host.hpp"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <iterator>
#include <memory>
namespace {
class Dispatch final : public CefTask {
public:
  explicit Dispatch(std::function<void()> f) : function(std::move(f)) {}
  void Execute() override { function(); }

private:
  std::function<void()> function;
  IMPLEMENT_REFCOUNTING(Dispatch);
};
CefRefPtr<CefDictionaryValue> dict() {
  return CefDictionaryValue::Create();
}
CefRefPtr<CefValue> value(CefRefPtr<CefDictionaryValue> d) {
  auto v = CefValue::Create();
  v->SetDictionary(d->Copy(false));
  return v;
}
std::string encode(CefRefPtr<CefValue> v) {
  return CefWriteJSON(v, JSON_WRITER_DEFAULT).ToString();
}
std::string quote(const std::string& s) {
  auto v = CefValue::Create();
  v->SetString(s);
  return encode(v);
}
CefRefPtr<CefDictionaryValue> parse(const void* data, size_t size) {
  auto v = CefParseJSON(static_cast<const char*>(data), size, JSON_PARSER_RFC);
  return v && v->GetType() == VTYPE_DICTIONARY ? v->GetDictionary() : dict();
}
std::string extension_id(const std::string& url) {
  if (!url.starts_with("chrome-extension://"))
    return "";
  return url.substr(19, url.find('/', 19) - 19);
}
CefRefPtr<CefDictionaryValue> failure(const std::string& message) {
  auto result = dict();
  result->SetBool("jupiterError", true);
  result->SetString("message", message);
  return result;
}
std::string error_message(CefRefPtr<CefDictionaryValue> result) {
  auto message = result->GetString("message").ToString();
  auto error = result->GetDictionary("error");
  if (message.empty() && error) message = error->GetString("message");
  return message.empty() ? "The extension operation could not be completed" : message;
}
} // namespace
bool ExtensionHost::current(const Origin& origin) const {
  if (!origin.extension.empty() && !extension_ids.contains(origin.extension)) return false;
  if (!origin.browser || !origin.browser->IsValid()) return false;
  auto page = pages_.find(origin.browser->GetIdentifier());
  if (page == pages_.end() || page->second.generation != origin.generation) return false;
  if (!origin.session.empty()) {
    auto session = sessions_.find(origin.session);
    return session != sessions_.end() && session->second.generation == origin.generation &&
      session->second.browser->GetIdentifier() == origin.browser->GetIdentifier();
  }
  if (!origin.context) return true;
  auto contexts = contexts_.find(origin.browser->GetIdentifier());
  if (contexts == contexts_.end()) return false;
  auto context = contexts->second.find(origin.context);
  return context != contexts->second.end() && context->second.extension == origin.extension;
}
void ExtensionHost::set_enabled(const std::string& id, bool enabled) {
  if (enabled) { extension_ids.insert(id); return; }
  extension_ids.erase(id);
  std::set<std::string> revoked;
  for (const auto& [session, origin] : sessions_) if (origin.extension == id) {
    revoked.insert(session);
    targets_.erase(origin.target);
  }
  std::erase_if(sessions_, [&](const auto& pair) { return revoked.contains(pair.first); });
  std::erase_if(trusted_contexts_, [&](const auto& pair) { return pair.second == id; });
  for (auto& [browser, contexts] : contexts_)
    std::erase_if(contexts, [&](const auto& pair) { return pair.second.extension == id; });
  std::erase_if(waiting_, [&](const auto& item) { return item.origin.extension == id; });
  std::vector<int> cancelled;
  for (const auto& [message, pending] : pending_)
    if (revoked.contains(pending.session)) cancelled.push_back(message);
  for (int message : cancelled) fail(message, "The extension was disabled");
}
void ExtensionHost::complete(int id, CefRefPtr<CefDictionaryValue> result) {
  auto it = pending_.find(id);
  if (it == pending_.end()) return;
  auto callback = std::move(it->second.callback);
  pending_.erase(it);
  callback(result);
}
void ExtensionHost::fail(int id, const std::string& message) {
  complete(id, failure(message));
}
void ExtensionHost::command(CefRefPtr<CefBrowser> b, const std::string& name,
                            CefRefPtr<CefDictionaryValue> args, Result callback, const std::string& session) {
  CefRefPtr<ExtensionHost> self = this;
  auto page = b && b->IsValid() ? pages_.find(b->GetIdentifier()) : pages_.end();
  if (page == pages_.end()) {
    if (callback) callback(failure("The destination tab is closed or unavailable"));
    return;
  }
  auto generation = page->second.generation;
  int id = next_devtools_message_id();
  bool has_callback = !!callback;
  if (callback)
    pending_[id] = {b->GetIdentifier(), generation, session, std::move(callback)};
  args = args ? args->Copy(false) : nullptr;
  if (!CefPostTask(TID_UI, new Dispatch([self, b, id, name, args, generation, has_callback, session] {
        auto page = b->IsValid() ? self->pages_.find(b->GetIdentifier()) : self->pages_.end();
        if (page == self->pages_.end() || page->second.generation != generation) {
          self->fail(id, "The destination tab is closed or unavailable");
          return;
        }
        if (has_callback && !self->pending_.contains(id)) return;
        if (!session.empty()) {
          auto source = self->sessions_.find(session);
          if (source == self->sessions_.end() || source->second.generation != generation ||
              source->second.browser->GetIdentifier() != b->GetIdentifier()) {
            self->fail(id, "The extension session was detached");
            return;
          }
        }
        if (!send_devtools_method(b, id, name, args))
          self->fail(id, "The native extension transport rejected the command");
      })))
    fail(id, "The native extension transport is shutting down");
}
void ExtensionHost::attach(CefRefPtr<CefBrowser> b) {
  int id = b->GetIdentifier();
  if (pages_.contains(id))
    return;
  pages_[id] = {b, b->GetHost()->AddDevToolsMessageObserver(this), ++next_generation_};
  command(b, "Runtime.enable", dict());
  command(b, "DOM.enable", dict());
  command(b, "CSS.enable", dict());
  if (!timer_)
    timer_ = g_timeout_add(
        500,
        +[](gpointer p) -> gboolean {
          static_cast<ExtensionHost*>(p)->scan();
          return G_SOURCE_CONTINUE;
        },
        this);
  command(b, "Target.getTargetInfo", dict(), [this](auto result) {
    auto info = result->GetDictionary("targetInfo");
    if (info && context_id_.empty())
      context_id_ = info->GetString("browserContextId");
  });
  scan();
}
void ExtensionHost::detach(int id) {
  CefRefPtr<ExtensionHost> self = this;
  if (!pages_.contains(id)) return;
  if (pages_.begin()->first == id)
    targets_.clear();
  // Keep the registration alive until its page and pending ownership have been invalidated.
  auto page = std::move(pages_.at(id));
  pages_.erase(id);
  std::erase_if(sessions_, [&](const auto& pair) { return pair.second.browser->GetIdentifier() == id; });
  std::erase_if(trusted_contexts_, [&](const auto& pair) { return pair.first.first == id; });
  std::erase_if(styles_, [&](const auto& pair) { return pair.first.starts_with(std::to_string(id) + ":"); });
  contexts_.erase(id);
  std::erase_if(frame_ids_, [&](const auto& pair) { return pair.first.first == id; });
  std::vector<int> cancelled;
  for (const auto& [message, pending] : pending_)
    if (pending.browser == id) cancelled.push_back(message);
  for (int message : cancelled) fail(message, "The destination tab was closed");
  auto waiting = std::move(waiting_);
  waiting_.clear();
  for (auto& item : waiting) {
    if (!current(item.origin)) continue;
    if (item.destination == id) {
      auto null = CefValue::Create(); null->SetNull();
      reply(item.origin, item.request->GetInt("id"), null, "The destination tab was closed");
    } else waiting_.push_back(std::move(item));
  }
  if (pages_.empty())
    stop();
}
void ExtensionHost::stop() {
  CefRefPtr<ExtensionHost> self = this;
  if (timer_)
    g_source_remove(timer_);
  timer_ = 0;
  waiting_.clear();
  popup_preparations_.clear();
  trusted_contexts_.clear();
  auto pending = std::move(pending_);
  pending_.clear();
  sessions_.clear();
  targets_.clear();
  auto pages = std::move(pages_);
  pages_.clear();
  styles_.clear();
  context_id_.clear();
  contexts_.clear();
  frame_ids_.clear();
  for (auto& [id, item] : pending)
    item.callback(failure("The native extension host stopped"));
}
void ExtensionHost::OnDevToolsAgentDetached(CefRefPtr<CefBrowser> browser) {
  // CEF drops pending responses and subscriptions. Reconnecting the source session is separate work.
  if (browser) detach(browser->GetIdentifier());
}
void ExtensionHost::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int id, bool success, const void* data,
                                           size_t size) {
  auto it = pending_.find(id);
  if (it == pending_.end() || it->second.nested || !browser ||
      it->second.browser != browser->GetIdentifier())
    return;
  auto page = pages_.find(it->second.browser);
  if (page == pages_.end() || page->second.generation != it->second.generation) return;
  auto result = parse(data, size);
  if (!success && g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("Extension CDP error: %s\n", encode(value(result)).c_str());
  if (!success)
    result->SetBool("jupiterError", true);
  complete(id, result);
}
void ExtensionHost::send(const Origin& origin, const std::string& method,
                         CefRefPtr<CefDictionaryValue> params, Result callback) {
  CefRefPtr<ExtensionHost> self = this;
  if (!current(origin)) {
    if (callback) callback(failure("The extension session is closed or unavailable"));
    return;
  }
  if (origin.session.empty()) {
    command(origin.browser, method, params, std::move(callback));
    return;
  }
  int id = next_devtools_message_id();
  if (callback)
    pending_[id] = {origin.browser->GetIdentifier(), origin.generation, origin.session, std::move(callback), true};
  auto message = dict();
  message->SetInt("id", id);
  message->SetString("method", method);
  message->SetDictionary("params", params);
  auto args = dict();
  args->SetString("sessionId", origin.session);
  args->SetString("message", encode(value(message)));
  command(origin.browser, "Target.sendMessageToTarget", args, [self, id](auto result) {
    if (result->GetBool("jupiterError")) self->complete(id, result);
  }, origin.session);
}
void ExtensionHost::prepare_popup(const std::string& extension, std::function<bool()> live,
                                  std::function<void(const std::string&)> done) {
  auto path = extension_paths.find(extension);
  if (!extension_ids.contains(extension) || path == extension_paths.end()) {
    done("The extension is no longer available.");
    return;
  }
  // Persistent MV2 background pages must finish loading before opening an action
  // page. Worker/lazy backgrounds are woken by Chromium messaging instead; waiting
  // for a DOM or an already-running target would deadlock those extensions.
  std::ifstream input(std::filesystem::path(path->second) / "manifest.json");
  std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  auto manifest = CefParseJSON(source, JSON_PARSER_RFC);
  auto object = manifest && manifest->GetType() == VTYPE_DICTIONARY ? manifest->GetDictionary() : nullptr;
  auto background = object ? object->GetDictionary("background") : nullptr;
  if (!background || object->GetInt("manifest_version") != 2 ||
      (background->HasKey("persistent") && !background->GetBool("persistent"))) {
    done("");
    return;
  }
  auto request = std::make_shared<PopupPreparation>(PopupPreparation{
    extension, std::move(live), std::move(done), g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND});
  popup_preparations_.push_back(request);
  poll_popup_preparations();
}
void ExtensionHost::poll_popup_preparations() {
  const auto requests = popup_preparations_;
  for (const auto& request : requests) {
    if (!request->live()) {
      std::erase(popup_preparations_, request);
      continue;
    }
    if (!extension_ids.contains(request->extension) || g_get_monotonic_time() > request->deadline) {
      std::erase(popup_preparations_, request);
      auto done = std::move(request->done);
      if (done) done("The extension background page did not become ready.");
      continue;
    }
    if (request->pending) continue;
    auto found = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto& item) {
      return item.second.extension == request->extension && current(item.second);
    });
    if (found == sessions_.end()) continue;
    request->pending = true;
    auto args = dict();
    args->SetString("expression", "(()=>{try{return document.readyState==='complete' && !!chrome.runtime.getManifest().version && !!chrome.runtime.getURL('')}catch(e){return false}})()");
    args->SetBool("returnByValue", true);
    CefRefPtr<ExtensionHost> self = this;
    send(found->second, "Runtime.evaluate", args, [self, request](auto response) {
      request->pending = false;
      if (!request->done || !request->live() ||
          std::find(self->popup_preparations_.begin(), self->popup_preparations_.end(), request) == self->popup_preparations_.end()) return;
      auto result = response->GetDictionary("result");
      if (!result || result->GetString("type") != "boolean" || !result->GetBool("value")) return;
      std::erase(self->popup_preparations_, request);
      auto done = std::move(request->done);
      done("");
    });
  }
}
void ExtensionHost::scan() {
  poll_popup_preparations();
  for (const auto& [id, origin] : sessions_)
    hook(origin);
  auto waiting = std::move(waiting_);
  waiting_.clear();
  auto all = tabs ? tabs(nullptr) : std::vector<ExtensionTab>{};
  for (auto& item : waiting) {
    if (!current(item.origin)) continue;
    int id = item.request->GetDictionary("args")->GetInt("tabId");
    auto found = std::find_if(all.begin(), all.end(), [&](const auto& tab) { return tab.id == id; });
    bool ready = found != all.end();
    auto details = item.request->GetDictionary("args")->GetDictionary("details");
    if (ready && details && details->HasKey("frameId") && details->GetInt("frameId") > 0)
      ready = frame_ids_.contains({found->browser->GetIdentifier(), details->GetInt("frameId")});
    if (ready || g_get_monotonic_time() > item.deadline)
      request(item.origin, item.request, false);
    else
      waiting_.push_back(std::move(item));
  }
  if (pages_.empty())
    return;
  auto b = pages_.begin()->second.browser;
  command(b, "Target.getTargets", dict(), [this, b](auto result) {
    if (g_getenv("JUPITER_EXTENSION_DEBUG"))
      g_print("Extension targets: %s\n", encode(value(result)).c_str());
    auto list = result->GetList("targetInfos");
    if (!list)
      return;
    for (size_t i = 0; i < list->GetSize(); i++) {
      auto target = list->GetDictionary(i);
      if (!target)
        continue;
      if (context_id_.empty() || target->GetString("browserContextId") != context_id_)
        continue;
      auto type = target->GetString("type").ToString();
      auto id = target->GetString("targetId").ToString();
      auto ext = extension_id(target->GetString("url"));
      if ((type != "background_page" && type != "service_worker") || !extension_ids.contains(ext) ||
          !targets_.insert(id).second)
        continue;
      auto args = dict();
      args->SetString("targetId", id);
      args->SetBool("flatten", false);
      command(b, "Target.attachToTarget", args, [this, b, id, ext](auto reply) {
        auto session = reply->GetString("sessionId").ToString();
        if (session.empty()) {
          targets_.erase(id);
          return;
        }
        auto page = pages_.find(b->GetIdentifier());
        if (page == pages_.end()) return;
        Origin o{session, b, 0, ext, page->second.generation, id};
        sessions_[session] = o;
        hook(o);
      });
    }
  });
}
void ExtensionHost::hook(const Origin& origin) {
  if (!current(origin)) return;
  if (g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("Extension hook session=%s context=%d\n", origin.session.c_str(), origin.context);
  if (origin.context)
    trusted_contexts_[{origin.browser->GetIdentifier(), origin.context}] = origin.extension;
  auto args = dict();
  args->SetString("name", "__jupiterExtensionRequest");
  if (origin.context)
    args->SetInt("executionContextId", origin.context);
  send(origin, "Runtime.addBinding", args, [this, origin](auto result) {
    if (result->GetBool("jupiterError") || !current(origin)) return;
    auto* bytes = g_resource_lookup_data(jupiter_engine_get_resource(), "/io/github/doromiert/Jupiter/extension-tabs.js",
                                          G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
      return;
    gsize size;
    auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    auto args = dict();
    args->SetString("expression", std::string(data, size));
    if (origin.context)
      args->SetInt("contextId", origin.context);
    send(origin, "Runtime.evaluate", args);
    g_bytes_unref(bytes);
  });
}
void ExtensionHost::OnDevToolsEvent(CefRefPtr<CefBrowser> b, const CefString& method, const void* data,
                                    size_t size) {
  if (!b || !b->IsValid() || !pages_.contains(b->GetIdentifier())) return;
  auto args = parse(data, size);
  std::string name = method;
  if (g_getenv("JUPITER_EXTENSION_DEBUG") && name.starts_with("Target."))
    g_print("Extension event %s: %.300s\n", name.c_str(), encode(value(args)).c_str());
  if (name == "Target.receivedMessageFromTarget") {
    auto session = args->GetString("sessionId").ToString();
    auto source = sessions_.find(session);
    if (source == sessions_.end() || !current(source->second) ||
        source->second.browser->GetIdentifier() != b->GetIdentifier())
      return;
    auto message = args->GetString("message").ToString();
    auto event = parse(message.data(), message.size());
    if (event->HasKey("id")) {
      int id = event->GetInt("id");
      auto it = pending_.find(id);
      if (it != pending_.end() && it->second.nested && it->second.session == session &&
          it->second.browser == b->GetIdentifier() &&
          it->second.generation == source->second.generation) {
        auto result = event->GetDictionary("result");
        auto error = event->GetDictionary("error");
        if (error) complete(id, failure(error->GetString("message")));
        else complete(id, result ? result : dict());
      }
    } else if (event->GetString("method") == "Runtime.bindingCalled") {
      auto params = event->GetDictionary("params");
      if (params && params->GetString("name") == "__jupiterExtensionRequest") {
        auto payload = params->GetString("payload").ToString();
        request(sessions_[session], parse(payload.data(), payload.size()));
      }
    }
    return;
  }
  if (name == "Target.detachedFromTarget") {
    auto session = args->GetString("sessionId").ToString();
    auto found = sessions_.find(session);
    if (found == sessions_.end() || found->second.browser->GetIdentifier() != b->GetIdentifier()) return;
    targets_.erase(found->second.target);
    sessions_.erase(found);
    std::vector<int> cancelled;
    for (const auto& [id, pending] : pending_)
      if (pending.session == session) cancelled.push_back(id);
    for (int id : cancelled) fail(id, "The extension session was detached");
    std::erase_if(waiting_, [&](const auto& item) { return item.origin.session == session; });
    return;
  }
  if (name == "Runtime.executionContextsCleared") {
    contexts_[b->GetIdentifier()].clear();
    std::erase_if(trusted_contexts_,
                  [&](const auto& pair) { return pair.first.first == b->GetIdentifier(); });
    std::erase_if(frame_ids_, [&](const auto& pair) { return pair.first.first == b->GetIdentifier(); });
  }
  if (name == "Runtime.executionContextDestroyed") {
    int context = args->GetInt("executionContextId");
    auto& contexts = contexts_[b->GetIdentifier()];
    auto found = contexts.find(context);
    if (found != contexts.end()) {
      auto frame = found->second.frame;
      std::erase_if(frame_ids_, [&](const auto& pair) {
        return pair.first.first == b->GetIdentifier() && pair.second == frame;
      });
      contexts.erase(found);
    }
    trusted_contexts_.erase({b->GetIdentifier(), context});
  }
  if (name == "Runtime.consoleAPICalled") {
    auto all = args->GetList("args");
    int context = args->GetInt("executionContextId");
    auto found = contexts_[b->GetIdentifier()].find(context);
    if (all && all->GetSize() && found != contexts_[b->GetIdentifier()].end() &&
        found->second.extension == tab_bridge_id) {
      auto first = all->GetDictionary(0);
      auto text = first ? first->GetString("value").ToString() : "";
      constexpr std::string_view prefix = "JUPITER_NATIVE_FRAME:";
      if (text.starts_with(prefix)) {
        auto frame = parse(text.data() + prefix.size(), text.size() - prefix.size());
        frame_ids_[{b->GetIdentifier(), frame->GetInt("frameId")}] = found->second.frame;
      }
    }
  }
  if (name == "Runtime.executionContextCreated") {
    auto context = args->GetDictionary("context");
    if (!context)
      return;
    auto ext = extension_id(context->GetString("origin"));
    auto aux = context->GetDictionary("auxData");
    if (g_getenv("JUPITER_EXTENSION_DEBUG"))
      g_print("Extension context: %s\n", encode(value(context)).c_str());
    if (aux)
      contexts_[b->GetIdentifier()][context->GetInt("id")] = {context->GetInt("id"), ext,
                                                              aux->GetString("frameId")};
    if (extension_ids.contains(ext) && aux && aux->GetBool("isDefault"))
      hook({"", b, context->GetInt("id"), ext, pages_.at(b->GetIdentifier()).generation});
  } else if (name == "Runtime.bindingCalled" && args->GetString("name") == "__jupiterExtensionRequest") {
    auto payload = args->GetString("payload").ToString();
    auto key = std::make_pair(b->GetIdentifier(), args->GetInt("executionContextId"));
    if (trusted_contexts_.contains(key))
      request({"", b, key.second, trusted_contexts_[key], pages_.at(b->GetIdentifier()).generation},
              parse(payload.data(), payload.size()));
  }
}
void ExtensionHost::reply(const Origin& origin, int id, CefRefPtr<CefValue> result,
                          const std::string& error) {
  auto args = dict();
  args->SetString("expression", "globalThis.__jupiterExtensionReply?.(" + std::to_string(id) + "," +
                                    encode(result) + "," + quote(error) + ")");
  if (origin.context)
    args->SetInt("contextId", origin.context);
  send(origin, "Runtime.evaluate", args);
}
bool ExtensionHost::allowed(const std::string& id, const std::string& url, int) const {
  auto found = permissions.find(id);
  if (found == permissions.end())
    return false;
  for (const auto& pattern : found->second)
    if (extension_host_permission_matches(pattern, url)) return true;
  return false;
}
void ExtensionHost::request(const Origin& origin, CefRefPtr<CefDictionaryValue> request, bool defer) {
  if (!current(origin)) return;
  int id = request->GetInt("id");
  auto op = request->GetString("op").ToString();
  auto args = request->GetDictionary("args");
  if (!args)
    return;
  auto all = tabs ? tabs(origin.session.empty() ? origin.browser : nullptr) : std::vector<ExtensionTab>{};
  auto result = CefValue::Create();
  result->SetNull();
  if (op == "query") {
    auto list = CefListValue::Create();
    size_t i = 0;
    for (auto& t : all) {
      auto info = t.info;
      if (args->HasKey("active") && args->GetBool("active") != info->GetBool("active"))
        continue;
      if (args->GetBool("currentWindow") && !info->GetBool("currentWindow"))
        continue;
      if (args->HasKey("windowId") && args->GetInt("windowId") != info->GetInt("windowId"))
        continue;
      if (args->HasKey("status") && args->GetString("status") != info->GetString("status"))
        continue;
      if (std::find(permissions[origin.extension].begin(), permissions[origin.extension].end(), "tabs") ==
              permissions[origin.extension].end() &&
          !allowed(origin.extension, info->GetString("url"), t.id)) {
        info = info->Copy(false);
        info->Remove("url");
        info->Remove("title");
      }
      list->SetDictionary(i++, info);
    }
    result->SetList(list);
    reply(origin, id, result);
    return;
  }
  if (op == "create") {
    auto props = args->GetDictionary("properties");
    if (!create || !props) { reply(origin, id, result, "Tab creation is unavailable"); return; }
    auto url = props->GetString("url").ToString();
    if (url.empty()) url = "about:blank";
    CefRefPtr<ExtensionHost> self = this;
    create(origin.session.empty() ? origin.browser : nullptr, origin.extension, url,
           !props->HasKey("active") || props->GetBool("active"),
           [self, origin, id](auto info, const auto& error) {
      auto result = CefValue::Create();
      if (info) result->SetDictionary(info); else result->SetNull();
      if (self->current(origin)) self->reply(origin, id, result, error);
    });
    return;
  }
  int tab_id = args->GetInt("tabId");
  auto found = std::find_if(all.begin(), all.end(), [&](const auto& t) { return t.id == tab_id; });
  if (found == all.end()) {
    if (defer && (op == "insertCSS" || op == "removeCSS" || op == "executeScript") && waiting_.size() < 256) {
      waiting_.push_back({origin, request->Copy(false), g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND});
      return;
    }
    reply(origin, id, result, (op == "executeScript" || op == "insertCSS" || op == "removeCSS")
      ? "The destination tab is unavailable" : "");
    return;
  }
  auto details = args->GetDictionary("details");
  if (details && details->HasKey("frameId") && details->GetInt("frameId") > 0 &&
      !frame_ids_.contains({found->browser->GetIdentifier(), details->GetInt("frameId")})) {
    if (defer && waiting_.size() < 256) {
      waiting_.push_back({origin, request->Copy(false), g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND,
                          found->browser->GetIdentifier()});
      return;
    }
    reply(origin, id, result, "The requested frame is unavailable");
    return;
  }
  if (op == "get" || op == "update") {
    if (op == "update" && update) {
      auto props = args->GetDictionary("properties");
      if (props)
        update(tab_id, props->GetString("url"), props->GetBool("active"));
    }
    auto info = found->info->Copy(false);
    if (std::find(permissions[origin.extension].begin(), permissions[origin.extension].end(), "tabs") ==
            permissions[origin.extension].end() &&
        !allowed(origin.extension, info->GetString("url"), tab_id)) {
      info->Remove("url");
      info->Remove("title");
    }
    result->SetDictionary(info);
    reply(origin, id, result);
    return;
  }
  if (op == "executeScript") {
    if (!allowed(origin.extension, found->info->GetString("url"), tab_id)) {
      reply(origin, id, result, "The extension does not have access to this website.");
      return;
    }
    auto details = args->GetDictionary("details");
    if (!details) {
      reply(origin, id, result, "Missing script details");
      return;
    }
    script(*found, details, origin.extension,
           [this, origin, id](auto result, const auto& error) { reply(origin, id, result, error); });
    return;
  }
  if (op == "insertCSS" || op == "removeCSS") {
    auto details = args->GetDictionary("details");
    if (!details) {
      reply(origin, id, result, "Missing stylesheet details");
      return;
    }
    if (!allowed(origin.extension, found->info->GetString("url"), tab_id)) {
      reply(origin, id, result, "The extension does not have access to this website.");
      return;
    }
    css(*found, details, op == "removeCSS", origin.extension, [this, origin, id] {
      auto v = CefValue::Create();
      v->SetNull();
      reply(origin, id, v);
    });
    return;
  }
  reply(origin, id, result, "Unsupported tab operation");
}
void ExtensionHost::css(const ExtensionTab& tab, CefRefPtr<CefDictionaryValue> details, bool remove,
                        const std::string& extension, std::function<void()> done) {
  // Nested CEF values become invalid when the incoming request is released.
  details = details->Copy(false);
  auto code = details->GetString("code").ToString();
  if (g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("CSS adapter: tab=%d ext=%s url=%s bytes=%zu frame=%d\n", tab.id, extension.c_str(),
            tab.info->GetString("url").ToString().c_str(), code.size(), details->GetInt("frameId"));
  auto frame_key = details->HasKey("frameId") ? std::to_string(details->GetInt("frameId"))
                   : details->GetBool("allFrames") ? "all" : "0";
  auto key = std::to_string(tab.browser->GetIdentifier()) + ":" + extension + ":" + frame_key + ":" + code;
  if (remove) {
    for (const auto& sheet : styles_[key]) {
      auto args = dict();
      args->SetString("styleSheetId", sheet);
      args->SetString("text", "");
      command(tab.browser, "CSS.setStyleSheetText", args);
    }
    styles_.erase(key);
    done();
    return;
  }
  command(tab.browser, "Page.getFrameTree", dict(),
          [this, tab, code, key, details, done, extension](auto result) {
            if (g_getenv("JUPITER_EXTENSION_DEBUG"))
              g_print("CSS frame tree: %s\n", encode(value(result)).c_str());
            auto tree = result->GetDictionary("frameTree");
            if (!tree) {
              done();
              return;
            }
            auto root = tree->GetDictionary("frame");
            std::string wanted = root ? root->GetString("id").ToString() : "";
            if (details->HasKey("frameId") &&
                frame_ids_.contains({tab.browser->GetIdentifier(), details->GetInt("frameId")}))
              wanted = frame_ids_[{tab.browser->GetIdentifier(), details->GetInt("frameId")}];
            std::vector<std::string> frames;
            std::function<void(CefRefPtr<CefDictionaryValue>)> collect = [&](auto node) {
              auto frame = node->GetDictionary("frame");
              if (frame && (details->GetBool("allFrames") || frame->GetString("id") == wanted) &&
                  allowed(extension, frame->GetString("url"), tab.id))
                frames.push_back(frame->GetString("id"));

              auto children = node->GetList("childFrames");
              if (children)
                for (size_t i = 0; i < children->GetSize(); i++)
                  collect(children->GetDictionary(i));
            };
            collect(tree);
            auto pending = std::make_shared<size_t>(frames.size());
            if (frames.empty()) {
              done();
              return;
            }
            for (const auto& frame : frames) {
              auto args = dict();
              args->SetString("frameId", frame);
              // Separate insertions must not overwrite each other's inspector sheet.
              args->SetBool("force", true);
              command(tab.browser, "CSS.createStyleSheet", args,
                      [this, tab, code, key, pending, done](auto result) {
                        if (g_getenv("JUPITER_EXTENSION_DEBUG"))
                          g_print("CSS stylesheet: %s\n", encode(value(result)).c_str());
                        auto sheet = result->GetString("styleSheetId").ToString();
                        if (sheet.empty()) {
                          if (!--*pending)
                            done();
                          return;
                        }
                        styles_[key].push_back(sheet);
                        auto args = dict();
                        args->SetString("styleSheetId", sheet);
                        args->SetString("text", code);
                        command(tab.browser, "CSS.setStyleSheetText", args, [pending, done](auto) {
                          if (!--*pending)
                            done();
                        });
                      });
            }
          });
}

void ExtensionHost::script(const ExtensionTab& tab, CefRefPtr<CefDictionaryValue> details,
                           const std::string& extension,
                           std::function<void(CefRefPtr<CefValue>, const std::string&)> done) {
  details = details->Copy(false);
  auto result = CefValue::Create();
  auto output = CefListValue::Create();
  result->SetList(output);
  std::string code = details->GetString("code");
  auto file = details->GetString("file").ToString();
  if (!file.empty()) {
    while (file.starts_with('/'))
      file.erase(0, 1);
    std::filesystem::path path(file);
    bool safe = extension_paths.contains(extension) && !path.is_absolute();
    for (const auto& part : path)
      if (part == "..")
        safe = false;
    if (!safe) {
      done(result, "Invalid extension script path");
      return;
    }
    auto source = std::filesystem::path(extension_paths[extension]) / path;
    std::error_code error;
    auto size = std::filesystem::file_size(source, error);
    if (error || size > 5 * 1024 * 1024) {
      done(result, "Extension script is missing or exceeds the size limit");
      return;
    }
    std::ifstream stream(source);
    code.assign(std::istreambuf_iterator<char>(stream), {});
  }
  if (code.size() > 5 * 1024 * 1024) {
    done(result, "Extension script exceeds the size limit");
    return;
  }
  command(tab.browser, "Page.getFrameTree", dict(),
          [this, tab, details, extension, code, result, output, done](auto response) {
            if (response->GetBool("jupiterError")) {
              done(result, error_message(response));
              return;
            }
            auto tree = response->GetDictionary("frameTree");
            if (!tree) {
              done(result, "The page is unavailable");
              return;
            }
            auto root = tree->GetDictionary("frame");
            std::string wanted = root ? root->GetString("id").ToString() : "";
            if (details->HasKey("frameId") &&
                frame_ids_.contains({tab.browser->GetIdentifier(), details->GetInt("frameId")}))
              wanted = frame_ids_[{tab.browser->GetIdentifier(), details->GetInt("frameId")}];
            std::vector<std::string> frames;
            std::function<void(CefRefPtr<CefDictionaryValue>)> collect = [&](auto node) {
              auto frame = node->GetDictionary("frame");
              if (frame && (details->GetBool("allFrames") || frame->GetString("id") == wanted) &&
                  allowed(extension, frame->GetString("url"), tab.id))
                frames.push_back(frame->GetString("id"));
              {
                auto children = node->GetList("childFrames");
                if (children)
                  for (size_t i = 0; i < children->GetSize(); i++)
                    collect(children->GetDictionary(i));
              }
            };
            collect(tree);
            if (frames.empty()) {
              done(result, "The extension cannot access this frame");
              return;
            }
            auto count = std::make_shared<size_t>(frames.size());
            auto error = std::make_shared<std::string>();
            for (size_t i = 0; i < frames.size(); i++) {
              auto evaluate = [this, tab, code, result, output, done, count, error, i](int context) {
                auto args = dict();
                args->SetString("expression", code);
                args->SetInt("contextId", context);
                args->SetBool("returnByValue", true);
                args->SetBool("awaitPromise", true);
                command(tab.browser, "Runtime.evaluate", args,
                        [result, output, done, count, error, i](auto response) {
                          if (response->GetBool("jupiterError"))
                            *error = error_message(response);
                          else if (response->HasKey("exceptionDetails"))
                            *error = "The extension script raised an exception";
                          auto remote = response->GetDictionary("result");
                          auto v = remote ? remote->GetValue("value") : nullptr;
                          if (v)
                            output->SetValue(i, v);
                          else
                            output->SetNull(i);
                          if (!--*count)
                            done(result, *error);
                        });
              };
              int context = 0;
              for (const auto& [id, c] : contexts_[tab.browser->GetIdentifier()])
                if (c.extension == extension && c.frame == frames[i]) {
                  context = id;
                  break;
                }
              if (context)
                evaluate(context);
              else {
                auto args = dict();
                args->SetString("frameId", frames[i]);
                args->SetString("worldName", "Jupiter extension " + extension);
                command(tab.browser, "Page.createIsolatedWorld", args,
                        [evaluate, result, done, count, error](auto response) {
                          if (response->GetBool("jupiterError") || !response->GetInt("executionContextId")) {
                            *error = response->GetBool("jupiterError") ? error_message(response)
                              : "The extension execution context is unavailable";
                            if (!--*count) done(result, *error);
                            return;
                          }
                          evaluate(response->GetInt("executionContextId"));
                        });
              }
            }
          });
}
