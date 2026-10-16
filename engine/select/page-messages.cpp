/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "page-messages.hpp"
#include "browser.hpp"
#include "clipboard.hpp"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

namespace {
constexpr const char* message_name = "jupiter.internal-page";
constexpr size_t payload_limit = 1024 * 1024;
bool internal_url(const std::string& url) {
  return url.starts_with("ephy-about:") ||
         (url.starts_with("data:text/html;charset=utf-8;base64,") &&
          url.find("#jupiter-alternate-") != std::string::npos);
}
class MessageHandler final : public CefV8Handler {
public:
  explicit MessageHandler(CefRefPtr<CefFrame> frame) : frame_(frame), url_(frame->GetURL()) {
    g_autofree char* token = g_uuid_string_random();
    token_ = token;
  }
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    if (args.empty() && frame_->IsValid() && frame_->GetURL() == url_) {
      activate();
      result = CefV8Value::CreateUndefined();
      return true;
    }
    if (args.size() != 2 || !args[0]->IsString() || !args[1]->IsString() ||
        !frame_->IsValid() || frame_->GetURL() != url_) {
      exception = "Invalid internal page message.";
      return true;
    }
    auto name = args[0]->GetStringValue();
    auto payload = args[1]->GetStringValue();
    if (name.length() > 128 || payload.length() > payload_limit) {
      exception = "Internal page message is too large.";
      return true;
    }
    auto message = CefProcessMessage::Create(message_name);
    auto values = message->GetArgumentList();
    values->SetString(0, token_);
    values->SetString(1, url_);
    values->SetString(2, name);
    values->SetString(3, payload);
    frame_->SendProcessMessage(PID_BROWSER, message);
    result = CefV8Value::CreateUndefined();
    return true;
  }
  void activate() {
    auto message = CefProcessMessage::Create(message_name);
    message->GetArgumentList()->SetString(0, token_);
    message->GetArgumentList()->SetString(1, url_);
    frame_->SendProcessMessage(PID_BROWSER, message);
  }
private:
  CefRefPtr<CefFrame> frame_;
  std::string url_, token_;
  IMPLEMENT_REFCOUNTING(MessageHandler);
};
class Renderer final : public CefRenderProcessHandler {
public:
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    clipboard_->OnContextCreated(browser, frame, context);
    // CEF also calls this hook for extension and named isolated worlds.
    if (!frame->IsMain() || !context->IsSame(frame->GetV8Context()) ||
        !internal_url(frame->GetURL())) return;
    CefRefPtr<MessageHandler> handler = new MessageHandler(frame);
    context->GetGlobal()->SetValue("__jupiterInternalMessage",
      CefV8Value::CreateFunction("__jupiterInternalMessage", handler),
      static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM));
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    context->Eval(R"JS((() => {
      const send = globalThis.__jupiterInternalMessage;
      const stringify = JSON.stringify;
      delete globalThis.__jupiterInternalMessage;
      Object.defineProperty(globalThis, 'Jupiter', {value: Object.freeze({
        postMessage(name, value) { send(name, stringify(value)); }
      })});
      addEventListener('pageshow', () => send());
    })())JS", "jupiter://internal-page-messages", 1, result, exception);
    if (!exception) handler->activate();
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source, CefRefPtr<CefProcessMessage> message) override {
    return clipboard_->OnProcessMessageReceived(browser, frame, source, message);
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    clipboard_->OnContextReleased(browser, frame, context);
  }
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    clipboard_->OnBrowserDestroyed(browser);
  }
private:
  CefRefPtr<CefRenderProcessHandler> clipboard_ = Clipboard::renderer_handler();
  IMPLEMENT_REFCOUNTING(Renderer);
};
}
CefRefPtr<CefRenderProcessHandler> PageMessages::renderer_handler() { return new Renderer; }
bool PageMessages::receive(Browser& browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                          CefRefPtr<CefProcessMessage> message) {
  if (source != PID_RENDERER || message->GetName() != message_name) return false;
  auto cef = browser.cef_browser();
  if (browser.closing || !cef || !frame || !frame->IsValid() || !frame->IsMain() ||
      frame->GetIdentifier() != cef->GetMainFrame()->GetIdentifier()) return true;
  auto values = message->GetArgumentList();
  if ((values->GetSize() != 2 && values->GetSize() != 4) || values->GetType(0) != VTYPE_STRING ||
      values->GetType(1) != VTYPE_STRING) return true;
  auto token = values->GetString(0).ToString();
  auto url = values->GetString(1).ToString();
  if (!g_uuid_string_is_valid(token.c_str()) || !internal_url(url) || frame->GetURL() != url) return true;
  if (values->GetSize() == 2) {
    browser.page_message_context = token;
    return true;
  }
  if (token != browser.page_message_context || values->GetType(2) != VTYPE_STRING ||
      values->GetType(3) != VTYPE_STRING) return true;
  auto name = values->GetString(2).ToString();
  auto payload = values->GetString(3).ToString();
  auto callback = browser.on_page_message;
  if (name.size() <= 128 && payload.size() <= payload_limit && callback)
    callback(url, name, payload);
  return true;
}
