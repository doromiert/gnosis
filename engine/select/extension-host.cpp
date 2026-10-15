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
} // namespace
void ExtensionHost::command(CefRefPtr<CefBrowser> b, const std::string& name,
                            CefRefPtr<CefDictionaryValue> args, Result callback) {
  if (!b || !b->IsValid())
    return;
  int id = sequence_++;
  if (callback)
    pending_[id] = std::move(callback);
  CefPostTask(TID_UI, new Dispatch([b, id, name, args] {
                if (b->IsValid())
                  b->GetHost()->ExecuteDevToolsMethod(id, name, args);
              }));
}
void ExtensionHost::attach(CefRefPtr<CefBrowser> b) {
  int id = b->GetIdentifier();
  if (pages_.contains(id))
    return;
  pages_[id] = {b, b->GetHost()->AddDevToolsMessageObserver(this)};
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
  if (!pages_.empty() && pages_.begin()->first == id) {
    sessions_.clear();
    targets_.clear();
    pending_.clear();
  }
  std::erase_if(trusted_contexts_, [&](const auto& pair) { return pair.first.first == id; });
  std::erase_if(styles_, [&](const auto& pair) { return pair.first.starts_with(std::to_string(id) + ":"); });
  contexts_.erase(id);
  pages_.erase(id);
  if (pages_.empty())
    stop();
}
void ExtensionHost::stop() {
  if (timer_)
    g_source_remove(timer_);
  timer_ = 0;
  waiting_.clear();
  trusted_contexts_.clear();
  pending_.clear();
  sessions_.clear();
  targets_.clear();
  pages_.clear();
  styles_.clear();
  context_id_.clear();
  contexts_.clear();
  frame_ids_.clear();
}
void ExtensionHost::OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success, const void* data,
                                           size_t size) {
  auto it = pending_.find(id);
  if (it == pending_.end())
    return;
  auto cb = std::move(it->second);
  pending_.erase(it);
  auto result = parse(data, size);
  if (!success && g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("Extension CDP error: %s\n", encode(value(result)).c_str());
  if (!success)
    result->SetBool("jupiterError", true);
  cb(result);
}
void ExtensionHost::send(const Origin& origin, const std::string& method,
                         CefRefPtr<CefDictionaryValue> params, Result callback) {
  if (origin.session.empty()) {
    command(origin.browser, method, params, std::move(callback));
    return;
  }
  int id = sequence_++;
  if (callback)
    pending_[id] = std::move(callback);
  auto message = dict();
  message->SetInt("id", id);
  message->SetString("method", method);
  message->SetDictionary("params", params);
  auto args = dict();
  args->SetString("sessionId", origin.session);
  args->SetString("message", encode(value(message)));
  command(origin.browser, "Target.sendMessageToTarget", args);
}
void ExtensionHost::scan() {
  for (const auto& [id, origin] : sessions_)
    hook(origin);
  auto waiting = std::move(waiting_);
  waiting_.clear();
  auto all = tabs ? tabs() : std::vector<ExtensionTab>{};
  for (auto& item : waiting) {
    int id = item.request->GetDictionary("args")->GetInt("tabId");
    if (std::any_of(all.begin(), all.end(), [&](const auto& tab) { return tab.id == id; }) ||
        g_get_monotonic_time() > item.deadline)
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
        Origin o{session, b, 0, ext};
        sessions_[session] = o;
        hook(o);
      });
    }
  });
}
void ExtensionHost::hook(const Origin& origin) {
  if (g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("Extension hook session=%s context=%d\n", origin.session.c_str(), origin.context);
  if (origin.context)
    trusted_contexts_[{origin.browser->GetIdentifier(), origin.context}] = origin.extension;
  auto args = dict();
  args->SetString("name", "__jupiterExtensionRequest");
  if (origin.context)
    args->SetInt("executionContextId", origin.context);
  send(origin, "Runtime.addBinding", args, [this, origin](auto) {
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
  auto args = parse(data, size);
  std::string name = method;
  if (g_getenv("JUPITER_EXTENSION_DEBUG") && name.starts_with("Target."))
    g_print("Extension event %s: %.300s\n", name.c_str(), encode(value(args)).c_str());
  if (name == "Target.receivedMessageFromTarget") {
    auto session = args->GetString("sessionId").ToString();
    if (!sessions_.contains(session))
      return;
    auto message = args->GetString("message").ToString();
    auto event = parse(message.data(), message.size());
    if (event->HasKey("id")) {
      int id = event->GetInt("id");
      auto it = pending_.find(id);
      if (it != pending_.end()) {
        auto cb = std::move(it->second);
        pending_.erase(it);
        auto result = event->GetDictionary("result");
        cb(result ? result : dict());
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
  if (name == "Runtime.executionContextsCleared") {
    contexts_[b->GetIdentifier()].clear();
    std::erase_if(trusted_contexts_,
                  [&](const auto& pair) { return pair.first.first == b->GetIdentifier(); });
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
      hook({"", b, context->GetInt("id"), ext});
  } else if (name == "Runtime.bindingCalled" && args->GetString("name") == "__jupiterExtensionRequest") {
    auto payload = args->GetString("payload").ToString();
    auto key = std::make_pair(b->GetIdentifier(), args->GetInt("executionContextId"));
    if (trusted_contexts_.contains(key))
      request({"", b, key.second, trusted_contexts_[key]}, parse(payload.data(), payload.size()));
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
bool ExtensionHost::allowed(const std::string& id, const std::string& url, int tab) const {
  auto grant = active_grants.find(id);
  if (grant != active_grants.end() && grant->second == tab)
    return true;
  auto found = permissions.find(id);
  if (found == permissions.end())
    return false;
  for (const auto& pattern : found->second) {
    if (pattern == "<all_urls>" &&
        (url.starts_with("http:") || url.starts_with("https:") || url.starts_with("file:")))
      return true;
    if (pattern.find("://") != std::string::npos && g_pattern_match_simple(pattern.c_str(), url.c_str()))
      return true;
  }
  return false;
}
void ExtensionHost::request(const Origin& origin, CefRefPtr<CefDictionaryValue> request, bool defer) {
  int id = request->GetInt("id");
  auto op = request->GetString("op").ToString();
  auto args = request->GetDictionary("args");
  if (!args)
    return;
  auto all = tabs ? tabs() : std::vector<ExtensionTab>{};
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
  int tab_id = args->GetInt("tabId");
  auto found = std::find_if(all.begin(), all.end(), [&](const auto& t) { return t.id == tab_id; });
  if (found == all.end()) {
    if (defer && (op == "insertCSS" || op == "removeCSS" || op == "executeScript") && waiting_.size() < 256) {
      waiting_.push_back({origin, request, g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND});
      return;
    }
    reply(origin, id, result);
    return;
  }
  auto details = args->GetDictionary("details");
  if (details && details->HasKey("frameId") && details->GetInt("frameId") > 0 &&
      !frame_ids_.contains({found->browser->GetIdentifier(), details->GetInt("frameId")})) {
    if (defer && waiting_.size() < 256) {
      waiting_.push_back({origin, request, g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND});
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
  auto code = details->GetString("code").ToString();
  if (g_getenv("JUPITER_EXTENSION_DEBUG"))
    g_print("CSS adapter: tab=%d ext=%s url=%s bytes=%zu\n", tab.id, extension.c_str(),
            tab.info->GetString("url").ToString().c_str(), code.size());
  auto key = std::to_string(tab.browser->GetIdentifier()) + ":" + extension + ":" + code;
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
                          if (response->HasKey("exceptionDetails"))
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
                        [evaluate](auto response) { evaluate(response->GetInt("executionContextId")); });
              }
            }
          });
}
