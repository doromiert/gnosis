/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "page-messages.hpp"
#include "browser.hpp"
#include "clipboard.hpp"
#include "webstore-guard.hpp"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include <json-glib/json-glib.h>
#include <map>
#include <memory>

namespace {
constexpr const char* message_name = "gnosis.internal-page";
constexpr const char* event_name = "gnosis.internal-page.event";
constexpr const char* release_name = "gnosis.internal-page.release";
constexpr size_t payload_limit = 1024 * 1024;
bool internal_url(const std::string& url) {
  return url.starts_with("ephy-about:") ||
         (url.starts_with("data:text/html;charset=utf-8;base64,") &&
          url.find("#gnosis-alternate-") != std::string::npos);
}
std::string document_key(const std::string& token, const std::string& url, CefRefPtr<CefFrame> frame) {
  return token + "\n" + frame->GetIdentifier().ToString() + "\n" + url;
}
struct Context {
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefFrame> frame;
  CefRefPtr<CefV8Context> value;
  CefRefPtr<CefV8Value> dispatch;
  std::string url, token;
  bool live() const {
    return value->IsValid() && frame->IsValid() && frame->IsMain() &&
           frame->GetBrowser()->IsSame(browser) &&
           frame->GetIdentifier() == browser->GetMainFrame()->GetIdentifier() &&
           frame->GetURL() == url && value->IsSame(frame->GetV8Context());
  }
  void activate() const {
    auto message = CefProcessMessage::Create(message_name);
    message->GetArgumentList()->SetString(0, token);
    message->GetArgumentList()->SetString(1, url);
    frame->SendProcessMessage(PID_BROWSER, message);
  }
};
class MessageHandler final : public CefV8Handler {
public:
  explicit MessageHandler(std::weak_ptr<Context> context) : context_(std::move(context)) {}
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    auto context = context_.lock();
    auto current = CefV8Context::GetCurrentContext();
    if (!context || !context->live() || !current || !current->IsSame(context->value)) {
      exception = "Invalid internal page context.";
      return true;
    }
    if (args.empty()) {
      context->activate();
      result = CefV8Value::CreateUndefined();
      return true;
    }
    if (args.size() != 2 || !args[0]->IsString() || !args[1]->IsString()) {
      exception = "Invalid internal page message.";
      return true;
    }
    auto name = args[0]->GetStringValue().ToString();
    auto payload = args[1]->GetStringValue().ToString();
    if (name.size() > 128 || payload.size() > payload_limit) {
      exception = "Internal page message is too large.";
      return true;
    }
    auto message = CefProcessMessage::Create(message_name);
    auto values = message->GetArgumentList();
    values->SetString(0, context->token);
    values->SetString(1, context->url);
    values->SetString(2, name);
    values->SetString(3, payload);
    context->frame->SendProcessMessage(PID_BROWSER, message);
    result = CefV8Value::CreateUndefined();
    return true;
  }
private:
  std::weak_ptr<Context> context_;
  IMPLEMENT_REFCOUNTING(MessageHandler);
};
class Renderer final : public CefRenderProcessHandler {
public:
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    clipboard_->OnContextCreated(browser, frame, context);
    WebstoreGuard::install(frame, context);
    // CEF also calls this hook for extension and named isolated worlds.
    if (!frame->IsMain() || !context->IsSame(frame->GetV8Context()) ||
        !internal_url(frame->GetURL())) return;
    auto state = std::make_shared<Context>();
    state->browser = browser; state->frame = frame; state->value = context; state->url = frame->GetURL();
    g_autofree char* token = g_uuid_string_random();
    state->token = token;
    context->GetGlobal()->SetValue("__gnosisInternalMessage",
      CefV8Value::CreateFunction("__gnosisInternalMessage", new MessageHandler(state)),
      static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM));
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    context->Eval(R"JS((() => {
      const send = globalThis.__gnosisInternalMessage;
      const stringify = JSON.stringify;
      const parse = JSON.parse;
      const CustomEventClass = CustomEvent;
      const events = new EventTarget();
      const dispatch = events.dispatchEvent.bind(events);
      delete globalThis.__gnosisInternalMessage;
      Object.defineProperty(globalThis, 'Gnosis', {value: Object.freeze({
        postMessage(name, value) { send(name, stringify(value)); },
        addEventListener: events.addEventListener.bind(events),
        removeEventListener: events.removeEventListener.bind(events)
      })});
      addEventListener('pageshow', () => send());
      return (name, json) => dispatch(new CustomEventClass(name, {detail: parse(json)}));
    })())JS", "gnosis://internal-page-messages", 1, result, exception);
    if (!exception && result && result->IsFunction()) {
      state->dispatch = result;
      contexts_.emplace(state->token, state);
      state->activate();
    }
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source, CefRefPtr<CefProcessMessage> message) override {
    if (source != PID_BROWSER || message->GetName() != event_name)
      return clipboard_->OnProcessMessageReceived(browser, frame, source, message);
    auto values = message->GetArgumentList();
    if (values->GetSize() != 4 || !frame || !frame->IsValid() || !frame->IsMain()) return true;
    for (size_t i = 0; i < 4; ++i)
      if (values->GetType(i) != VTYPE_STRING) return true;
    auto found = contexts_.find(values->GetString(0).ToString());
    if (found == contexts_.end()) return true;
    auto state = found->second;
    auto name = values->GetString(2).ToString();
    auto payload = values->GetString(3).ToString();
    if (!state->browser->IsSame(browser) || !frame->GetBrowser()->IsSame(browser) ||
        state->frame->GetIdentifier() != frame->GetIdentifier() ||
        state->url != values->GetString(1).ToString() || !state->live() ||
        name.size() > 128 || payload.size() > payload_limit || !state->value->Enter()) return true;
    state->dispatch->ExecuteFunction(nullptr, {CefV8Value::CreateString(name), CefV8Value::CreateString(payload)});
    state->value->Exit();
    return true;
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    clipboard_->OnContextReleased(browser, frame, context);
    std::erase_if(contexts_, [&](const auto& entry) {
      auto state = entry.second;
      if (!state->browser->IsSame(browser) || !state->value->IsSame(context)) return false;
      if (state->frame->IsValid()) {
        auto message = CefProcessMessage::Create(release_name);
        message->GetArgumentList()->SetString(0, state->token);
        message->GetArgumentList()->SetString(1, state->url);
        state->frame->SendProcessMessage(PID_BROWSER, message);
      }
      return true;
    });
  }
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    clipboard_->OnBrowserDestroyed(browser);
    std::erase_if(contexts_, [&](const auto& entry) { return entry.second->browser->IsSame(browser); });
  }
