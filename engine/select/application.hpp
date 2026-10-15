// CEF host callbacks. This module owns no browser chrome or tab widgets.
#pragma once
#include "browser.hpp"
#include "extension-host.hpp"
#include "extensions.hpp"
#include "include/cef_request_context.h"
class Application {
public:
  explicit Application(const std::string& profile);
  GApplication* gtk_app = nullptr;
  Extensions extensions;
  CefRefPtr<ExtensionHost> extension_host;
  std::string download_folder;
  bool render_test = false;
  void external(const std::string&);
  void export_completed(const std::string&, const std::string&, const std::string&);
};
class Window {
public:
  explicit Window(Application& runtime) : app(runtime) {}
  Application& app;
  GtkWindow* window = nullptr;
  bool closing = false, destroyed = false, private_mode = false;
  CefRefPtr<CefRequestContext> context;
  std::function<CefRefPtr<Browser>(const std::string&, bool, bool)> open_tab;
  std::function<void(const std::string&)> notify;
  std::function<void()> refresh, closed;
  CefRefPtr<Browser> open(const std::string& url, bool active=true, bool defer=false) {
    return open_tab ? open_tab(url, active, defer) : nullptr;
  }
  void update() { if (refresh) refresh(); }
  void browser_closed(Browser*) { if (closed) closed(); }
  void toast(const std::string& message) { if (notify) notify(message); }
};
