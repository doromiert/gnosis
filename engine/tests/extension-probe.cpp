#include "diagnostic-process.hpp"
#include "extension-host.hpp"
#include "extension-runtime.hpp"
#include "extension-tab-id.hpp"
#include "extensions.hpp"
#include <glib.h>
static bool finished = false;
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_task.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <set>
#include <unistd.h>
class Client final : public CefClient,
                     public CefRenderHandler,
                     public CefLifeSpanHandler,
                     public CefDisplayHandler,
                     public CefLoadHandler {
public:
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, int) override {
    if (f->IsMain() && f->GetURL().ToString().starts_with("chrome-extension://") &&
        getenv("JUPITER_PROBE_SCRIPT")) {
      std::ifstream file(getenv("JUPITER_PROBE_SCRIPT"));
      std::string js((std::istreambuf_iterator<char>(file)), {});
      f->ExecuteJavaScript(js, f->GetURL(), 1);
    }
  }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& r) override { r = CefRect(0, 0, 800, 600); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
    browser = b;
    printf("Created Alloy browser %d\n", b->GetIdentifier());
    host = new ExtensionHost;
    std::string path = getenv("JUPITER_PROBE_EXTENSION");
    path = path.substr(0, path.find(','));
    auto extension = Extensions::read_manifest(path);
    auto ext = extension.id;
    std::string paths = getenv("JUPITER_PROBE_EXTENSION");
    auto split = paths.find(',');
    if (split != std::string::npos)
      host->tab_bridge_id = Extensions::id_for_path(paths.substr(split + 1));
    host->extension_ids.insert(ext);
    host->permissions[ext] = extension.permissions;
    host->extension_paths[ext] = path;
    host->tabs = [this] {
      std::vector<ExtensionTab> all;
      if (browser && native_id >= 0) {
        auto info = CefDictionaryValue::Create();
        info->SetInt("id", native_id);
        info->SetBool("active", true);
        info->SetString("url", browser->GetMainFrame()->GetURL());
        all.push_back({native_id, browser, info});
      }
      return all;
    };
    host->attach(b);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    if (host)
      host->stop();
    browser = nullptr;
    finished = true;
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t severity, const CefString& m, const CefString& source,
                        int line) override {
    if (m.ToString().starts_with("JUPITER_NATIVE_TAB_ID:"))
      native_id = atoi(m.ToString().substr(sizeof("JUPITER_NATIVE_TAB_ID:") - 1).c_str());
    printf("%s\n", m.ToString().c_str());
    if (severity >= LOGSEVERITY_ERROR)
      printf("Extension console source: %s:%d\n", source.ToString().c_str(), line);
    fflush(stdout);
    const std::set<std::string> expected = getenv("JUPITER_PROBE_SCRIPT") ?
      std::set<std::string>{"UBO_NETWORK", "UBO_COSMETIC", "UBO_SCRIPTLET", "UBO_FRAME"} :
      std::set<std::string>{"EXTENSION_RUNTIME", "EXTENSION_BLOCKING", "EXTENSION_HOST_ISOLATION"};
    for (const auto& marker : expected) {
      if (m == marker + ":FAIL") {
        if (browser) browser->GetHost()->CloseBrowser(true);
      } else if (m == marker + ":PASS") passed.insert(marker);
    }
    if (passed.size() == expected.size() && browser) browser->GetHost()->CloseBrowser(true);
    return true;
  }
  CefRefPtr<CefBrowser> browser;
  int native_id = -1;
  CefRefPtr<ExtensionHost> host;
  std::set<std::string> passed;

private:
  IMPLEMENT_REFCOUNTING(Client);
};
class Exit final : public CefTask {
public:
  explicit Exit(CefRefPtr<Client> c) : client(c) {}
  void Execute() override {
    if (client->browser) {
      fprintf(stderr, "Extension probe timed out waiting for its result markers\n");
      client->browser->GetHost()->CloseBrowser(true);
    }
  }

private:
  CefRefPtr<Client> client;
  IMPLEMENT_REFCOUNTING(Exit);
};
class Reload final : public CefTask {
public:
  explicit Reload(CefRefPtr<Client> c) : client(c) {}
  void Execute() override {
    if (client->browser)
      client->browser->GetMainFrame()->LoadURL(getenv("JUPITER_PROBE_URL"));
  }

private:
  CefRefPtr<Client> client;
  IMPLEMENT_REFCOUNTING(Reload);
};
class App final : public CefApp, public CefBrowserProcessHandler {
public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnBeforeCommandLineProcessing(const CefString& process, CefRefPtr<CefCommandLine> c) override {
    if (process.empty()) {
      configure_extensions(c, getenv("JUPITER_PROBE_EXTENSION"));
      c->AppendSwitch("enable-unsafe-extension-debugging");
      c->AppendSwitch("disable-gpu");
      c->AppendSwitchWithValue("ozone-platform", "x11");
      c->AppendSwitch("enable-logging");
      c->AppendSwitchWithValue("log-level", "0");
    }
  }
  void OnContextInitialized() override {
    printf("EXTENSION_DEVELOPER_MODE:%d\n", enable_extension_developer_mode());
    CefWindowInfo w;
    w.SetAsWindowless(0);
    CefBrowserSettings s;
    client = new Client;
    CefBrowserHost::CreateBrowser(w, client, getenv("JUPITER_PROBE_URL"), s, nullptr, nullptr);
    if (getenv("JUPITER_PROBE_SCRIPT"))
      CefPostDelayedTask(TID_UI, new Reload(client), 8000);
    CefPostDelayedTask(TID_UI, new Exit(client), 35000);
  }
  CefRefPtr<Client> client;

private:
  IMPLEMENT_REFCOUNTING(App);
};
int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  CefMainArgs args(argc, argv);
  CefRefPtr<App> app = new App;
  int sub = CefExecuteProcess(args, app, nullptr);
  if (sub >= 0)
    return sub;
  CefSettings s;
  s.external_message_pump = true;
  s.no_sandbox = true;
  s.windowless_rendering_enabled = true;
  std::string sdk = getenv("CEF_ROOT");
  CefString(&s.resources_dir_path) = sdk + "/Resources";
  CefString(&s.locales_dir_path) = sdk + "/Resources/locales";
  char profile[] = "/tmp/jupiter-extension-probe-XXXXXX";
  if (!mkdtemp(profile))
    return 3;
  CefString(&s.root_cache_path) = profile;
  CefString(&s.cache_path) = std::string(profile) + "/profile";
  CefString(&s.log_file) = std::string(profile) + "/cef.log";
  if (!CefInitialize(args, s, app, nullptr))
    return 2;
  disable_diagnostic_core_dumps();
  while (!finished) {
    CefDoMessageLoopWork();
    while (g_main_context_iteration(nullptr, false)) {
    }
    usleep(1000);
  }
  app->client = nullptr;
  CefShutdown();
  return 0;
}
