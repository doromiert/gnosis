#include "diagnostic-process.hpp"
#include "extension-host.hpp"
#include "extension-runtime.hpp"
#include "extension-tab-id.hpp"
#include "extensions.hpp"
#include <glib.h>
static bool finished = false;
static int browsers = 0;
static int creating = 0;
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_task.h"
#include "include/cef_parser.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <set>
#include <unistd.h>
// Only the regression peer can address the real background session directly.
class ExtensionHostTestPeer {
public:
  static bool background(CefRefPtr<ExtensionHost> host, const std::string& extension,
                         const std::string& expression, ExtensionHost::Result done) {
    for (const auto& [id, origin] : host->sessions_) {
      if (origin.extension != extension || !host->current(origin)) continue;
      auto args = CefDictionaryValue::Create();
      args->SetString("expression", expression);
      args->SetBool("awaitPromise", true);
      args->SetBool("returnByValue", true);
      host->send(origin, "Runtime.evaluate", args, std::move(done));
      return true;
    }
    return false;
  }
  static void failures(CefRefPtr<ExtensionHost> host, CefRefPtr<CefBrowser> browser,
                       std::function<void(bool)> done) {
    struct Counts { int invalid = 0, stale = 0, detached = 0, transport = 0, inner = 0; bool ok = true; };
    auto counts = std::make_shared<Counts>();
    auto args = CefDictionaryValue::Create(); args->SetString("expression", "42");
    // Handler/ownership coverage only; this does not claim a real Chromium agent crash or detach.
    CefRefPtr<ExtensionHost> detached = new ExtensionHost;
    detached->attach(browser);
    detached->command(browser, "Runtime.evaluate", args, [counts](auto result) {
      counts->detached++; counts->ok &= result->GetBool("gnosisError");
    });
    detached->OnDevToolsAgentDetached(browser);
    detached->OnDevToolsAgentDetached(browser);
    host->command(nullptr, "Runtime.evaluate", args, [counts](auto result) {
      counts->invalid++; counts->ok &= result->GetBool("gnosisError");
    });
    ExtensionHost::Origin stale{"", browser, 0, "", host->pages_.at(browser->GetIdentifier()).generation+1};
    host->send(stale, "Runtime.evaluate", args, [counts](auto result) {
      counts->stale++; counts->ok &= result->GetBool("gnosisError");
    });
    ExtensionHost::Origin missing{"gnosis-regression-nonexistent-session", browser, 0, "",
      host->pages_.at(browser->GetIdentifier()).generation};
    host->send(missing, "Runtime.evaluate", args, [counts](auto result) {
      counts->invalid++; counts->ok &= result->GetBool("gnosisError");
    });
    // The host owns this session, but Chromium does not: exercise an actual outer CDP rejection.
    host->sessions_[missing.session] = missing;
    host->send(missing, "Runtime.evaluate", args, [host, browser, counts, missing, done](auto result) {
      counts->transport++; counts->ok &= result->GetBool("gnosisError");
      auto late = CefDictionaryValue::Create(); late->SetInt("id", counts->inner);
      late->SetDictionary("result", CefDictionaryValue::Create());
      auto value = CefValue::Create(); value->SetDictionary(late);
      auto event = CefDictionaryValue::Create(); event->SetString("sessionId", missing.session);
      event->SetString("message", CefWriteJSON(value, JSON_WRITER_DEFAULT));
      value = CefValue::Create(); value->SetDictionary(event);
      auto json = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
      host->OnDevToolsEvent(browser, "Target.receivedMessageFromTarget", json.data(), json.size());
      host->sessions_.erase(missing.session);
      done(counts->ok && counts->invalid == 2 && counts->stale == 1 &&
        counts->detached == 1 && counts->transport == 1 && counts->inner > 0);
    });
    for (const auto& [id, pending] : host->pending_)
      if (pending.nested && pending.session == missing.session) counts->inner = id;
  }
  static std::shared_ptr<std::map<int, int>> watch_destination(CefRefPtr<ExtensionHost> host, int browser) {
    auto counts = std::make_shared<std::map<int, int>>();
    for (auto& [id, pending] : host->pending_) {
      if (pending.browser != browser) continue;
      (*counts)[id] = 0;
      auto callback = std::move(pending.callback);
      pending.callback = [counts, id, callback](auto result) { ++(*counts)[id]; callback(result); };
    }
    return counts;
  }
};
class Close final : public CefTask {
public:
  explicit Close(CefRefPtr<CefBrowser> b) : browser(b) {}
  void Execute() override { if (browser && browser->IsValid()) browser->GetHost()->CloseBrowser(true); }
private:
  CefRefPtr<CefBrowser> browser;
  IMPLEMENT_REFCOUNTING(Close);
};
class Client final : public CefClient,
                     public CefRenderHandler,
                     public CefLifeSpanHandler,
                     public CefDisplayHandler,
                     public CefLoadHandler,
                     public CefDevToolsMessageObserver {
public:
  explicit Client(Client* parent = nullptr) : anchor(parent), lifecycle(getenv("GNOSIS_PROBE_LIFECYCLE")) {}
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, int) override {
    if (anchor && f->IsMain()) { page_loaded = true; anchor->start_lifecycle(); }
    if (f->IsMain() && getenv("GNOSIS_PUBLIC_ADBLOCK_SCRIPT") &&
        f->GetURL().ToString().starts_with("https://adblock.turtlecute.org/")) {
      f->ExecuteJavaScript("console.log('PUBLIC_ADBLOCK_OBSERVER_STATE:'+JSON.stringify({href:location.href,readyState:document.readyState,installed:!!window.__gnosisPublicObserver,storage:localStorage.getItem('adb_tool')}))", f->GetURL(), 1);
    }
    if (f->IsMain() && f->GetURL().ToString().starts_with("chrome-extension://") &&
        getenv("GNOSIS_PROBE_SCRIPT")) {
      std::ifstream file(getenv("GNOSIS_PROBE_SCRIPT"));
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
    --creating;
    ++browsers;
    browser = b;
    printf("Created Alloy browser %d\n", b->GetIdentifier());
    if (!anchor && getenv("GNOSIS_PUBLIC_ADBLOCK_SCRIPT")) {
      public_trace = b->GetHost()->AddDevToolsMessageObserver(this);
      b->GetHost()->ExecuteDevToolsMethod(1000000, "Page.enable", nullptr);
      b->GetHost()->ExecuteDevToolsMethod(1000001, "Network.enable", nullptr);
      std::ifstream file(getenv("GNOSIS_PUBLIC_ADBLOCK_SCRIPT"));
      std::string script((std::istreambuf_iterator<char>(file)), {});
      auto args = CefDictionaryValue::Create(); args->SetString("source", script);
      b->GetHost()->ExecuteDevToolsMethod(1000002, "Page.addScriptToEvaluateOnNewDocument", args);
    }
    if (anchor && !anchor->browser) { CefPostTask(TID_UI, new Close(browser)); return; }
    if (anchor) { host = anchor->host; host->attach(b); return; }
    host = new ExtensionHost;
    std::string path = getenv("GNOSIS_PROBE_EXTENSION");
    path = path.substr(0, path.find(','));
    auto extension = Extensions::read_manifest(path);
    auto ext = extension.id;
    extension_name = ext;
    std::string paths = getenv("GNOSIS_PROBE_EXTENSION");
    auto split = paths.find(',');
    if (split != std::string::npos)
      host->tab_bridge_id = Extensions::id_for_path(paths.substr(split + 1));
    host->extension_ids.insert(ext);
    host->permissions[ext] = extension.permissions;
    host->extension_paths[ext] = path;
    host->tabs = [this](CefRefPtr<CefBrowser>) {
      std::vector<ExtensionTab> all;
      for (auto* client : {this, destination.get()}) {
        if (!client || !client->browser || client->native_id < 0) continue;
        auto info = CefDictionaryValue::Create();
        info->SetInt("id", client->native_id);
        info->SetBool("active", client == this);
        info->SetString("url", client->browser->GetMainFrame()->GetURL());
        all.push_back({client->native_id, client->browser, info});
      }
      return all;
    };
    host->attach(b);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> b) override {
    public_trace = nullptr;
    browser = nullptr;
    if (anchor) { if (host) host->detach(b->GetIdentifier()); }
    else {
      if (host) host->stop();
      if (destination && destination->browser) CefPostTask(TID_UI, new Close(destination->browser));
    }
    finished = --browsers == 0 && creating == 0;
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t severity, const CefString& m, const CefString& source,
                        int line) override {
    if (m.ToString().starts_with("GNOSIS_NATIVE_TAB_ID:")) {
      native_id = atoi(m.ToString().substr(sizeof("GNOSIS_NATIVE_TAB_ID:") - 1).c_str());
      if (anchor) anchor->start_lifecycle();
    }
    if (anchor && m == "EXTENSION_LIFECYCLE_PENDING") {
      anchor->destination_callbacks = ExtensionHostTestPeer::watch_destination(host, browser->GetIdentifier());
      CefPostTask(TID_UI, new Close(browser));
      return true;
    }
    printf("%s\n", m.ToString().c_str());
    if (severity >= LOGSEVERITY_ERROR)
      printf("Extension console source: %s:%d\n", source.ToString().c_str(), line);
    if (m == "UBO_DASHBOARD_READY") dashboard_ready = true;
    fflush(stdout);
    if (m.ToString().starts_with("UBO_SETUP_ERROR:") && browser)
      CefPostTask(TID_UI, new Close(browser));
    std::set<std::string> expected = getenv("GNOSIS_PROBE_SCRIPT") ?
      std::set<std::string>{"UBO_NETWORK", "UBO_COSMETIC", "UBO_SCRIPTLET", "UBO_FRAME"} :
      std::set<std::string>{"EXTENSION_RUNTIME", "EXTENSION_BLOCKING", "EXTENSION_HOST_ISOLATION"};
    if (getenv("GNOSIS_PUBLIC_ADBLOCK_SCRIPT")) expected = {"PUBLIC_ADBLOCK"};
    if (getenv("GNOSIS_PROBE_NETWORK")) expected.insert("UBO_NETWORK_PROBES");
    for (const auto& marker : expected) {
      if (m == marker + ":FAIL") {
        if (browser) CefPostTask(TID_UI, new Close(browser));
      } else if (m == marker + ":PASS") passed.insert(marker);
    }
    if (passed.size() == expected.size() && browser) {
      if (!lifecycle) CefPostTask(TID_UI, new Close(browser));
      else if (!destination) {
        destination = new Client(this);
        CefWindowInfo w; w.SetAsWindowless(0); CefBrowserSettings s;
        auto target = std::string(getenv("GNOSIS_PROBE_URL"));
        target = target.substr(0, target.find_last_of('/')+1)+"extension-child.html?lifecycle-target";
        ++creating;
        if (!CefBrowserHost::CreateBrowser(w, destination, target, s, nullptr, nullptr)) {
          --creating;
          printf("EXTENSION_DESTINATION_CLOSE:FAIL (native browser creation rejected)\n"); fflush(stdout);
          CefPostTask(TID_UI, new Close(browser));
        }
      }
    }
    return true;
  }
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success, const void* result, size_t size) override {
    if (id != 1000000 && id != 1000001 && id != 1000002) return;
    printf("PUBLIC_ADBLOCK_CDP_SETUP:%d:%s:%.*s\n", id, success ? "PASS" : "FAIL", static_cast<int>(size), static_cast<const char*>(result));
    fflush(stdout);
  }
  void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString& method, const void* params, size_t size) override {
    const auto event = method.ToString();
    if (!event.starts_with("Network.")) return;
    printf("PUBLIC_ADBLOCK_CDP:%s:%.*s\n", event.c_str(), static_cast<int>(size), static_cast<const char*>(params));
    fflush(stdout);
  }
  CefRefPtr<CefBrowser> browser;
  int native_id = -1;
  CefRefPtr<ExtensionHost> host;
  std::set<std::string> passed;
  bool dashboard_ready = false;
  void start_lifecycle() {
    if (lifecycle_started || !destination || !destination->page_loaded || destination->native_id < 0) return;
    lifecycle_started = true;
    auto target = std::to_string(destination->native_id), source = std::to_string(native_id);
    auto expression = "(async()=>{let caught='';try{await chrome.tabs.executeScript("+target+
      ",{code:\"console.log('EXTENSION_LIFECYCLE_PENDING');new Promise(()=>{})\"});}"
      "catch(error){caught=error.message;}const all=await chrome.tabs.query({});return {caught,"
      "anchor:all.some(t=>t.id==="+source+"),removed:!all.some(t=>t.id==="+target+")};})()";
    CefRefPtr<Client> self = this;
    if (!ExtensionHostTestPeer::background(host, extension_name, expression, [self](auto response) {
      auto diagnostic = CefValue::Create(); diagnostic->SetDictionary(response->Copy(false));
      printf("Lifecycle native background result: %s\n", CefWriteJSON(diagnostic, JSON_WRITER_DEFAULT).ToString().c_str());
      auto remote = response->GetDictionary("result");
      auto result = remote ? remote->GetDictionary("value") : nullptr;
      bool ok = !response->GetBool("gnosisError") && result && result->GetBool("anchor") &&
        result->GetBool("removed") && result->GetString("caught").ToString().find("closed") != std::string::npos;
      ok &= self->destination_callbacks && !self->destination_callbacks->empty();
      if (self->destination_callbacks)
        for (const auto& [id, count] : *self->destination_callbacks) ok &= count == 1;
      printf("EXTENSION_DESTINATION_CLOSE:%s\n", ok ? "PASS" : "FAIL"); fflush(stdout);
      if (!ok) { CefPostTask(TID_UI, new Close(self->browser)); return; }
      ExtensionHostTestPeer::failures(self->host, self->browser, [self](bool ok) {
        printf("EXTENSION_TRANSPORT_OWNERSHIP:%s\n", ok ? "PASS" : "FAIL"); fflush(stdout);
        CefPostTask(TID_UI, new Close(self->browser));
      });
    })) {
      printf("EXTENSION_DESTINATION_CLOSE:FAIL (missing native background session)\n"); fflush(stdout);
      CefPostTask(TID_UI, new Close(browser));
    }
  }

