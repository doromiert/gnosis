#pragma once
#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_registration.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
struct ExtensionTab {
  int id;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefDictionaryValue> info;
};
// Chromium's extension runtime handles scripts, storage, messaging and requests.
// Alloy web contents need an adapter for APIs tied to Chrome's native tab strip.
class ExtensionHost final : public CefDevToolsMessageObserver {
public:
  using Result = std::function<void(CefRefPtr<CefDictionaryValue>)>;
  std::function<std::vector<ExtensionTab>()> tabs;
  std::function<void(int, const std::string&, bool)> update;
  std::set<std::string> extension_ids;
  std::string tab_bridge_id;
  std::map<std::string, std::vector<std::string>> permissions;
  std::map<std::string, int> active_grants;
  std::map<std::string, std::string> extension_paths;
  void attach(CefRefPtr<CefBrowser>);
  void detach(int);
  void stop();
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int, bool, const void*, size_t) override;
  void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString&, const void*, size_t) override;

private:
  struct Origin {
    std::string session;
    CefRefPtr<CefBrowser> browser;
    int context = 0;
    std::string extension;
  };
  struct Page {
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefRegistration> registration;
  };
  struct Waiting {
    Origin origin;
    CefRefPtr<CefDictionaryValue> request;
    long long deadline;
  };
  std::vector<Waiting> waiting_;
  struct Context {
    int id;
    std::string extension, frame;
  };
  std::map<int, std::map<int, Context>> contexts_;
  std::map<std::pair<int, int>, std::string> frame_ids_;
  std::map<int, Page> pages_;
  std::map<std::pair<int, int>, std::string> trusted_contexts_;
  std::map<int, Result> pending_;
  std::map<std::string, Origin> sessions_;
  std::set<std::string> targets_;
  std::string context_id_;
  std::map<std::string, std::vector<std::string>> styles_;
  unsigned timer_ = 0;
  int sequence_ = 1;
  void command(CefRefPtr<CefBrowser>, const std::string&, CefRefPtr<CefDictionaryValue>, Result = {});
  void send(const Origin&, const std::string&, CefRefPtr<CefDictionaryValue>, Result = {});
  void scan();
  void hook(const Origin&);
  void request(const Origin&, CefRefPtr<CefDictionaryValue>, bool defer = true);
  void reply(const Origin&, int, CefRefPtr<CefValue>, const std::string& error = "");
  bool allowed(const std::string&, const std::string&, int) const;
  void script(const ExtensionTab&, CefRefPtr<CefDictionaryValue>, const std::string&,
              std::function<void(CefRefPtr<CefValue>, const std::string&)>);
  void css(const ExtensionTab&, CefRefPtr<CefDictionaryValue>, bool, const std::string&,
           std::function<void()>);
  IMPLEMENT_REFCOUNTING(ExtensionHost);
};
