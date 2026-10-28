/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "webstore-guard.hpp"
#include "browser.hpp"
#include "include/cef_parser.h"
#include "include/cef_process_message.h"
#include "include/cef_task.h"

namespace {
constexpr const char* message_name = "gnosis.webstore-install-guard";
bool trusted(const std::string& url) {
  CefURLParts parts;
  if (!CefParseURL(url, parts)) return false;
  const auto port = CefString(&parts.port).ToString();
  return CefString(&parts.scheme) == "https" &&
         CefString(&parts.host) == "chromewebstore.google.com" &&
         (port.empty() || port == "443");
}
void notify(CefRefPtr<CefFrame> frame, const char* state) {
  auto message = CefProcessMessage::Create(message_name);
  message->GetArgumentList()->SetString(0, state);
  message->GetArgumentList()->SetString(1, frame->GetURL());
  frame->SendProcessMessage(PID_BROWSER, message);
}
class FailureReply final : public CefTask {
public:
  FailureReply(CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Value> callback)
    : context_(context), callback_(callback) {}
  void Execute() override {
    if (!context_->IsValid()) return;
    auto frame = context_->GetFrame();
    if (!frame || !frame->IsValid() || !frame->IsMain() ||
        !trusted(frame->GetURL()) || !context_->IsSame(frame->GetV8Context())) return;
    // Chromium 150 webstore_private.Result: feature_disabled means unavailable.
    callback_->ExecuteFunctionWithContext(context_, nullptr,
      {CefV8Value::CreateString("feature_disabled")});
  }
private:
  CefRefPtr<CefV8Context> context_;
  CefRefPtr<CefV8Value> callback_;
  IMPLEMENT_REFCOUNTING(FailureReply);
};
class DenyInstall final : public CefV8Handler {
public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& result, CefString& exception) override {
    auto context = CefV8Context::GetCurrentContext();
    auto frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsValid() || !frame->IsMain() ||
        !trusted(frame->GetURL()) || !context->IsSame(frame->GetV8Context())) {
      exception = "Invalid Web Store installation context.";
      return true;
    }
    if (args.empty() || !args[0]->IsObject() || args.size() > 2 ||
        (args.size() == 2 && !args[1]->IsFunction() && !args[1]->IsUndefined())) {
      exception = "Invalid Web Store installation arguments.";
      return true;
    }
    notify(frame, "blocked");
    if (args.size() == 2 && args[1]->IsFunction()) {
      result = CefV8Value::CreateUndefined();
      CefPostTask(TID_RENDERER, new FailureReply(context, args[1]));
    } else {
      result = CefV8Value::CreatePromise();
      result->RejectPromise("Chrome Web Store installation is not supported by Gnosis yet (feature_disabled).");
    }
    return true;
  }
private:
  IMPLEMENT_REFCOUNTING(DenyInstall);
};
}
namespace {
const char* attempt_install(CefRefPtr<CefV8Context> context) {
  auto chrome = context->GetGlobal()->GetValue("chrome");
  if (!chrome || !chrome->IsObject()) return "unavailable-chrome";
  auto api = chrome->GetValue("webstorePrivate");
  if (!api || !api->IsObject()) return "unavailable-namespace";
  auto original = api->GetValue("beginInstallWithManifest3");
  if (!original || !original->IsFunction()) return "unavailable-method";
  auto replacement = CefV8Value::CreateFunction("beginInstallWithManifest3", new DenyInstall);
  const bool set = api->SetValue("beginInstallWithManifest3", replacement,
    static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE));
  auto installed = api->GetValue("beginInstallWithManifest3");
  return set && installed && installed->IsSame(replacement) ? "installed" : "unavailable-replacement";
}
class DeferredInstall final : public CefTask {
public:
  explicit DeferredInstall(CefRefPtr<CefV8Context> context) : context_(context) {}
  void Execute() override {
    if (!context_->IsValid()) return;
    auto frame = context_->GetFrame();
    if (!frame || !frame->IsValid() || !frame->IsMain() || !trusted(frame->GetURL()) ||
        !context_->IsSame(frame->GetV8Context()) || !context_->Enter()) return;
    const char* state = attempt_install(context_);
    context_->Exit();
    if (std::string(state) == "installed" || ++attempts_ >= 100) notify(frame, state);
    else CefPostDelayedTask(TID_RENDERER, this, 20);
  }
private:
  CefRefPtr<CefV8Context> context_;
  unsigned attempts_ = 0;
  IMPLEMENT_REFCOUNTING(DeferredInstall);
};
}
bool WebstoreGuard::install(CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) {
  if (!frame || !frame->IsMain() || !trusted(frame->GetURL()) ||
      !context->IsSame(frame->GetV8Context())) return false;
  const char* state = attempt_install(context);
  if (std::string(state) == "installed") {
    notify(frame, state);
    return true;
  }
  // Chrome's bindings observer can run after CEF's context-created observer.
  // Retry briefly after it finishes; never assume deferred interception worked.
  CefPostTask(TID_RENDERER, new DeferredInstall(context));
  return false;
}
bool WebstoreGuard::receive(Browser& browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                           CefRefPtr<CefProcessMessage> message) {
  if (message->GetName() != message_name) return false;
  auto cef = browser.cef_browser();
  if (source != PID_RENDERER || browser.closing || !cef || !frame || !frame->IsValid() ||
      !frame->IsMain() || !frame->GetBrowser()->IsSame(cef)) return true;
  auto main = cef->GetMainFrame();
  if (!main || frame->GetIdentifier() != main->GetIdentifier()) return true;
  auto values = message->GetArgumentList();
  if (values->GetSize() != 2 || values->GetType(0) != VTYPE_STRING ||
      values->GetType(1) != VTYPE_STRING) return true;
  const auto url = values->GetString(1).ToString();
  if (!trusted(url) || frame->GetURL() != url || main->GetURL() != url) return true;
  const auto state = values->GetString(0).ToString();
  if (state == "blocked") {
    if (browser.on_webstore_install_blocked) browser.on_webstore_install_blocked();
  } else if (state == "installed" || state.starts_with("unavailable-")) {
    g_debug("Web Store install crash guard: %s", state.c_str());
  }
  return true;
}
