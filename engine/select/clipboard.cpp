#include "clipboard.hpp"
#include "application.hpp"
#include "browser.hpp"
#include "policy.hpp"

#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include <cstring>
#include <functional>
#include <json-glib/json-glib.h>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace {
struct Item {
  std::string mime;
  std::string bytes;
};
struct Paste {
  CefRefPtr<Browser> browser;
  CefRefPtr<CefFrame> frame;
  std::string url;
  std::vector<Item> items;
  unsigned pending = 0;
  std::function<void(Paste&)> complete;
  bool failed = false;
  size_t total = 0;
  GCancellable* cancel = g_cancellable_new();
  guint timeout = 0;
  GdkClipboard* clipboard = nullptr;
  gulong changed = 0;
  ~Paste() {
    if (timeout) g_source_remove(timeout);
    if (changed) g_signal_handler_disconnect(clipboard, changed);
    g_clear_object(&clipboard);
    g_object_unref(cancel);
  }
};
using SharedPaste = std::shared_ptr<Paste>;
std::string encode_items(const std::vector<Item>& items) {
  JsonArray* array = json_array_new();
  for (const auto& item : items) {
    JsonObject* object = json_object_new();
    json_object_set_string_member(object, "type", item.mime.c_str());
    char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(item.bytes.data()), item.bytes.size());
    json_object_set_string_member(object, "data", encoded);
    g_free(encoded);
    json_array_add_object_element(array, object);
  }
  JsonNode* node = json_node_new(JSON_NODE_ARRAY);
  json_node_take_array(node, array);
  JsonGenerator* generator = json_generator_new();
  json_generator_set_root(generator, node);
  g_autofree char* encoded = json_generator_to_data(generator, nullptr);
  std::string result(encoded);
  g_object_unref(generator);
  json_node_free(node);
  return result;
}
void finished(const SharedPaste& paste) {
  if (--paste->pending)
    return;
  if (paste->timeout) { g_source_remove(paste->timeout); paste->timeout = 0; }
  if (paste->changed) { g_signal_handler_disconnect(paste->clipboard, paste->changed); paste->changed = 0; }
  if (paste->complete) { paste->complete(*paste); return; }
  if (paste->browser->closing || !paste->frame->IsValid() || paste->frame->GetURL().ToString() != paste->url)
    return;
  if (paste->items.empty()) {
    if (auto* window = paste->browser->owner())
      window->toast("No supported content in the clipboard");
    return;
  }
  // Dispatch web clipboard data for web editors' object/image handlers. Editable fields
  // also need the default insertion, which synthetic ClipboardEvents do not perform.
  const std::string script = std::string(R"JS((() => {
    const items = )JS") + encode_items(paste->items) +
                             R"JS(;
    const transfer = new DataTransfer();
    let text = '';
    for (const item of items) {
      const bytes = Uint8Array.from(atob(item.data), c => c.charCodeAt(0));
      if (item.type.startsWith('image/')) {
        transfer.items.add(new File([bytes], 'clipboard.' + (item.type === 'image/png' ? 'png' : item.type === 'image/jpeg' ? 'jpg' : 'svg'), {type: item.type}));
      } else {
        const value = new TextDecoder().decode(bytes);
        transfer.setData(item.type, value);
        if (item.type === 'text/plain') text = value;
      }
    }
    const target = document.activeElement || document.body;
    const event = new ClipboardEvent('paste', {clipboardData: transfer, bubbles: true, cancelable: true, composed: true});
    target.dispatchEvent(event);
    if (!event.defaultPrevented && (target.isContentEditable || target.matches('input:not([readonly]):not([disabled]),textarea:not([readonly]):not([disabled])'))) {
      document.execCommand('insertText', false, text);
    }
  })())JS";
  paste->frame->ExecuteJavaScript(script, "gnosis://clipboard", 1);
}
struct Read {
  SharedPaste paste;
  std::string mime;
  std::string bytes;
  GInputStream* stream = nullptr;
  const char* requested[2] = {nullptr, nullptr};
  ~Read() {
    if (stream)
      g_object_unref(stream);
  }
};
void read_next(Read* read);
void read_chunk(GObject* object, GAsyncResult* result, gpointer data) {
  auto* read = static_cast<Read*>(data);
  GBytes* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), result, nullptr);
  if (!bytes) {
    read->paste->failed = true;
    finished(read->paste);
    delete read;
    return;
  }
  gsize size;
  const auto* buffer = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  if (size && read->paste->total + size <= 64 * 1024 * 1024) {
    read->paste->total += size;
    read->bytes.append(buffer, size);
    g_bytes_unref(bytes);
    read_next(read);
    return;
  }
  if (!size)
    read->paste->items.push_back({read->mime, std::move(read->bytes)});
  else {
    read->paste->failed = true;
    g_cancellable_cancel(read->paste->cancel);
  }
  g_bytes_unref(bytes);
  finished(read->paste);
  delete read;
}
void read_next(Read* read) {
  g_input_stream_read_bytes_async(read->stream, 65536, G_PRIORITY_DEFAULT, read->paste->cancel, read_chunk, read);
}
void read_formats(const SharedPaste& paste, bool plain_text, bool rich, const std::set<std::string>* types = nullptr) {
  GdkClipboard* clipboard = gtk_widget_get_clipboard(paste->browser->widget());
  GdkContentFormats* formats = gdk_clipboard_get_formats(clipboard);
  std::vector<std::string> mimes;
  if (!plain_text)
    for (const char* mime : {"text/html", "image/png", "image/jpeg", "image/svg+xml", "application/x-figma"})
      if ((!types || types->contains(!strcmp(mime, "application/x-figma") ? "web application/x-figma" : mime)) &&
          gdk_content_formats_contain_mime_type(formats, mime) &&
          (!rich || strcmp(mime, "image/jpeg")))
        mimes.emplace_back(mime);
  // Match ClipboardOzone::ReadPng: transfer encoded PNG without a host-side decode.
  g_autoptr(GdkTexture) texture = nullptr;
  if (rich && (!types || types->contains("image/png")) &&
      !gdk_content_formats_contain_mime_type(formats, "image/png")) {
    // Local GTK textures are already decoded; encode those only when no PNG is offered.
    if (auto* content = gdk_clipboard_get_content(clipboard)) {
      GValue value = G_VALUE_INIT;
      g_value_init(&value, GDK_TYPE_TEXTURE);
      if (gdk_content_provider_get_value(content, &value, nullptr))
        texture = GDK_TEXTURE(g_value_dup_object(&value));
      g_value_unset(&value);
    }
  }
  const bool text = (!types || types->contains("text/plain")) &&
                   (gdk_content_formats_contain_gtype(formats, G_TYPE_STRING) ||
                    gdk_content_formats_contain_mime_type(formats, "text/plain") ||
                    gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8"));
  paste->pending = mimes.size() + (text ? 1 : 0) + (texture ? 1 : 0);
  if (!paste->pending) {
    paste->pending = 1;
    finished(paste);
    return;
  }
  if (rich) {
    paste->clipboard = GDK_CLIPBOARD(g_object_ref(clipboard));
    paste->changed = g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard*, Paste* paste) {
      paste->failed = true;
      g_cancellable_cancel(paste->cancel);
    }), paste.get());
  }
  paste->timeout = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
    auto* paste = static_cast<Paste*>(data);
    paste->timeout = 0;
    paste->failed = true;
    g_cancellable_cancel(paste->cancel);
    return G_SOURCE_REMOVE;
  }, paste.get());
  if (text) {
    gdk_clipboard_read_text_async(
        clipboard, paste->cancel,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
          auto paste = *static_cast<SharedPaste*>(data);
          delete static_cast<SharedPaste*>(data);
          char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(object), result, nullptr);
          if (text && paste->total + strlen(text) <= 64 * 1024 * 1024) {
            paste->total += strlen(text);
            paste->items.push_back({"text/plain", text});
          } else paste->failed = true;
          g_free(text);
          finished(paste);
        },
        new SharedPaste(paste));
  }
  if (texture) {
    if (static_cast<uint64_t>(gdk_texture_get_width(texture)) * gdk_texture_get_height(texture) * 4 <= 64 * 1024 * 1024) {
      g_autoptr(GBytes) bytes = gdk_texture_save_to_png_bytes(texture);
      gsize size = 0;
      const auto* data = bytes ? static_cast<const char*>(g_bytes_get_data(bytes, &size)) : nullptr;
      if (data && paste->total + size <= 64 * 1024 * 1024) {
        paste->total += size;
        paste->items.push_back({"image/png", std::string(data, size)});
      } else paste->failed = true;
    } else paste->failed = true;
    finished(paste);
  }
  for (const auto& mime : mimes) {
    auto* read = new Read();
    read->paste = paste;
    read->mime = mime;
    read->requested[0] = read->mime.c_str();
    gdk_clipboard_read_async(
        clipboard, read->requested, G_PRIORITY_DEFAULT, paste->cancel,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
          auto* read = static_cast<Read*>(data);
          const char* mime = nullptr;
          read->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(object), result, &mime, nullptr);
          if (!read->stream) {
            read->paste->failed = true;
            finished(read->paste);
            delete read;
            return;
          }
          read_next(read);
        },
        read);
  }
}
} // namespace
void Clipboard::paste(Browser& browser, bool plain_text) {
  if (!browser.cef_browser()) return;
  browser.activate_clipboard();
  auto paste = std::make_shared<Paste>();
  paste->browser = &browser;
  paste->frame = browser.cef_browser()->GetFocusedFrame();
  if (!paste->frame) paste->frame = browser.cef_browser()->GetMainFrame();
  if (!paste->frame || !paste->frame->IsValid()) return;
  paste->url = paste->frame->GetURL();
  read_formats(paste, plain_text, false);
}

