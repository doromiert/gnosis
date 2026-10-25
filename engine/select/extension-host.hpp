#pragma once
#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_registration.h"
#include <functional>
#include <map>
#include <memory>
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
  std::function<std::vector<ExtensionTab>(CefRefPtr<CefBrowser>)> tabs;
  using Created = std::function<void(CefRefPtr<CefDictionaryValue>, const std::string&)>;
  std::function<void(CefRefPtr<CefBrowser>, const std::string&, const std::string&, bool, Created)> create;
  std::function<void(int, const std::string&, bool)> update;
  std::set<std::string> extension_ids;
  std::string tab_bridge_id;
  std::map<std::string, std::vector<std::string>> permissions;
  std::map<std::string, std::string> extension_paths;
  void attach(CefRefPtr<CefBrowser>);
  void detach(int);
  void stop();
  void set_enabled(const std::string& id, bool enabled);
  void prepare_popup(const std::string& extension, std::function<bool()> live,
                     std::function<void(const std::string&)> done);
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int, bool, const void*, size_t) override;
  void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString&, const void*, size_t) override;
  void OnDevToolsAgentDetached(CefRefPtr<CefBrowser>) override;

private:
  friend class ExtensionHostTestPeer;
  struct Origin {
    std::string session;
    CefRefPtr<CefBrowser> browser;
    int context = 0;
    std::string extension;
    uint64_t generation = 0;
    std::string target;
  };
  struct Page {
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefRegistration> registration;
    uint64_t generation;
  };
  struct Waiting {
    Origin origin;
    CefRefPtr<CefDictionaryValue> request;
    long long deadline;
    int destination = 0;
  };
  std::vector<Waiting> waiting_;
  struct PopupPreparation {
    std::string extension;
    std::function<bool()> live;
    std::function<void(const std::string&)> done;
    long long deadline;
    bool pending = false;
  };
  std::vector<std::shared_ptr<PopupPreparation>> popup_preparations_;
  void poll_popup_preparations();
  struct Context {
    int id;
    std::string extension, frame;
  };
  std::map<int, std::map<int, Context>> contexts_;
  std::map<std::pair<int, int>, std::string> frame_ids_;
  std::map<int, Page> pages_;
  std::map<std::pair<int, int>, std::string> trusted_contexts_;
  struct Pending {
    int browser;
    uint64_t generation;
    std::string session;
    Result callback;
    bool nested = false;
  };
  std::map<int, Pending> pending_;
  std::map<std::string, Origin> sessions_;
  std::set<std::string> targets_;
  std::string context_id_;
  std::map<std::string, std::vector<std::string>> styles_;
  unsigned timer_ = 0;
  uint64_t next_generation_ = 0;
  bool current(const Origin&) const;
  void complete(int, CefRefPtr<CefDictionaryValue>);
  void fail(int, const std::string&);
  void command(CefRefPtr<CefBrowser>, const std::string&, CefRefPtr<CefDictionaryValue>, Result = {},
               const std::string& session = "");
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
