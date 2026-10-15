#include "clipboard.hpp"
#include "application.hpp"
#include "browser.hpp"
#include "policy.hpp"

#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include <cstring>
#include <json-glib/json-glib.h>
#include <memory>
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
  bool plain = false;
};
using SharedPaste = std::shared_ptr<Paste>;
void finished(const SharedPaste& paste) {
  if (--paste->pending)
    return;
  if (paste->browser->closing || !paste->frame->IsValid() || paste->frame->GetURL().ToString() != paste->url)
    return;
  if (paste->items.empty()) {
    if (auto* window = paste->browser->owner())
      window->toast("No supported content in the clipboard");
    return;
  }
  JsonArray* array = json_array_new();
  for (const auto& item : paste->items) {
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
  char* encoded = json_generator_to_data(generator, nullptr);
  // Dispatch web clipboard data for web editors' object/image handlers. Editable fields
  // also need the default insertion, which synthetic ClipboardEvents do not perform.
  const std::string script = std::string(R"JS((() => {
    const items = )JS") + encoded +
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
  paste->frame->ExecuteJavaScript(script, "jupiter://clipboard", 1);
  g_free(encoded);
  g_object_unref(generator);
  json_node_free(node);
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
    finished(read->paste);
    delete read;
    return;
  }
  gsize size;
  const auto* buffer = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  if (size && read->bytes.size() + size <= 64 * 1024 * 1024) {
    read->bytes.append(buffer, size);
    g_bytes_unref(bytes);
    read_next(read);
    return;
  }
  if (!size)
    read->paste->items.push_back({read->mime, std::move(read->bytes)});
  g_bytes_unref(bytes);
  finished(read->paste);
  delete read;
}
void read_next(Read* read) {
  g_input_stream_read_bytes_async(read->stream, 65536, G_PRIORITY_DEFAULT, nullptr, read_chunk, read);
}
} // namespace
void Clipboard::paste(Browser& browser, bool plain_text) {
  if (!browser.cef_browser())
    return;
  auto paste = std::make_shared<Paste>();
  paste->browser = &browser;
  paste->frame = browser.cef_browser()->GetFocusedFrame();
  if (!paste->frame)
    paste->frame = browser.cef_browser()->GetMainFrame();
  if (!paste->frame || !paste->frame->IsValid())
    return;
  paste->url = paste->frame->GetURL();
  paste->plain = plain_text;
  GdkClipboard* clipboard = gtk_widget_get_clipboard(browser.widget());
  GdkContentFormats* formats = gdk_clipboard_get_formats(clipboard);
  std::vector<std::string> mimes;
  if (!plain_text)
    for (const char* mime : {"text/html", "image/png", "image/jpeg", "image/svg+xml", "application/x-figma"})
      if (gdk_content_formats_contain_mime_type(formats, mime))
        mimes.emplace_back(mime);
  const bool text = gdk_content_formats_contain_gtype(formats, G_TYPE_STRING) ||
                    gdk_content_formats_contain_mime_type(formats, "text/plain") ||
                    gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8");
  paste->pending = mimes.size() + (text ? 1 : 0);
  if (!paste->pending) {
    if (auto* window = browser.owner())
      window->toast("No supported content in the clipboard");
    return;
  }
  if (text) {
    gdk_clipboard_read_text_async(
        clipboard, nullptr,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
          auto paste = *static_cast<SharedPaste*>(data);
          delete static_cast<SharedPaste*>(data);
          char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(object), result, nullptr);
          if (text) {
            paste->items.push_back({"text/plain", text});
            g_free(text);
          }
          finished(paste);
        },
        new SharedPaste(paste));
  }
  for (const auto& mime : mimes) {
    auto* read = new Read();
    read->paste = paste;
    read->mime = mime;
    read->requested[0] = read->mime.c_str();
    gdk_clipboard_read_async(
        clipboard, read->requested, G_PRIORITY_DEFAULT, nullptr,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
          auto* read = static_cast<Read*>(data);
          const char* mime = nullptr;
          read->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(object), result, &mime, nullptr);
          if (!read->stream) {
            finished(read->paste);
            delete read;
            return;
          }
          read_next(read);
        },
        read);
  }
}

namespace {
constexpr const char* write_message = "jupiter.clipboard.write";
constexpr const char* result_message = "jupiter.clipboard.result";
constexpr size_t clipboard_limit = 64 * 1024 * 1024;
class WriteHandler final : public CefV8Handler {
public:
  explicit WriteHandler(CefRefPtr<CefFrame> frame) : frame_(frame) {}
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
    frame_->SendProcessMessage(PID_BROWSER, message);
    result = CefV8Value::CreateUndefined();
    return true;
  }

private:
  CefRefPtr<CefFrame> frame_;
  IMPLEMENT_REFCOUNTING(WriteHandler);
};
class Renderer final : public CefRenderProcessHandler {
public:
  void OnContextCreated(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    auto* bytes = g_resources_lookup_data("/io/github/doromiert/Jupiter/clipboard/bridge.js",
                                          G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
      return;
    context->GetGlobal()->SetValue(
        "__jupiterClipboardWrite",
        CefV8Value::CreateFunction("__jupiterClipboardWrite", new WriteHandler(frame)),
        static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY |
                                                V8_PROPERTY_ATTRIBUTE_DONTENUM));
    gsize size;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    context->Eval(std::string(data, size), "jupiter://clipboard", 1, result, exception);
    g_bytes_unref(bytes);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source != PID_BROWSER || message->GetName() != result_message)
      return false;
    auto context = frame->GetV8Context();
    if (!context || !context->Enter())
      return true;
    auto callback = context->GetGlobal()->GetValue("__jupiterClipboardResult");
    auto args = message->GetArgumentList();
    if (callback && callback->IsFunction())
      callback->ExecuteFunction(nullptr, {CefV8Value::CreateString(args->GetString(0)),
                                          CefV8Value::CreateInt(args->GetInt(1)),
                                          CefV8Value::CreateBool(args->GetBool(2))});
    context->Exit();
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(Renderer);
};
std::string member(JsonObject* obj, const char* key) {
  auto* value = json_object_get_member(obj, key);
  return value && JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING
             ? json_node_get_string(value)
             : "";
}
} // namespace
CefRefPtr<CefRenderProcessHandler> Clipboard::renderer_handler() {
  return new Renderer();
}
void Clipboard::copy(Browser& browser, bool cut) {
  if (!browser.cef_browser())
    return;
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
  if (source != PID_RENDERER || message->GetName() != write_message)
    return false;
  if (!frame || !frame->IsValid() || browser.closing || !browser.owner())
    return true;
  const auto value = message->GetArgumentList()->GetString(0).ToString();
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
  const int id =
      id_node && JSON_NODE_HOLDS_VALUE(id_node) && json_node_get_value_type(id_node) == G_TYPE_INT64
          ? static_cast<int>(json_node_get_int(id_node))
          : 0;
  const auto address = frame->GetURL().ToString();
  bool ok = (Policy::navigable(address) && browser.clipboard_allowed()) && id > 0 && !generation.empty();
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
  if (browser.owner()->app.render_test && g_getenv("JUPITER_TEST_COPY"))
    g_print("Clipboard bridge diagnostic: request=%d success=%s bytes=%zu\n", id, ok ? "yes" : "no", total);
  auto result = CefProcessMessage::Create(result_message);
  auto args = result->GetArgumentList();
  args->SetString(0, generation);
  args->SetInt(1, id);
  args->SetBool(2, ok);
  frame->SendProcessMessage(PID_RENDERER, result);
  g_object_unref(parser);
  return true;
}