namespace {
constexpr const char* write_message = "gnosis.clipboard.write";
constexpr const char* result_message = "gnosis.clipboard.result";
constexpr const char* read_message = "gnosis.clipboard.read-text";
constexpr const char* rich_read_message = "gnosis.clipboard.read";
constexpr const char* read_result_message = "gnosis.clipboard.read-text-result";
constexpr size_t clipboard_limit = 64 * 1024 * 1024;
struct Context {
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefFrame> frame;
  CefRefPtr<CefV8Context> value;
  CefRefPtr<CefV8Value> clipboard, read_text, read, then;
  std::string token;
  std::map<std::string, CefRefPtr<CefV8Value>> reads;
};
class ApprovedRead final : public CefV8Handler {
public:
  ApprovedRead(std::weak_ptr<Context> context, bool rich, CefRefPtr<CefListValue> types)
    : context_(std::move(context)), rich_(rich), types_(types) {}
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList&,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    auto context = context_.lock();
    if (!context || !context->frame->IsValid()) { exception = "Clipboard document was destroyed."; return true; }
    // Only the captured native Promise.then can invoke this after Blink grants the read.
    g_autofree char* id = g_uuid_string_random();
    result = CefV8Value::CreatePromise();
    context->reads.emplace(id, result);
    auto message = CefProcessMessage::Create(rich_ ? rich_read_message : read_message);
    auto args = message->GetArgumentList();
    args->SetString(0, context->token);
    args->SetString(1, id);
    if (rich_) {
      if (types_) args->SetList(2, types_);
      else args->SetNull(2);
    }
    context->frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }
private:
  std::weak_ptr<Context> context_;
  bool rich_;
  CefRefPtr<CefListValue> types_;
  IMPLEMENT_REFCOUNTING(ApprovedRead);
};
class ReadHandler final : public CefV8Handler {
public:
  ReadHandler(std::weak_ptr<Context> context, bool rich) : context_(std::move(context)), rich_(rich) {}
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    auto context = context_.lock();
    if (!context) { exception = "Clipboard document was destroyed."; return true; }
    auto original = rich_ ? context->read : context->read_text;
    original->SetRethrowExceptions(true);
    auto gate_args = args;
    if (rich_ && !args.empty() && args[0]->IsObject()) {
      auto options = CefV8Value::CreateObject(nullptr, nullptr);
      options->SetValue("types", CefV8Value::CreateArray(0), V8_PROPERTY_ATTRIBUTE_NONE);
      auto unsanitized = args[0]->GetValue("unsanitized");
      if (unsanitized && !unsanitized->IsUndefined())
        options->SetValue("unsanitized", unsanitized, V8_PROPERTY_ATTRIBUTE_NONE);
      // Blink validates the request and prompts; GTK supplies the actual formats.
      gate_args = {options};
    }
    auto permission = original->ExecuteFunction(context->clipboard, gate_args);
    if (permission) {
      CefRefPtr<CefListValue> types;
      if (rich_ && !args.empty() && args[0]->IsObject()) {
        auto values = args[0]->GetValue("types");
        if (values && (values->IsNull() || values->IsArray())) {
          types = CefListValue::Create();
          if (values->IsArray())
            for (int i = 0; i < values->GetArrayLength(); ++i)
              types->SetString(i, values->GetValue(i)->GetStringValue());
        }
      }
      result = context->then->ExecuteFunction(permission, {
        CefV8Value::CreateFunction("nativeClipboardRead", new ApprovedRead(context, rich_, types))});
    }
    return true;
  }