private:
  std::map<std::string, std::shared_ptr<Context>> contexts_;
  CefRefPtr<CefRenderProcessHandler> clipboard_ = Clipboard::renderer_handler();
  IMPLEMENT_REFCOUNTING(Renderer);
};
}
CefRefPtr<CefRenderProcessHandler> PageMessages::renderer_handler() { return new Renderer; }
bool PageMessages::receive(Browser& browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                          CefRefPtr<CefProcessMessage> message) {
  if (WebstoreGuard::receive(browser, frame, source, message)) return true;
  const bool released = message->GetName() == release_name;
  if (source != PID_RENDERER || (!released && message->GetName() != message_name)) return false;
  auto cef = browser.cef_browser();
  if (browser.closing || !cef || !frame || !frame->IsValid() || !frame->IsMain() ||
      !frame->GetBrowser()->IsSame(cef) ||
      frame->GetIdentifier() != cef->GetMainFrame()->GetIdentifier()) return true;
  auto values = message->GetArgumentList();
  if ((values->GetSize() != 2 && (released || values->GetSize() != 4)) || values->GetType(0) != VTYPE_STRING ||
      values->GetType(1) != VTYPE_STRING) return true;
  auto token = values->GetString(0).ToString();
  auto url = values->GetString(1).ToString();
  if (!g_uuid_string_is_valid(token.c_str()) || !internal_url(url)) return true;
  auto key = document_key(token, url, frame);
  if (released) {
    if (browser.page_message_context == key) browser.page_message_context.clear();
    return true;
  }
  if (frame->GetURL() != url) return true;
  if (values->GetSize() == 2) {
    browser.page_message_context = key;
    return true;
  }
  if (key != browser.page_message_context || values->GetType(2) != VTYPE_STRING ||
      values->GetType(3) != VTYPE_STRING) return true;
  auto name = values->GetString(2).ToString();
  auto payload = values->GetString(3).ToString();
  auto callback = browser.on_page_message;
  if (name.size() <= 128 && payload.size() <= payload_limit && callback)
    callback(url, name, payload);
  return true;
}
bool PageMessages::send(Browser& browser, const std::string& expected_uri,
                        const std::string& name, const std::string& json) {
  auto cef = browser.cef_browser();
  if (browser.closing || !cef || !internal_url(expected_uri) || name.size() > 128 ||
      json.size() > payload_limit || browser.page_message_context.size() < 37) return false;
  auto frame = cef->GetMainFrame();
  auto token = browser.page_message_context.substr(0, 36);
  if (!frame || !frame->IsValid() || !frame->IsMain() || !frame->GetBrowser()->IsSame(cef) ||
      frame->GetURL() != expected_uri || !g_uuid_string_is_valid(token.c_str()) ||
      browser.page_message_context != document_key(token, expected_uri, frame)) return false;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json.data(), json.size(), nullptr)) return false;
  auto message = CefProcessMessage::Create(event_name);
  auto values = message->GetArgumentList();
  values->SetString(0, token);
  values->SetString(1, expected_uri);
  values->SetString(2, name);
  values->SetString(3, json);
  frame->SendProcessMessage(PID_RENDERER, message);
  return true;
}