private:
  Client* anchor;
  bool lifecycle, page_loaded = false, lifecycle_started = false;
  std::string extension_name;
  CefRefPtr<Client> destination;
  std::shared_ptr<std::map<int, int>> destination_callbacks;
  CefRefPtr<CefRegistration> public_trace;
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
    if (client->browser && !client->dashboard_ready)
      client->browser->GetMainFrame()->LoadURL(getenv("GNOSIS_PROBE_URL"));
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
      configure_extensions(c, getenv("GNOSIS_PROBE_EXTENSION"));
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
    ++creating;
    if (!CefBrowserHost::CreateBrowser(w, client, getenv("GNOSIS_PROBE_URL"), s, nullptr, nullptr)) {
      --creating;
      finished = true;
      fprintf(stderr, "Native extension browser creation was rejected\n");
    }
    if (getenv("GNOSIS_PROBE_SCRIPT"))
      CefPostDelayedTask(TID_UI, new Reload(client), 8000);
    CefPostDelayedTask(TID_UI, new Exit(client), getenv("GNOSIS_PUBLIC_ADBLOCK_SCRIPT") ? 165000 : 35000);
  }
  CefRefPtr<Client> client;

private:
  IMPLEMENT_REFCOUNTING(App);
};
NO_STACK_PROTECTOR int main(int argc, char** argv) {
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
  char profile[] = "/tmp/gnosis-extension-probe-XXXXXX";
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