private:
  std::weak_ptr<Context> context_;
  bool rich_;
  IMPLEMENT_REFCOUNTING(ReadHandler);
};
class WriteHandler final : public CefV8Handler {
public:
  WriteHandler(CefRefPtr<CefFrame> frame, std::string context) : frame_(frame), context_(std::move(context)) {}
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    if (args.size() != 1 || !args[0]->IsString() || !frame_->IsValid()) {
      exception = "Invalid clipboard request.";
      return true;
    }
    const auto value = args[0]->GetStringValue();
    if (value.length() > 96 * 1024 * 1024) {
      exception = "Clipboard content is too large.";
      return true;
    }
    auto message = CefProcessMessage::Create(write_message);
    message->GetArgumentList()->SetString(0, value);
    message->GetArgumentList()->SetString(1, context_);
    frame_->SendProcessMessage(PID_BROWSER, message);
    result = CefV8Value::CreateUndefined();
    return true;
  }

private:
  CefRefPtr<CefFrame> frame_;
  std::string context_;
  IMPLEMENT_REFCOUNTING(WriteHandler);
};
class Renderer final : public CefRenderProcessHandler {
public:
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    auto* bytes = g_resources_lookup_data("/com/neg_zero/Gnosis/clipboard/bridge.js",
                                          G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
      return;
    g_autofree char* token = g_uuid_string_random();
    auto state = std::make_shared<Context>();
    state->browser = browser; state->frame = frame; state->value = context; state->token = token;
    context->GetGlobal()->SetValue(
        "__gnosisClipboardWrite",
        CefV8Value::CreateFunction("__gnosisClipboardWrite", new WriteHandler(frame, token)),
        static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY |
                                                V8_PROPERTY_ATTRIBUTE_DONTENUM));
    gsize size;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    const bool main_world = context->IsSame(frame->GetV8Context());
    CefRefPtr<CefV8Value> read = CefV8Value::CreateUndefined();
    CefRefPtr<CefV8Value> rich_read = CefV8Value::CreateUndefined();
    auto navigator = context->GetGlobal()->GetValue("navigator");
    if (navigator && navigator->IsObject()) {
      auto clipboard = navigator->GetValue("clipboard");
      if (clipboard && clipboard->IsObject()) {
        auto original = clipboard->GetValue("readText");
        auto promise = context->GetGlobal()->GetValue("Promise");
        if (original && original->IsFunction() && promise && promise->IsFunction()) {
          auto prototype = promise->GetValue("prototype");
          auto then = prototype && prototype->IsObject() ? prototype->GetValue("then") : nullptr;
          if (then && then->IsFunction()) {
            state->clipboard = clipboard; state->read_text = original; state->then = then;
            read = CefV8Value::CreateFunction("readText", new ReadHandler(state, false));
            auto rich_original = clipboard->GetValue("read");
            if (rich_original && rich_original->IsFunction()) {
              state->read = rich_original;
              rich_read = CefV8Value::CreateFunction("read", new ReadHandler(state, true));
            }
          }
        }
      }
    }
    context->Eval(std::string(data, size), "gnosis://clipboard", 1, result, exception);
    if (!exception && result && result->IsFunction() &&
        result->ExecuteFunction(nullptr, {CefV8Value::CreateBool(main_world), read, rich_read}))
      contexts_.emplace(token, state);
    g_bytes_unref(bytes);
  }
  void OnContextReleased(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefV8Context> context) override {
    std::erase_if(contexts_, [&](const auto& entry) { return entry.second->value->IsSame(context); });
  }
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    // Replacement render views can share an ID before the old proxy is destroyed.
    std::erase_if(contexts_, [&](const auto& entry) { return entry.second->browser->IsSame(browser); });
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    const bool reading = message->GetName() == read_result_message;
    if (source != PID_BROWSER || (!reading && message->GetName() != result_message))
      return false;
    auto args = message->GetArgumentList();
    if (args->GetSize() != 4 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_STRING ||
        args->GetType(2) != (reading ? VTYPE_BOOL : VTYPE_INT) ||
        args->GetType(3) != (reading ? VTYPE_STRING : VTYPE_BOOL) || !frame || !frame->IsValid()) return true;
    auto found = contexts_.find(args->GetString(0).ToString());
    if (found == contexts_.end() || !found->second->browser->IsSame(browser) ||
        found->second->frame->GetIdentifier() != frame->GetIdentifier()) return true;
    auto state = found->second;
    auto context = state->value;
    if (!context->IsValid() || !context->Enter())
      return true;
    if (reading) {
      auto pending = state->reads.find(args->GetString(1).ToString());
      if (pending != state->reads.end()) {
        auto reply = CefV8Value::CreateObject(nullptr, nullptr);
        reply->SetValue("ok", CefV8Value::CreateBool(args->GetBool(2)), V8_PROPERTY_ATTRIBUTE_NONE);
        reply->SetValue("text", CefV8Value::CreateString(args->GetString(3)), V8_PROPERTY_ATTRIBUTE_NONE);
        auto promise = pending->second;
        state->reads.erase(pending);
        promise->ResolvePromise(reply);
      }
    } else {
      auto callback = context->GetGlobal()->GetValue("__gnosisClipboardResult");
      if (callback && callback->IsFunction())
        callback->ExecuteFunction(nullptr, {CefV8Value::CreateString(args->GetString(1)),
                                          CefV8Value::CreateInt(args->GetInt(2)),
                                          CefV8Value::CreateBool(args->GetBool(3))});
    }
    context->Exit();
    return true;
  }

private:
  std::map<std::string, std::shared_ptr<Context>> contexts_;
  IMPLEMENT_REFCOUNTING(Renderer);
};
std::string member(JsonObject* obj, const char* key) {
  auto* value = json_object_get_member(obj, key);
  return value && JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING
             ? json_node_get_string(value)
             : "";
}
struct TextRead {
  CefRefPtr<Browser> browser;
  CefRefPtr<CefFrame> frame;
  CefRefPtr<CefProcessMessage> reply;
  std::string url;
  GCancellable* cancel = g_cancellable_new();
  guint timeout = 0;
  bool focused() const {
    if (browser->closing || !frame->IsValid() || !browser->clipboard_focused() ||
        frame->GetURL().ToString() != url || !browser->cef_browser()) return false;
    auto focused = browser->cef_browser()->GetFocusedFrame();
    return focused && focused->GetIdentifier() == frame->GetIdentifier();
  }
  ~TextRead() {
    if (timeout) g_source_remove(timeout);
    g_object_unref(cancel);
  }
};
CefRefPtr<CefProcessMessage> read_reply(CefRefPtr<CefProcessMessage> message) {
  auto args = message->GetArgumentList();
  const bool rich = message->GetName() == rich_read_message;
  if (args->GetSize() != (rich ? 3 : 2) || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_STRING ||
      (rich && args->GetType(2) != VTYPE_NULL && args->GetType(2) != VTYPE_LIST)) return nullptr;
  auto context = args->GetString(0).ToString(), id = args->GetString(1).ToString();
  if (!g_uuid_string_is_valid(context.c_str()) || !g_uuid_string_is_valid(id.c_str())) return nullptr;
  auto reply = CefProcessMessage::Create(read_result_message);
  auto result = reply->GetArgumentList();
  result->SetString(0, context); result->SetString(1, id);
  result->SetBool(2, false); result->SetString(3, "");
  return reply;
}
void read_rich(Browser& browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message) {
  auto reply = read_reply(message);
  if (!reply) return;
  auto paste = std::make_shared<Paste>();
  paste->browser = &browser; paste->frame = frame; paste->url = frame->GetURL();
  paste->complete = [reply](Paste& paste) {
    if (paste.browser->closing || !paste.frame->IsValid()) return;
    auto focused = paste.browser->cef_browser() ? paste.browser->cef_browser()->GetFocusedFrame() : nullptr;
    const bool allowed = Policy::navigable(paste.url) && paste.browser->clipboard_focused() &&
      paste.frame->GetURL().ToString() == paste.url && focused &&
      focused->GetIdentifier() == paste.frame->GetIdentifier();
    auto args = reply->GetArgumentList();
    args->SetBool(2, allowed && !paste.failed);
    args->SetString(3, !allowed ? "NotAllowedError" : paste.failed ? "DataError" : encode_items(paste.items));
    paste.frame->SendProcessMessage(PID_RENDERER, reply);
  };
  auto focused = browser.cef_browser() ? browser.cef_browser()->GetFocusedFrame() : nullptr;
  if (!browser.clipboard_focused() || !focused || focused->GetIdentifier() != frame->GetIdentifier() || !Policy::navigable(paste->url)) {
    paste->complete(*paste);
    return;
  }
  std::set<std::string> types;
  auto args = message->GetArgumentList();
  if (args->GetType(2) == VTYPE_LIST) {
    auto values = args->GetList(2);
    for (size_t i = 0; i < values->GetSize(); ++i) {
      if (values->GetType(i) != VTYPE_STRING) return;
      types.insert(values->GetString(i));
    }
  }
  read_formats(paste, false, true, args->GetType(2) == VTYPE_LIST ? &types : nullptr);
}
void read_text(Browser& browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message) {
  auto reply = read_reply(message);
  if (!reply) return;
  auto* read = new TextRead{&browser, frame, reply, frame->GetURL()};
  if (!read->focused() || !Policy::navigable(read->url)) {
    frame->SendProcessMessage(PID_RENDERER, reply);
    delete read;
    return;
  }
  read->timeout = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
    auto* read = static_cast<TextRead*>(data);
    read->timeout = 0;
    g_cancellable_cancel(read->cancel);
    return G_SOURCE_REMOVE;
  }, read);
  gdk_clipboard_read_text_async(gtk_widget_get_clipboard(browser.widget()), read->cancel,
    +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<TextRead> read(static_cast<TextRead*>(data));
      g_autoptr(GError) error = nullptr;
      g_autofree char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
      if (read->browser->closing || !read->frame->IsValid()) return;
      // An empty/non-text clipboard is a successful empty text read.
      const bool ok = read->focused() && !g_cancellable_is_cancelled(read->cancel) &&
        (!error || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) &&
        (!text || strlen(text) <= clipboard_limit);
      auto args = read->reply->GetArgumentList();
      args->SetBool(2, ok);
      if (ok && text) args->SetString(3, text);
      read->frame->SendProcessMessage(PID_RENDERER, read->reply);
    }, read);
}
} // namespace
CefRefPtr<CefRenderProcessHandler> Clipboard::renderer_handler() {
  return new Renderer();
}
void Clipboard::copy(Browser& browser, bool cut) {
  if (!browser.cef_browser())
    return;
  browser.activate_clipboard();
  auto frame = browser.cef_browser()->GetFocusedFrame();
  if (!frame)
    frame = browser.cef_browser()->GetMainFrame();
  if (frame) {
    if (cut)
      frame->Cut();
    else
      frame->Copy();
  }
}
bool Clipboard::receive(Browser& browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                        CefRefPtr<CefProcessMessage> message) {
  const bool rich = message->GetName() == rich_read_message;
  const bool reading = rich || message->GetName() == read_message;
  if (source != PID_RENDERER || (!reading && message->GetName() != write_message))
    return false;
  if (!frame || !frame->IsValid() || browser.closing || !browser.owner())
    return true;
  if (reading) {
    if (rich) read_rich(browser, frame, message);
    else read_text(browser, frame, message);
    return true;
  }
  auto arguments = message->GetArgumentList();
  if (arguments->GetSize() != 2 || arguments->GetType(0) != VTYPE_STRING || arguments->GetType(1) != VTYPE_STRING) return true;
  const auto context = arguments->GetString(1).ToString();
  if (context.size() != 36 || !g_uuid_string_is_valid(context.c_str())) return true;
  const auto value = arguments->GetString(0).ToString();
  if (value.size() > 96 * 1024 * 1024)
    return true;
  auto* parser = json_parser_new();
  if (!json_parser_load_from_data(parser, value.data(), value.size(), nullptr) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    g_object_unref(parser);
    return true;
  }
  auto* obj = json_node_get_object(json_parser_get_root(parser));
  const auto generation = member(obj, "generation");
  auto* id_node = json_object_get_member(obj, "id");
  const auto raw_id =
      id_node && JSON_NODE_HOLDS_VALUE(id_node) && json_node_get_value_type(id_node) == G_TYPE_INT64
          ? json_node_get_int(id_node)
          : 0;
  const int id = raw_id > 0 && raw_id <= G_MAXINT ? static_cast<int>(raw_id) : 0;
  const auto address = frame->GetURL().ToString();
  auto focused = browser.cef_browser() ? browser.cef_browser()->GetFocusedFrame() : nullptr;
  bool ok = Policy::navigable(address) && browser.clipboard_allowed() && focused &&
            focused->GetIdentifier() == frame->GetIdentifier() && id > 0 && !generation.empty();
  auto* node = json_object_get_member(obj, "items");
  std::vector<GdkContentProvider*> providers;
  size_t total = 0;
  if (!node || !JSON_NODE_HOLDS_ARRAY(node))
    ok = false;
  if (ok) {
    auto* items = json_node_get_array(node);
    ok = json_array_get_length(items) > 0 && json_array_get_length(items) <= 16;
    for (unsigned i = 0; ok && i < json_array_get_length(items); ++i) {
      auto* item_node = json_array_get_element(items, i);
      if (!JSON_NODE_HOLDS_OBJECT(item_node)) {
        ok = false;
        break;
      }
      auto* item = json_node_get_object(item_node);
      const auto mime = member(item, "type");
      if (mime != "text/plain" && mime != "text/html" && mime != "text/uri-list" && mime != "image/png" &&
          mime != "image/jpeg" && mime != "image/svg+xml" && mime != "application/x-figma") {
        ok = false;
        break;
      }
      GBytes* bytes;
      if (json_object_has_member(item, "text")) {
        const auto text = member(item, "text");
        bytes = g_bytes_new(text.data(), text.size());
      } else {
        const auto encoded = member(item, "data");
        gsize size;
        auto* data = g_base64_decode(encoded.c_str(), &size);
        bytes = g_bytes_new_take(data, size);
      }
      gsize size;
      const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
      total += size;
      if (total > clipboard_limit) {
        ok = false;
        g_bytes_unref(bytes);
        break;
      }
      if (mime == "text/plain" && g_utf8_validate(data, size, nullptr)) {
        const std::string text(data, size);
        providers.push_back(gdk_content_provider_new_typed(G_TYPE_STRING, text.c_str()));
      }
      providers.push_back(gdk_content_provider_new_for_bytes(mime.c_str(), bytes));
      g_bytes_unref(bytes);
    }
  }
  if (ok && !providers.empty()) {
    auto* provider = gdk_content_provider_new_union(providers.data(), providers.size());
    providers.clear();
    ok = gdk_clipboard_set_content(gtk_widget_get_clipboard(browser.widget()), provider);
    g_object_unref(provider);
  } else
    ok = false;
  for (auto* provider : providers)
    g_object_unref(provider);
  if (browser.owner()->app.render_test && g_getenv("GNOSIS_TEST_COPY"))
    g_print("Clipboard bridge diagnostic: request=%d success=%s bytes=%zu widget-focus=%d gesture=%d frame-focus=%d\n",
      id, ok ? "yes" : "no", total, gtk_widget_has_focus(browser.surface()), browser.clipboard_allowed(),
      focused && focused->GetIdentifier() == frame->GetIdentifier());
  auto result = CefProcessMessage::Create(result_message);
  auto args = result->GetArgumentList();
  args->SetString(0, context);
  args->SetString(1, generation);
  args->SetInt(2, id);
  args->SetBool(3, ok);
  frame->SendProcessMessage(PID_RENDERER, result);
  g_object_unref(parser);
  return true;
}
