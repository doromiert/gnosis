/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native-window embedding extracted from the visibly verified Jupiter Brave PoC.
 */
#include "browser.hpp"
#include "compositor.h"
#include "profile.h"
#include <gdk/x11/gdkx.h>
#include <glib-unix.h>
#include <libsoup/soup.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <map>
#include <set>
#include <vector>
#include <cstring>
#include <cmath>
#include <glib/gi18n.h>
namespace JupiterBrave {
namespace {
std::string profile_root;
// Fixed by the public key in the packaged native-integration manifest.
constexpr const char* native_integration_id = "lplpdmfakaadhenboelgbkbdngpldloj";
std::map<bool, std::shared_ptr<Process>> processes;
const char* text(JsonObject* o, const char* key) { return o ? json_object_get_string_member_with_default(o, key, "") : ""; }
JsonObject* object(JsonObject* o, const char* key) { return o && json_object_has_member(o, key) ? json_object_get_object_member(o, key) : nullptr; }
int ignore_x_error(Display*, XErrorEvent*) { return 0; }
}
bool enabled() { return g_strcmp0(g_getenv("JUPITER_ENGINE"), "brave") == 0; }
bool composited() { return g_strcmp0(g_getenv("JUPITER_SURFACE"), "x11") != 0; }
std::string quote(const std::string& value) {
  auto* node = json_node_new(JSON_NODE_VALUE); json_node_set_string(node, value.c_str());
  char* json = json_to_string(node, false); std::string result(json); g_free(json); json_node_free(node); return result;
}
namespace {
bool internal_document(const std::string& value) { return value.starts_with("ephy-about:"); }
std::string transport_uri(const std::string& origin, const std::string& value) {
  for (const char* scheme : {"ephy-about", "ephy-resource"}) {
    std::string prefix = std::string(scheme) + ':';
    if (value.starts_with(prefix)) return origin + '/' + scheme + '/' + value.substr(prefix.size());
  }
  return value;
}
std::string rewrite_resources(std::string value, const std::string& origin) {
  for (const char* scheme : {"ephy-about", "ephy-resource"}) {
    std::string from = std::string(scheme) + ':', to = origin + '/' + scheme + '/';
    for (size_t pos = 0; (pos = value.find(from, pos)) != std::string::npos; pos += to.size()) value.replace(pos, from.size(), to);
  }
  return value;
}
}
struct Page::State {
  GtkWidget* content;
  GtkEventController* keys = nullptr;
  GtkWidget* surface = nullptr;
  std::shared_ptr<Process> process;
  std::string token, bootstrap, requested = "about:blank", main_frame;
  std::string internal_origin, context_unique;
  std::string authorized_document, fulfilled_document, committed_document;
  std::map<std::string, std::string> internal_loaders;
  guint64 navigation_generation = 0;
  int context_id = 0;
  bool protocol_ready = false;
  Display* display = nullptr;
  ::Window parent = 0, child = 0, container = 0;
  int window = -1, width = 0, height = 0;
  int native_tab = -1;
  double zoom_requested = 1, zoom_applied = 1;
  bool zoom_pending = false, zoom_busy = false;
  guint64 zoom_revision = 0;
  int response_status = 0;
  GTlsCertificate* certificate = nullptr;
  GTlsCertificateFlags tls_errors = GTlsCertificateFlags(0);
  std::string security_state;
  gint64 withdrawn_until = 0;
  guint timer = 0;
  bool ephemeral, started = false, disposed = false, mapped = true, attached = false, native_destroyed = false;
  GtkWidget* context_menu = nullptr;
  std::map<int, std::string> menu_contexts;
  std::vector<std::function<void()>> waiting;
  explicit State(GtkWidget* widget, bool private_mode) : content(widget), ephemeral(private_mode) {
    char* id = g_uuid_string_random(); token = id; g_free(id);
    internal_origin = "https://" + token + ".jupiter.invalid";
  }
  ~State() { g_clear_object(&certificate); }
};
class Process : public std::enable_shared_from_this<Process> {
public:
  std::string profile, window_class, base, product;
  GSubprocess* browser = nullptr;
  SoupServer* server = nullptr;
  JupiterCompositor* compositor = nullptr;
  int input = -1, output = -1, sequence = 0;
  guint watch = 0;
  bool closing = false, ready = false, launched = false;
  bool integration_ready = false, integration_connecting = false;
  std::string integration_id;
  std::string integration_session;
  guint integration_poll = 0;
  gint64 integration_deadline = 0;
  std::string buffer;
  std::map<int, Reply> replies;
  std::map<std::string, std::weak_ptr<Page>> pages, targets, sessions;
  std::vector<std::weak_ptr<Page>> pending_opens;
  std::map<int,std::weak_ptr<Page>> zoom_tabs;
  std::map<std::string, std::weak_ptr<Page>> frame_owners;
  std::map<std::string, DownloadProgress> downloads;
  std::set<std::string> download_guids;
  std::string download_directory;
  struct ParkedPage { std::string target, session; ::Window child; GtkWidget* surface = nullptr; };
  std::vector<ParkedPage> parked;
  std::map<std::string, gint64> routed_windows;
  explicit Process(bool ephemeral) {
    if (composited()) {
      GError* failure = nullptr;
      compositor = jupiter_compositor_new(&failure);
      if (!compositor) {
        g_warning("Brave compositor: %s", failure->message); g_error_free(failure); return;
      }
    }
    if (ephemeral) { char* path = g_dir_make_tmp("jupiter-brave-private-XXXXXX", nullptr); if (path) { profile = path; g_free(path); } }
    else profile = profile_root + "/brave";
    GError* profile_error = nullptr;
    if (!jupiter_brave_profile_prepare(profile.c_str(), &profile_error)) {
      g_warning("Brave profile preparation: %s", profile_error->message); g_error_free(profile_error); return;
    }
    std::string staging = profile + "/.downloads-XXXXXX";
    char* directory = g_mkdtemp(staging.data());
    if (!directory) { g_warning("Cannot create Brave's private download directory"); return; }
    download_directory = directory;
    char* id = g_uuid_string_random(); window_class = std::string("JupiterBrave-") + id; g_free(id);
    server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(server, nullptr, +[](SoupServer*, SoupServerMessage* message, const char* path, GHashTable*, gpointer data) {
      auto* self = static_cast<Process*>(data);
      const std::string key = path && g_str_has_prefix(path, "/bootstrap/") ? path + 11 : "";
      auto found = self->pages.find(key);
      if (found == self->pages.end() || found->second.expired()) { soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr); return; }
      std::string html = "<!doctype html><title>JupiterTab-" + key + "</title><style>:root{color-scheme:dark light}</style>";
      soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
      soup_message_headers_replace(soup_server_message_get_response_headers(message), "Cache-Control", "no-store");
      soup_message_headers_replace(soup_server_message_get_response_headers(message), "Referrer-Policy", "no-referrer");
      soup_server_message_set_response(message, "text/html", SOUP_MEMORY_COPY, html.data(), html.size());
    }, this, nullptr);
    GError* error = nullptr;
    if (!soup_server_listen_local(server, 0, SoupServerListenOptions(SOUP_SERVER_LISTEN_IPV4_ONLY), &error)) {
      g_warning("Brave bootstrap server: %s", error->message); g_error_free(error); return;
    }
    GSList* uris = soup_server_get_uris(server);
    if (uris) { char* value = g_uri_to_string(static_cast<GUri*>(uris->data)); base = value; g_free(value); }
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
  }
  ~Process() {
    stop();
    for (auto& page : parked) if (page.surface) g_object_unref(page.surface);
    parked.clear();
    g_clear_object(&server); g_clear_object(&compositor);
    if (!download_directory.empty()) {
      GDir* directory = g_dir_open(download_directory.c_str(), 0, nullptr);
      if (directory) {
        while (const char* name = g_dir_read_name(directory)) {
          g_autofree char* path = g_build_filename(download_directory.c_str(), name, nullptr);
          unlink(path);
        }
        g_dir_close(directory);
      }
      rmdir(download_directory.c_str());
    }
  }
  void command(const std::string& method, const std::string& params, const std::string& session, Reply reply = {}) {
    if (closing || output < 0) { if (reply) reply(nullptr, "The Brave browser is closed"); return; }
    int id = ++sequence;
    std::string json = "{\"id\":" + std::to_string(id) + ",\"method\":" + quote(method) + ",\"params\":" + params;
    if (!session.empty()) json += ",\"sessionId\":" + quote(session);
    json += '}'; json.push_back('\0');
    if (reply) replies.emplace(id, std::move(reply));
    for (size_t offset = 0; offset < json.size();) {
      ssize_t count = write(output, json.data() + offset, json.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { fail("Brave's private protocol pipe closed"); return; }
      offset += count;
    }
  }
  void fail(const std::string& error) {
    auto pending = std::move(replies); replies.clear();
    for (auto& [id, reply] : pending) reply(nullptr, error);
  }
  void fail_downloads() {
    auto pending = std::move(downloads); downloads.clear();
    for (auto& [guid, progress] : pending) progress(0, 0, "failed");
  }
  void disconnected() {
    if (closing) return;
    closing = true;
    if (watch) { g_source_remove(watch); watch = 0; }
    if (integration_poll) { g_source_remove(integration_poll); integration_poll = 0; }
    fail_downloads();
    fail("The Brave browser process exited");
    if (input >= 0) ::close(input);
    if (output >= 0) ::close(output);
    input = output = -1;
    g_clear_object(&browser);
    if (server) soup_server_disconnect(server);
    std::vector<std::shared_ptr<Page>> affected;
    for (auto& [token, weak] : pages) if (auto page = weak.lock()) affected.push_back(page);
    for (auto& page : affected) {
      auto failed = page->failed;
      page->close();
      if (failed) failed("The browser process stopped. Reload this tab to reconnect.");
    }
  }
  void open(const std::shared_ptr<Page>& page) {
    if (base.empty()) {
      closing = true;
      auto failed = page->failed; page->close();
      if (failed) failed("Could not initialize the Brave content backend. Reload to retry.");
      return;
    }
    pages[page->state_->token] = page;
    // Restored tabs arrive together. Until the first process owns its profile
    // and protocol pipe, another --app launcher can win Chromium's singleton
    // race and leave us connected to the process that exits instead.
    if (launched && !ready) {
      pending_opens.push_back(page);
      return;
    }
    g_message("Brave tab requested: token=%s reuse=%s", page->state_->token.c_str(), parked.empty() ? "no" : "yes");
    if (!parked.empty()) {
      // Keep one hidden app window alive when its GTK tab closes. Otherwise
      // Chromium exits after the last app window and a concurrent --app launch
      // can become an uncontrolled process with no inherited protocol pipe.
      auto reusable = std::move(parked.back()); parked.pop_back();
      page->target = reusable.target; page->session = reusable.session;
      page->state_->child = reusable.child;
      if (reusable.surface) {
        page->adopt_surface(reusable.surface); g_object_unref(reusable.surface);
      }
      targets[page->target] = page; sessions[page->session] = page;
      page->command("Page.resetNavigationHistory");
      page->attach();
      return;
    }
    page->state_->bootstrap = base + "bootstrap/" + page->state_->token;
    if (composited()) page->adopt_surface(jupiter_compositor_view_new_for_server(compositor, page->state_->token.c_str()));
    std::vector<std::string> args{g_getenv("JUPITER_BRAVE_EXECUTABLE") ? g_getenv("JUPITER_BRAVE_EXECUTABLE") : "brave",
      "--user-data-dir=" + profile, "--app=" + page->state_->bootstrap, "--class=" + window_class,
      composited() ? "--ozone-platform=wayland" : "--ozone-platform=x11", "--no-first-run", "--no-default-browser-check", "--disable-background-mode",
      "--password-store=basic", "--window-size=1000,700", "--window-position=-16000,-16000"};
    // The outer GTK compositor owns display pacing. Chromium's nested timer
    // otherwise caps animation at 60 Hz even with 120 Hz frame callbacks.
    // Keep an override for comparing the native scheduler during migration.
    if (composited() && g_strcmp0(g_getenv("JUPITER_UNCAP_FRAMES"), "0") != 0)
      args.push_back("--disable-frame-rate-limit");
    auto* launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    if (composited()) {
      g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", jupiter_compositor_get_socket(compositor), true);
      g_subprocess_launcher_unsetenv(launcher, "WAYLAND_SOCKET");
      g_subprocess_launcher_unsetenv(launcher, "DISPLAY");
    }
    for (const char* name : {"LD_LIBRARY_PATH", "LD_PRELOAD", "CEF_ROOT"}) g_subprocess_launcher_unsetenv(launcher, name);
    const bool first = !launched;
    g_subprocess_launcher_set_stdout_file_path(launcher, (profile + (first ? "/stdout.log" : "/launcher-stdout.log")).c_str());
    g_subprocess_launcher_set_stderr_file_path(launcher, (profile + (first ? "/stderr.log" : "/launcher-stderr.log")).c_str());
    if (first) {
      int to_browser[2], from_browser[2];
      if (pipe2(to_browser, O_CLOEXEC) || pipe2(from_browser, O_CLOEXEC)) { g_object_unref(launcher); return; }
      output = to_browser[1]; input = from_browser[0]; fcntl(input, F_SETFL, O_NONBLOCK);
      g_subprocess_launcher_take_fd(launcher, to_browser[0], 3);
      g_subprocess_launcher_take_fd(launcher, from_browser[1], 4);
      args.push_back("--remote-debugging-pipe"); args.push_back("--enable-unsafe-extension-debugging");
      // Implemented by our small native Brave patch. Stock Brave ignores it;
      // the extension allowlist does not suppress automatic zoom bubbles.
      args.push_back("--jupiter-native-ui");
      launched = true;
    }
    std::vector<const char*> argv; for (auto& argument : args) argv.push_back(argument.c_str()); argv.push_back(nullptr);
    GError* error = nullptr; auto* child = g_subprocess_launcher_spawnv(launcher, argv.data(), &error); g_object_unref(launcher);
    if (!child) { g_warning("Could not start Brave: %s", error->message); g_error_free(error); return; }
    if (!first) { g_object_unref(child); return; }
    browser = child;
    g_subprocess_wait_async(child,nullptr,+[](GObject* source,GAsyncResult* result,gpointer data) {
      std::unique_ptr<std::weak_ptr<Process>> weak(static_cast<std::weak_ptr<Process>*>(data));
      g_autoptr(GError) error = nullptr;
      g_subprocess_wait_finish(G_SUBPROCESS(source),result,&error);
      if (auto self = weak->lock(); self && !self->closing && self->browser == G_SUBPROCESS(source)) {
        if (error) g_warning("Brave process wait failed: %s",error->message);
        else if (g_subprocess_get_if_signaled(G_SUBPROCESS(source)))
          g_warning("Brave browser process died from signal %d",g_subprocess_get_term_sig(G_SUBPROCESS(source)));
        else g_warning("Brave browser process exited with status %d",g_subprocess_get_exit_status(G_SUBPROCESS(source)));
        self->disconnected();
      }
    },new std::weak_ptr<Process>(weak_from_this()));
    watch = g_unix_fd_add(input, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), +[](gint, GIOCondition condition, gpointer data) -> gboolean {
      auto* self = static_cast<Process*>(data); self->read();
      if (self->closing) { self->watch = 0; return G_SOURCE_REMOVE; }
      if (condition & (G_IO_HUP | G_IO_ERR)) { self->watch = 0; self->disconnected(); return G_SOURCE_REMOVE; }
      return G_SOURCE_CONTINUE;
    }, this);
    command("Browser.getVersion", "{}", "", [this](JsonObject* result, const std::string& error) {
      if (!result) { g_warning("Brave startup: %s", error.c_str()); return; }
      product = text(result, "product"); ready = true;
      command("Browser.setDownloadBehavior", "{\"behavior\":\"allowAndName\",\"downloadPath\":" + quote(download_directory) +
        ",\"eventsEnabled\":true}", "", [this](JsonObject*, const std::string& error) {
          if (!error.empty()) { fail_downloads(); g_warning("Could not connect Brave downloads: %s", error.c_str()); }
        });
      command("Target.setDiscoverTargets", "{\"discover\":true}", "");
      command("Extensions.loadUnpacked", "{\"path\":" + quote(JUPITER_NATIVE_INTEGRATION_DIR) + "}", "", [this](JsonObject* result, const std::string& error) {
        if (!result) { integration_failed(error); return; }
        integration_id = text(result, "id");
        if (integration_id.empty()) { integration_failed("Native integration has no extension identity"); return; }
        if (integration_id != native_integration_id) { integration_failed("Native integration does not match its packaged identity"); return; }
        command("Target.getTargets", "{}", "", [this](JsonObject* result, const std::string&) {
          auto* list = result ? json_object_get_array_member(result, "targetInfos") : nullptr;
          for (guint i = 0; list && i < json_array_get_length(list); ++i) discovered(json_array_get_object_element(list, i));
        });
      });
      auto pending = std::move(pending_opens); pending_opens.clear();
      for (auto& weak : pending) if (auto page = weak.lock(); page && !page->is_closed()) open(page);
    });
  }
  void integration_failed(const std::string& error) {
    g_warning("Native browser UI handoff failed: %s", error.c_str());
    command("Browser.close", "{}", ""); disconnected();
  }
  void poll_integration() {
    if (closing || integration_ready) return;
    command("Runtime.evaluate", "{\"expression\":\"Promise.resolve(globalThis.jupiterNativeReady).then(value=>value===true)\",\"awaitPromise\":true,\"returnByValue\":true}", integration_session,
      [this](JsonObject* result, const std::string& error) {
        if (closing) return;
        auto* details = object(result, "exceptionDetails");
        if (!result || details) {
          const std::string description = text(object(details, "exception"), "description");
          integration_failed(!error.empty() ? error : !description.empty() ? description : "Native integration worker failed"); return;
        }
        auto* value = object(result, "result");
        if (value && json_object_get_boolean_member_with_default(value, "value", false)) {
          integration_ready = true;
          g_message("Brave download UI disabled: GTK owns download feedback");
          // Keep this private worker session alive for native browser services.
          for (auto& [token, weak] : pages) if (auto page = weak.lock()) page->surface_ready();
          return;
        }
        if (g_get_monotonic_time() >= integration_deadline) { integration_failed("Native integration worker did not become ready"); return; }
        integration_poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 25, +[](gpointer data) -> gboolean {
          if (auto self = static_cast<std::weak_ptr<Process>*>(data)->lock()) {
            self->integration_poll = 0; self->poll_integration();
          }
          return G_SOURCE_REMOVE;
        }, new std::weak_ptr<Process>(weak_from_this()), +[](gpointer data) { delete static_cast<std::weak_ptr<Process>*>(data); });
      });
  }
  void discovered(JsonObject* info) {
    if (!integration_ready && !integration_connecting && !integration_id.empty() &&
        std::string(text(info, "type")) == "service_worker" &&
        std::string(text(info, "url")) == "chrome-extension://" + integration_id + "/background.js") {
      integration_connecting = true;
      command("Target.attachToTarget", "{\"targetId\":" + quote(text(info, "targetId")) + ",\"flatten\":true}", "",
        [this](JsonObject* result, const std::string& error) {
          if (!result) { integration_failed(error); return; }
          integration_session = text(result, "sessionId");
          integration_deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
          command("Runtime.enable", "{}", integration_session);
          command("Runtime.addBinding", "{\"name\":\"__jupiterZoomChanged\"}",integration_session);
          command("Runtime.runIfWaitingForDebugger", "{}", integration_session);
          poll_integration();
        });
    }
    if (strcmp(text(info, "type"), "page")) return;
    const std::string uri = text(info, "url");
    for (auto& [token, weak] : pages) if (auto page = weak.lock()) {
      if (!page->target.empty() || (page->state_->bootstrap != uri &&
          std::string(text(info, "title")) != "JupiterTab-" + token)) continue;
      page->target = text(info, "targetId"); targets[page->target] = page;
      g_message("Brave target connected: token=%s target=%s", token.c_str(), page->target.c_str());
      auto args = "{\"targetId\":" + quote(page->target) + ",\"flatten\":true}";
      command("Target.attachToTarget", args, "", [this, weak](JsonObject* result, const std::string&) {
        auto page = weak.lock(); if (!page || page->state_->disposed || !result) return;
        page->session = text(result, "sessionId"); sessions[page->session] = page; page->attach();
      });
      return;
    }
    const std::string id = text(info, "targetId");
    if (targets.contains(id)) return;
    const auto now = g_get_monotonic_time();
    std::erase_if(routed_windows, [now](const auto& entry) { return entry.second < now; });
    if (auto routed = routed_windows.find(uri); routed != routed_windows.end()) {
      routed_windows.erase(routed);
      command("Target.closeTarget", "{\"targetId\":" + quote(id) + "}", "");
      return;
    }
    auto opener = targets.find(text(info, "openerId"));
    if (opener != targets.end() && (uri.starts_with("https://") || uri.starts_with("http://"))) {
      auto page = opener->second.lock();
      if (page && page->open_uri && page->open_uri(uri))
        command("Target.closeTarget", "{\"targetId\":" + quote(id) + "}", "");
    }
  }
  void read() {
    char chunk[65536];
    for (;;) {
      ssize_t size = ::read(input, chunk, sizeof chunk);
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) break;
      buffer.append(chunk, size);
    }
    for (;;) {
      const auto end = buffer.find('\0'); if (end == std::string::npos) break;
      std::string json = buffer.substr(0, end); buffer.erase(0, end + 1);
      auto* parser = json_parser_new();
      if (json_parser_load_from_data(parser, json.data(), json.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        auto* root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "id")) {
          int id = json_object_get_int_member(root, "id"); auto found = replies.find(id);
          if (found != replies.end()) {
            auto reply = std::move(found->second); replies.erase(found);
            auto* failure = object(root, "error"); reply(object(root, "result"), failure ? text(failure, "message") : "");
          }
        } else {
          const std::string method = text(root, "method"); auto* params = object(root, "params");
          if (method == "Browser.downloadWillBegin") {
            const std::string guid = text(params, "guid");
            // Browser events can be delivered through more than one attached
            // session. Each transfer must create exactly one native object.
            if (download_guids.contains(guid)) { g_object_unref(parser); continue; }
            download_guids.insert(guid);
            auto found = frame_owners.find(text(params, "frameId"));
            auto page = found == frame_owners.end() ? nullptr : found->second.lock();
            if (!g_uuid_string_is_valid(guid.c_str()) || !page || page->is_closed() || !page->download_begin) {
              g_message("Brave download rejected: valid-guid=%d known-frame=%d active-page=%d", g_uuid_string_is_valid(guid.c_str()),
                found != frame_owners.end(), page && !page->is_closed());
              command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", "");
            } else {
              auto weak = weak_from_this();
              auto progress = page->download_begin(text(params, "url"), text(params, "suggestedFilename"), download_directory + '/' + guid,
                [weak, guid] { if (auto self = weak.lock()) self->command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", ""); });
              if (progress) downloads[guid] = std::move(progress);
              else command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", "");
            }
          } else if (method == "Browser.downloadProgress") {
            auto found = downloads.find(text(params, "guid"));
            if (found != downloads.end()) {
              auto progress = found->second;
              const std::string state = text(params, "state");
              if (state != "inProgress") downloads.erase(found);
              progress(static_cast<guint64>(std::max(0., json_object_get_double_member_with_default(params, "receivedBytes", 0))),
                       static_cast<guint64>(std::max(0., json_object_get_double_member_with_default(params, "totalBytes", 0))), state);
            }
          } else if (method == "Runtime.bindingCalled" && std::string(text(root,"sessionId")) == integration_session &&
                     std::string(text(params,"name")) == "__jupiterZoomChanged") {
            const std::string payload = text(params,"payload");
            g_autoptr(JsonParser) event = json_parser_new();
            if (payload.size() <= 1024 && json_parser_load_from_data(event,payload.data(),payload.size(),nullptr) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(event))) {
              auto* change = json_node_get_object(json_parser_get_root(event));
              auto found = zoom_tabs.find(json_object_get_int_member_with_default(change,"tabId",-1));
              if (found != zoom_tabs.end() && std::string(text(change,"scope")) == "per-tab") if (auto page = found->second.lock())
                page->receive_zoom(json_object_get_double_member_with_default(change,"factor",0));
            }
          } else if (method == "Target.targetCreated") discovered(object(params, "targetInfo"));
          else if (method == "Target.targetInfoChanged") {
            auto* info = object(params, "targetInfo"); discovered(info); auto found = targets.find(text(info, "targetId"));
            if (found != targets.end()) if (auto page = found->second.lock()) {
              if (page->state_->attached) { page->title = text(info, "title"); page->uri = page->canonical_uri(text(info, "url")); if (page->changed) page->changed(); }
            }
          } else if (method == "Target.targetCrashed") {
            g_warning("Brave renderer crashed: target=%s status=%s error=%d",text(params,"targetId"),text(params,"status"),
              int(json_object_get_int_member_with_default(params,"errorCode",0)));
            auto found = targets.find(text(params,"targetId"));
            if (found != targets.end()) if (auto page = found->second.lock()) {
              page->loading = false; page->progress = 0; page->dismiss_context_menu();
              if (page->changed) page->changed();
            }
          } else if (method == "Target.targetDestroyed") {
            auto found = targets.find(text(params, "targetId"));
            if (found != targets.end()) {
              auto page = found->second.lock();
              targets.erase(found);
              if (page && !page->state_->disposed) {
                page->state_->native_destroyed = true;
                if (page->closed) page->closed();
              }
            }
            std::erase_if(parked, [&](const auto& page) {
              if (page.target != text(params, "targetId")) return false;
              if (page.surface) g_object_unref(page.surface);
              return true;
            });
          }
          auto found = sessions.find(text(root, "sessionId"));
          if (found != sessions.end()) if (auto page = found->second.lock()) page->handle(method, params);
        }
      }
      g_object_unref(parser);
    }
    if (buffer.size() > 64 * 1024 * 1024) { buffer.clear(); fail("Oversized Brave protocol message"); }
  }
  void stop() {
    if (closing) return;
    if (output >= 0) command("Browser.close", "{}", "");
    closing = true; fail_downloads(); fail("Brave is shutting down");
    if (integration_poll) { g_source_remove(integration_poll); integration_poll = 0; }
    if (watch) { g_source_remove(watch); watch = 0; }
    if (input >= 0) ::close(input);
    if (output >= 0) ::close(output);
    input = output = -1;
    g_clear_object(&browser);
    if (server) soup_server_disconnect(server);
  }
};
void initialize(const std::string& profile) { profile_root = profile; signal(SIGPIPE, SIG_IGN); }
void shutdown() { for (auto& [mode, process] : processes) process->stop(); processes.clear(); }
Page::Page(GtkWidget* widget, bool ephemeral) : state_(std::make_unique<State>(widget, ephemeral)) {
  if (composited()) return;
  auto* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_BUBBLE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey* controller, guint, guint, GdkModifierType modifiers, Page* self) -> gboolean {
    self->send_key(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)));
    // Let GTK's window shortcuts continue through their normal propagation path.
    return !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
  }), this);
  g_signal_connect(keys, "key-released", G_CALLBACK(+[](GtkEventControllerKey* controller, guint, guint, GdkModifierType, Page* self) {
    self->send_key(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)));
  }), this);
  gtk_widget_add_controller(widget, keys);
  g_object_set_data(G_OBJECT(keys), "jupiter-page", this);
  state_->keys = keys;
}
std::shared_ptr<Page> Page::create(GtkWidget* widget, bool ephemeral) { return std::shared_ptr<Page>(new Page(widget, ephemeral)); }
Page::~Page() { close(); }
GtkWidget* Page::widget() const { return state_->content; }
bool Page::is_closed() const { return state_->disposed; }
void Page::start() {
  if (state_->started || state_->disposed) return;
  auto* root = gtk_widget_get_root(state_->content); if (!root || !GTK_IS_NATIVE(root)) return;
  if (!composited()) {
    auto* surface = gtk_native_get_surface(GTK_NATIVE(root)); if (!GDK_IS_X11_SURFACE(surface)) { g_warning("Native Brave views require X11/XWayland"); return; }
    state_->parent = gdk_x11_surface_get_xid(surface); state_->display = XOpenDisplay(nullptr);
    if (!state_->display) return;
    XSetErrorHandler(ignore_x_error);
  }
  auto& process = processes[state_->ephemeral]; if (!process || process->closing) process = std::make_shared<Process>(state_->ephemeral);
  state_->process = process; state_->started = true;
  process->open(shared_from_this());
  if (!composited()) state_->timer = g_timeout_add(50, +[](gpointer data) -> gboolean { static_cast<Page*>(data)->layout(); return G_SOURCE_CONTINUE; }, this);
}
void Page::adopt_surface(GtkWidget* widget) {
  state_->surface = widget;
  g_signal_connect(widget, "surface-ready", G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<Page*>(data)->surface_ready(); }), this);
  g_signal_connect(widget, "fullscreen-changed", G_CALLBACK(+[](GtkWidget*, gboolean value, gpointer data) {
    auto* page = static_cast<Page*>(data); if (page->fullscreen_changed) page->fullscreen_changed(value);
  }), this);
  gtk_box_append(GTK_BOX(state_->content), widget);
  gtk_widget_set_visible(widget, state_->mapped);
}
void Page::surface_ready() {
  auto& s = *state_;
  if (s.disposed || s.attached || !s.protocol_ready || !s.process->integration_ready || session.empty() || !s.surface ||
      !jupiter_compositor_view_has_surface(JUPITER_COMPOSITOR_VIEW(s.surface))) return;
  s.attached = true; navigate(s.requested);
  g_message("Brave composited tab ready: target=%s", target.c_str());
}
void Page::command(const std::string& method, const std::string& params, Reply reply) {
  if (state_->disposed) { if (reply) reply(nullptr, "The tab is closed"); return; }
  if (!state_->process || session.empty()) { if (reply) reply(nullptr, "The Brave tab is not ready"); return; }
  state_->process->command(method, params, session, std::move(reply));
}
void Page::attach() {
  command("Page.enable"); command("Runtime.enable"); command("Network.enable"); command("Security.enable");
  state_->protocol_ready = false;
  command("Runtime.addBinding", "{\"name\":\"__jupiterContextMenu\",\"executionContextName\":\"JupiterContextMenu\"}");
  g_autofree char* menu_path = g_build_filename(JUPITER_NATIVE_INTEGRATION_DIR, "context-menu.js", nullptr);
  g_autofree char* menu_script = nullptr;
  if (g_file_get_contents(menu_path, &menu_script, nullptr, nullptr))
    command("Page.addScriptToEvaluateOnNewDocument", "{\"worldName\":\"JupiterContextMenu\",\"source\":" + quote(menu_script) + "}");
  else g_warning("The native context-menu adapter could not be loaded");
  command("Runtime.addBinding", "{\"name\":\"__jupiterNativeMessage\"}");
  const std::string script = "(()=>{const send=globalThis.__jupiterNativeMessage;delete globalThis.__jupiterNativeMessage;if(location.origin!==" + quote(state_->internal_origin) + R"JS(||window!==top)return;
    const events=new EventTarget();
    Object.defineProperty(globalThis,'Jupiter',{value:Object.freeze({
      postMessage:(name,payload)=>send(JSON.stringify({name,payload})),
      addEventListener:(...args)=>events.addEventListener(...args),
      removeEventListener:(...args)=>events.removeEventListener(...args)
    })});
    Object.defineProperty(globalThis,'__jupiterDispatch',{value:(name,payload)=>events.dispatchEvent(new CustomEvent(name,{detail:payload}))});
  })())JS";
  command("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + quote(script) + "}");
  command("Fetch.enable", "{\"patterns\":[{\"urlPattern\":" + quote(state_->internal_origin + "/*") + ",\"requestStage\":\"Request\"}]}");
  auto weak = weak_from_this();
  command("Page.getFrameTree", "{}", [weak](JsonObject* result, const std::string&) {
    auto self = weak.lock();
    auto* frame = object(object(result, "frameTree"), "frame");
    if (self && frame) {
      self->state_->main_frame = text(frame, "id");
      auto page = self;
      std::function<void(JsonObject*)> register_frames = [&](JsonObject* tree) {
        auto* item = object(tree, "frame");
        if (item) page->state_->process->frame_owners[text(item, "id")] = page;
        auto* children = tree && json_object_has_member(tree, "childFrames") ? json_object_get_array_member(tree, "childFrames") : nullptr;
        for (guint i = 0; children && i < json_array_get_length(children); ++i) register_frames(json_array_get_object_element(children, i));
      };
      register_frames(object(result, "frameTree"));
      self->state_->protocol_ready = true;
      if (composited()) self->surface_ready();
    }
  });
  state_->process->command("Browser.getWindowForTarget", "{\"targetId\":" + quote(target) + "}", "", [weak](JsonObject* result, const std::string&) {
    if (auto self = weak.lock(); self && result) {
      self->state_->window = json_object_get_int_member(result, "windowId"); self->apply_zoom();
    }
  });
}
void Page::navigate(const std::string& value) {
  dismiss_context_menu();
  state_->requested = value;
  state_->authorized_document = internal_document(value) ? value : "";
  state_->fulfilled_document.clear(); state_->committed_document.clear();
  state_->context_id = 0; state_->context_unique.clear(); ++state_->navigation_generation;
  state_->response_status = 0;
  if (!state_->attached) { uri = value; if (changed) changed(); start(); return; }
  command("Page.navigate", "{\"url\":" + quote(transport_uri(state_->internal_origin, value)) + "}");
}
std::string Page::canonical_uri(const std::string& value) const {
  for (const char* scheme : {"ephy-about", "ephy-resource"}) {
    std::string prefix = state_->internal_origin + '/' + scheme + '/';
    if (value.starts_with(prefix)) return std::string(scheme) + ':' + value.substr(prefix.size());
  }
  return value;
}
bool Page::is_internal_page(const std::string& value) const {
  return !state_->disposed && internal_document(value) && uri == value && state_->committed_document == value;
}
void Page::internal_request(JsonObject* params) {
  const std::string id = text(params, "requestId");
  auto* request = object(params, "request");
  const std::string value = canonical_uri(text(request, "url"));
  const bool main = std::string(text(params, "resourceType")) == "Document";
  const bool trusted = main ? (value == state_->authorized_document || !state_->committed_document.empty()) : !state_->committed_document.empty();
  if (!trusted || std::string(text(params, "frameId")) != state_->main_frame ||
      std::string(text(request, "method")) != "GET" || !internal_resource ||
      (main ? !internal_document(value) : !(value.starts_with("ephy-resource:") || internal_document(value)))) {
    if (g_getenv("JUPITER_TRACE_INTERNAL")) g_message("Native resource denied: main=%d trusted=%d frame-match=%d handler=%d", main, trusted,
      std::string(text(params, "frameId")) == state_->main_frame, bool(internal_resource));
    command("Fetch.failRequest", "{\"requestId\":" + quote(id) + ",\"errorReason\":\"AccessDenied\"}"); return;
  }
  auto weak = weak_from_this();
  const std::string frame = state_->main_frame;
  const guint64 generation = state_->navigation_generation;
  internal_resource(value, main, [weak, id, frame, generation, value, main](GBytes* bytes, const std::string& mime, const std::string& error) {
    auto self = weak.lock(); if (!self || self->is_closed()) return;
    if (!bytes || !error.empty() || frame != self->state_->main_frame || generation != self->state_->navigation_generation ||
        (main ? !(value == self->state_->authorized_document || !self->state_->committed_document.empty()) : self->state_->committed_document.empty())) {
      if (!error.empty()) g_warning("Native internal resource failed: %s", error.c_str());
      self->command("Fetch.failRequest", "{\"requestId\":" + quote(id) + ",\"errorReason\":\"AccessDenied\"}"); return;
    }
    gsize length = 0; const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &length));
    std::string content(data, length);
    if (mime.starts_with("text/") || mime.find("javascript") != std::string::npos || mime == "image/svg+xml")
      content = rewrite_resources(std::move(content), self->state_->internal_origin);
    g_autofree char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(content.data()), content.size());
    if (main) self->state_->fulfilled_document = value;
    if (g_getenv("JUPITER_TRACE_INTERNAL"))
      g_message("Native resource fulfilled: main=%d mime=%s bytes=%zu", main, mime.c_str(), content.size());
    self->command("Fetch.fulfillRequest", "{\"requestId\":" + quote(id) + ",\"responseCode\":200,\"responseHeaders\":["
      "{\"name\":\"Content-Type\",\"value\":" + quote(mime) + "},"
      "{\"name\":\"Cache-Control\",\"value\":\"no-store\"},"
      "{\"name\":\"Referrer-Policy\",\"value\":\"no-referrer\"},"
      "{\"name\":\"X-Frame-Options\",\"value\":\"DENY\"},"
      "{\"name\":\"X-Content-Type-Options\",\"value\":\"nosniff\"}],\"body\":" + quote(encoded) + "}");
  });
}
bool Page::send_page_message(const std::string& expected, const std::string& name, const std::string& json) {
  if (!is_internal_page(expected) || state_->context_unique.empty() || name.size() > 128 || json.size() > 1048576) return false;
  command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(state_->context_unique) + ",\"expression\":" +
    quote("globalThis.__jupiterDispatch?.(" + quote(name) + "," + rewrite_resources(json, state_->internal_origin) + ")") + "}");
  return true;
}
void Page::refresh_history() {
  auto weak = weak_from_this();
  command("Page.getNavigationHistory", "{}", [weak](JsonObject* result, const std::string&) {
    auto self = weak.lock(); if (!self || !result) return;
    auto* entries = json_object_get_array_member(result, "entries"); int index = json_object_get_int_member(result, "currentIndex");
    self->can_back = index > 0; self->can_forward = entries && index + 1 < int(json_array_get_length(entries));
    if (self->changed) self->changed();
  });
}
void Page::history(int delta) {
  auto weak = weak_from_this(); command("Page.getNavigationHistory", "{}", [weak, delta](JsonObject* result, const std::string&) {
    auto self = weak.lock(); if (!self || !result) return;
    auto* entries = json_object_get_array_member(result, "entries"); int index = json_object_get_int_member(result, "currentIndex") + delta;
    if (entries && index >= 0 && index < int(json_array_get_length(entries))) {
      auto* item = json_array_get_object_element(entries, index);
      self->command("Page.navigateToHistoryEntry", "{\"entryId\":" + std::to_string(json_object_get_int_member(item, "id")) + "}");
    }
  });
}
void Page::back() { history(-1); } void Page::forward() { history(1); }
void Page::reload(bool bypass) { command("Page.reload", bypass ? "{\"ignoreCache\":true}" : "{}"); }
void Page::stop() { command("Page.stopLoading"); }
void Page::handle(const std::string& method, JsonObject* params) {
  if (state_->disposed || !params) return;
  if (method == "Page.frameAttached") state_->process->frame_owners[text(params, "frameId")] = shared_from_this();
  else if (method == "Page.frameDetached" && std::string(text(params, "reason")) == "remove") state_->process->frame_owners.erase(text(params, "frameId"));
  else if (method == "Page.frameNavigated") {
    auto* frame = object(params, "frame");
    if (frame) state_->process->frame_owners[text(frame, "id")] = shared_from_this();
  }
  if (g_getenv("JUPITER_TRACE_LOADS") && (method == "Page.frameStartedLoading" || method == "Page.frameStoppedLoading" ||
      method == "Page.frameNavigated" || method == "Page.navigatedWithinDocument" || method == "Page.loadEventFired")) {
    auto* frame = object(params, "frame");
    g_message("Brave load trace: %s frame=%s main=%s loading=%d", method.c_str(), frame ? text(frame,"id") : text(params,"frameId"),
      state_->main_frame.c_str(), loading);
  }
  if (method == "Fetch.requestPaused") {
    internal_request(params);
  } else if (method == "Runtime.executionContextsCleared") {
    dismiss_context_menu(); state_->menu_contexts.clear();
    state_->context_id = 0; state_->context_unique.clear();
  } else if (method == "Runtime.executionContextDestroyed") {
    state_->menu_contexts.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    if (json_object_get_int_member_with_default(params, "executionContextId", 0) == state_->context_id) {
      state_->context_id = 0; state_->context_unique.clear();
    }
  } else if (method == "Runtime.executionContextCreated") {
    auto* context = object(params, "context"); auto* aux = object(context, "auxData");
    if (context && aux && std::string(text(context, "name")) == "JupiterContextMenu" &&
        !std::string(text(context, "uniqueId")).empty())
      state_->menu_contexts[json_object_get_int_member(context, "id")] = text(context, "uniqueId");
    if (context && aux && std::string(text(context, "origin")) == state_->internal_origin &&
        std::string(text(aux, "frameId")) == state_->main_frame && json_object_get_boolean_member_with_default(aux, "isDefault", false)) {
      state_->context_id = json_object_get_int_member(context, "id"); state_->context_unique = text(context, "uniqueId");
    }
  } else if (method == "Runtime.bindingCalled") {
    const std::string payload = text(params, "payload");
    if (std::string(text(params, "name")) == "__jupiterContextMenu" && payload.size() <= 131072 &&
        state_->menu_contexts.contains(json_object_get_int_member_with_default(params, "executionContextId", 0))) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, payload.data(), payload.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        show_context_menu(json_node_get_object(json_parser_get_root(parser)), json_object_get_int_member(params,"executionContextId"));
      return;
    }
    if (std::string(text(params, "name")) == "__jupiterNativeMessage" && is_internal_page(uri) &&
        state_->context_id != 0 && json_object_get_int_member_with_default(params, "executionContextId", 0) == state_->context_id &&
        !state_->context_unique.empty() && payload.size() <= 1048576 && page_message) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, payload.data(), payload.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        auto* message = json_node_get_object(json_parser_get_root(parser));
        const std::string name = text(message, "name");
        if (!name.empty() && name.size() <= 128 && json_object_has_member(message, "payload")) {
          g_autofree char* json = json_to_string(json_object_get_member(message, "payload"), false);
          if (g_getenv("JUPITER_TRACE_INTERNAL")) g_message("Native page message: %s", name.c_str());
          page_message(uri, name, json);
        }
      }
    }
  } else if (method == "Security.visibleSecurityStateChanged") {
    auto* security = object(params, "visibleSecurityState");
    auto* certificate = object(security, "certificateSecurityState");
    state_->security_state = text(security, "securityState");
    g_clear_object(&state_->certificate); state_->tls_errors = GTlsCertificateFlags(0);
    if (certificate) {
      auto* chain = json_object_get_array_member(certificate, "certificate");
      std::string pem;
      for (guint i = 0; chain && i < json_array_get_length(chain); ++i) {
        const char* encoded = json_array_get_string_element(chain, i); if (!encoded) continue;
        std::string der(encoded); pem += "-----BEGIN CERTIFICATE-----\n";
        for (size_t offset = 0; offset < der.size(); offset += 64) pem += der.substr(offset, 64) + '\n';
        pem += "-----END CERTIFICATE-----\n";
      }
      if (!pem.empty()) state_->certificate = g_tls_certificate_new_from_pem(pem.c_str(), pem.size(), nullptr);
      const std::string error = text(certificate, "certificateNetworkError");
      unsigned flags = 0;
      if (error.find("COMMON_NAME_INVALID") != std::string::npos) flags |= G_TLS_CERTIFICATE_BAD_IDENTITY;
      else if (error.find("AUTHORITY_INVALID") != std::string::npos) flags |= G_TLS_CERTIFICATE_UNKNOWN_CA;
      else if (error.find("REVOKED") != std::string::npos) flags |= G_TLS_CERTIFICATE_REVOKED;
      else if (error.find("DATE_INVALID") != std::string::npos) {
        const double now = double(g_get_real_time()) / G_USEC_PER_SEC;
        if (json_object_get_double_member_with_default(certificate, "validFrom", 0) > now) flags |= G_TLS_CERTIFICATE_NOT_ACTIVATED;
        if (json_object_get_double_member_with_default(certificate, "validTo", now) < now) flags |= G_TLS_CERTIFICATE_EXPIRED;
        if (!flags) flags |= G_TLS_CERTIFICATE_GENERIC_ERROR;
      } else if (!error.empty()) flags |= G_TLS_CERTIFICATE_GENERIC_ERROR;
      if (json_object_get_boolean_member_with_default(certificate, "certificateHasWeakSignature", false)) flags |= G_TLS_CERTIFICATE_INSECURE;
      state_->tls_errors = GTlsCertificateFlags(flags);
    }
    if (tls_changed) tls_changed();
  } else if (method == "Page.windowOpen") {
    const std::string value = text(params, "url");
    if ((value.starts_with("https://") || value.starts_with("http://")) &&
        json_object_get_boolean_member_with_default(params, "userGesture", false) && open_uri && open_uri(value)) {
      state_->process->routed_windows[value] = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
      g_message("Brave link window routed into the GTK shell");
    }
  } else if (method == "Page.frameStartedLoading" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    loading = true; progress = 0.1; state_->response_status = 0;
    if (changed) changed();
  } else if (method == "Page.frameNavigated") {
    auto* frame = object(params, "frame");
    if (frame && !json_object_has_member(frame, "parentId")) {
      // The bootstrap's commit can be delivered after attaching the mapped
      // surface and requesting the native document. It is not that navigation.
      if (std::string(text(frame, "url")) == state_->bootstrap) {
        state_->main_frame = text(frame, "id");
        if (event) event(method, params);
        return;
      }
      g_clear_object(&state_->certificate); state_->security_state.clear(); state_->tls_errors = GTlsCertificateFlags(0);
      state_->main_frame = text(frame, "id");
      state_->context_id = 0; state_->context_unique.clear();
      if (state_->attached) {
        uri = canonical_uri(text(frame, "url")); state_->requested = uri; progress = loading ? 0.1 : 1;
        const std::string loader = text(frame, "loaderId");
        const bool restored = state_->internal_loaders.contains(loader) && state_->internal_loaders[loader] == uri;
        state_->committed_document = internal_document(uri) && (uri == state_->fulfilled_document || restored) ? uri : "";
        if (!state_->committed_document.empty() && !loader.empty()) state_->internal_loaders[loader] = uri;
        if (!state_->committed_document.empty()) state_->authorized_document.clear();
        if (navigation_started) navigation_started(uri);
        if (changed) changed();
        // A cached history commit can arrive after frameStoppedLoading. Do
        // not restart its spinner, and finish the newly created shell resource.
        if (!loading) {
          refresh_history();
          if (load_finished) load_finished(state_->response_status);
        }
        state_->zoom_pending = true; apply_zoom();
      }
    }
  } else if (method == "Network.responseReceived" && state_->attached &&
             std::string(text(params, "type")) == "Document" &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    auto* response = object(params, "response");
    if (response) state_->response_status = json_object_get_int_member(response, "status");
  } else if (method == "Page.navigatedWithinDocument" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    uri = canonical_uri(text(params, "url")); refresh_history(); if (changed) changed();
  } else if (method == "Page.frameStoppedLoading" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    // DOM load events do not run again for back/forward-cache restores, and
    // aborted navigations may never dispatch one. Track the frame's actual
    // loading lifetime instead; child frames must not finish the main page.
    bool was_loading = loading;
    loading = false; progress = 1; refresh_history(); if (changed) changed();
    if (was_loading && load_finished) load_finished(state_->response_status);
    apply_zoom();
    if (is_internal_page(uri) && g_getenv("JUPITER_TRACE_INTERNAL")) {
      command("Runtime.evaluate", "{\"returnByValue\":true,\"expression\":\"({overview:!!document.getElementById('overview'),stylesheets:document.styleSheets.length,bridge:typeof Jupiter,ready:document.readyState})\"}",
        [](JsonObject* result, const std::string& error) {
          if (!error.empty()) { g_warning("Native document inspection failed: %s", error.c_str()); return; }
          auto* value = object(result, "result");
          if (value && json_object_has_member(value, "value")) {
            g_autofree char* json = json_to_string(json_object_get_member(value, "value"), false);
            g_message("Native document inspection: %s", json);
          }
        });
    }
  }
  if (event) event(method, params);
}
namespace {
std::string window_title(Display* display, ::Window window) {
  Atom type; int format; unsigned long count, remaining; unsigned char* data = nullptr;
  std::string result;
  if (XGetWindowProperty(display, window, XInternAtom(display, "_NET_WM_NAME", false), 0, 4096, false, AnyPropertyType,
      &type, &format, &count, &remaining, &data) == Success && data) { if (format == 8) result.assign(reinterpret_cast<char*>(data), count); XFree(data); }
  return result;
}
::Window find_window(Display* display, ::Window root, const std::string& window_class,
                     const std::string& token, unsigned depth = 0) {
  if (depth > 5) return 0;
  XClassHint hint{};
  bool matches = false;
  if (XGetClassHint(display, root, &hint)) {
    // Mutter's decoration frame copies the client's title. Only Brave's own
    // WM_CLASS identifies the client; reparenting the decoration instead leaves
    // a managed ghost window and the page clipped to the old frame's bounds.
    matches = hint.res_class && window_class == hint.res_class &&
      ((hint.res_name && std::string(hint.res_name).find(token) != std::string::npos) ||
       window_title(display, root).find("JupiterTab-" + token) != std::string::npos);
    if (hint.res_class) XFree(hint.res_class);
    if (hint.res_name) XFree(hint.res_name);
  }
  bool normal = false;
  if (matches) {
    Atom type; int format; unsigned long count, remaining; unsigned char* values = nullptr;
    if (XGetWindowProperty(display, root, XInternAtom(display, "_NET_WM_WINDOW_TYPE", false),
        0, 16, false, XA_ATOM, &type, &format, &count, &remaining, &values) == Success && values) {
      if (format == 32) for (unsigned long i = 0; i < count; ++i)
        normal |= reinterpret_cast<Atom*>(values)[i] == XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", false);
      XFree(values);
    }
  }
  XWindowAttributes attributes{};
  if (matches && normal && XGetWindowAttributes(display, root, &attributes) &&
      attributes.c_class == InputOutput && attributes.map_state == IsViewable &&
      attributes.width > 200 && attributes.height > 150) return root;
  ::Window actual_root, parent, *children = nullptr; unsigned count = 0; ::Window found = 0;
  if (XQueryTree(display, root, &actual_root, &parent, &children, &count)) {
    for (unsigned i = 0; i < count && !found; ++i) found = find_window(display, children[i], window_class, token, depth + 1);
    if (children) XFree(children);
  }
  return found;
}
}
void Page::layout() {
  auto& s = *state_; if (s.disposed || !s.display) return;
  if (!s.process || !s.process->integration_ready) return;
  auto* root_widget = gtk_widget_get_root(s.content); if (!root_widget || !GTK_IS_NATIVE(root_widget)) return;
  auto* surface = gtk_native_get_surface(GTK_NATIVE(root_widget)); if (!GDK_IS_X11_SURFACE(surface)) return;
  ::Window parent = gdk_x11_surface_get_xid(surface);
  if (s.parent != parent) { s.parent = parent; if (s.container) XReparentWindow(s.display, s.container, parent, 0, 0); }
  if (!s.child) {
    s.child = find_window(s.display, DefaultRootWindow(s.display), s.process->window_class, s.token);
    if (!s.child) return;
    XWithdrawWindow(s.display, s.child, DefaultScreen(s.display)); XUnmapWindow(s.display, s.child); XSync(s.display, false);
    s.withdrawn_until = g_get_monotonic_time() + 300000; return;
  }
  if (!s.container) {
    if (g_get_monotonic_time() < s.withdrawn_until || session.empty()) return;
    XSetWindowAttributes attributes{}; attributes.override_redirect = True; XChangeWindowAttributes(s.display, s.child, CWOverrideRedirect, &attributes);
    s.container = XCreateSimpleWindow(s.display, s.parent, 0, 0, 1, 1, 0, 0, 0);
    XReparentWindow(s.display, s.child, s.container, 0, 0); XSetWindowBorderWidth(s.display, s.child, 0);
    unsigned long extents[] = {0, 0, 0, 0};
    for (auto* name : {"_GTK_FRAME_EXTENTS", "_NET_FRAME_EXTENTS"})
      XChangeProperty(s.display, s.child, XInternAtom(s.display, name, false), XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char*>(extents), 4);
    struct { unsigned long flags, functions, decorations; long input; unsigned long status; } hints{2, 0, 0, 0, 0};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_MOTIF_WM_HINTS", false), XInternAtom(s.display, "_MOTIF_WM_HINTS", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(&hints), 5);
    Atom states[] = {XInternAtom(s.display, "_NET_WM_STATE_SKIP_TASKBAR", false), XInternAtom(s.display, "_NET_WM_STATE_SKIP_PAGER", false)};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_NET_WM_STATE", false), XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(states), 2);
    unsigned long embed_info[] = {0, 1};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_XEMBED_INFO", false), XInternAtom(s.display, "_XEMBED_INFO", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(embed_info), 2);
    unsigned long withdrawn[] = {WithdrawnState, None};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "WM_STATE", false), XInternAtom(s.display, "WM_STATE", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(withdrawn), 2);
    XSetTransientForHint(s.display, s.child, s.parent); XMapWindow(s.display, s.child);
    // A synchronous passive grab performs click-to-focus, then replays the actual
    // pointer event to Brave. It does not emulate mouse buttons or keyboard text.
    XGrabButton(s.display, Button1, AnyModifier, s.child, false, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    s.attached = true; navigate(s.requested);
    g_message("Brave native tab embedded: target=%s parent=%lu container=%lu child=%lu", target.c_str(), s.parent, s.container, s.child);
  }
  const bool mapped = s.mapped && gtk_widget_get_mapped(s.content);
  if (!mapped) { XUnmapWindow(s.display, s.container); XFlush(s.display); return; }
  XMapWindow(s.display, s.container);
  ::Window actual_root, actual_parent, *children = nullptr; unsigned count = 0;
  if (XQueryTree(s.display, s.child, &actual_root, &actual_parent, &children, &count)) {
    if (children) XFree(children);
    if (actual_parent != s.container) {
      XSetWindowAttributes attributes{}; attributes.override_redirect = True;
      XUnmapWindow(s.display, s.child);
      XChangeWindowAttributes(s.display, s.child, CWOverrideRedirect, &attributes);
      XReparentWindow(s.display, s.child, s.container, 0, 0); XMapWindow(s.display, s.child);
    }
  }
  graphene_point_t zero{0, 0}, point{}; if (!gtk_widget_compute_point(s.content, GTK_WIDGET(root_widget), &zero, &point)) return;
  double x, y; gtk_native_get_surface_transform(GTK_NATIVE(root_widget), &x, &y);
  int scale = gtk_widget_get_scale_factor(s.content), width = gtk_widget_get_width(s.content) * scale, height = gtk_widget_get_height(s.content) * scale;
  if (width > 0 && height > 0) {
    XMoveResizeWindow(s.display, s.container, (point.x + x) * scale, (point.y + y) * scale, width, height);
    if (s.window >= 0 && (s.width != width || s.height != height)) {
      s.process->command("Browser.setWindowBounds", "{\"windowId\":" + std::to_string(s.window) + ",\"bounds\":{\"left\":0,\"top\":0,\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}}", "");
      s.width = width; s.height = height;
    }
    XMoveResizeWindow(s.display, s.child, 0, 0, width, height); XRaiseWindow(s.display, s.container);
  }
  while (XPending(s.display)) {
    XEvent event; XNextEvent(s.display, &event);
    if (event.type == ButtonPress) { focus(); XAllowEvents(s.display, ReplayPointer, event.xbutton.time); }
  }
  XFlush(s.display);
}
bool Page::edit(const std::string& name) {
  auto& s = *state_;
  return s.surface && JUPITER_IS_COMPOSITOR_VIEW(s.surface) &&
    jupiter_compositor_view_edit(JUPITER_COMPOSITOR_VIEW(s.surface), name.c_str());
}
void Page::focus() {
  auto& s = *state_;
  if (s.surface) { gtk_widget_grab_focus(s.surface); return; }
  if (!s.display || !s.child || s.disposed) return;
  gtk_widget_grab_focus(s.content);
  // The WM only knows the GTK toplevel. Giving server focus to the foreign
  // child makes Mutter deactivate GTK. Retain native focus in the host and
  // deliver its real keyboard events to the embedded client instead.
  XEvent active{}; active.xclient.type = ClientMessage; active.xclient.window = s.parent;
  active.xclient.message_type = XInternAtom(s.display, "_NET_ACTIVE_WINDOW", false);
  active.xclient.format = 32; active.xclient.data.l[0] = 1;
  XSendEvent(s.display, DefaultRootWindow(s.display), false, SubstructureRedirectMask | SubstructureNotifyMask, &active);
  XSetInputFocus(s.display, s.parent, RevertToParent, CurrentTime);
  XEvent event{}; event.xfocus.type = FocusIn; event.xfocus.window = s.child;
  event.xfocus.mode = NotifyNormal; event.xfocus.detail = NotifyAncestor;
  XSendEvent(s.display, s.child, false, FocusChangeMask, &event); XFlush(s.display);
}
void Page::send_key(GdkEvent* source) {
  auto& s = *state_; if (!source || !s.display || !s.child || s.disposed) return;
  auto type = gdk_event_get_event_type(source);
  if (type != GDK_KEY_PRESS && type != GDK_KEY_RELEASE) return;
  XEvent event{}; event.xkey.type = type == GDK_KEY_PRESS ? KeyPress : KeyRelease;
  event.xkey.display = s.display; event.xkey.window = s.child;
  event.xkey.root = DefaultRootWindow(s.display); event.xkey.same_screen = True;
  event.xkey.time = gdk_event_get_time(source);
  event.xkey.keycode = gdk_key_event_get_keycode(source);
  auto modifiers = gdk_event_get_modifier_state(source);
  if (modifiers & GDK_SHIFT_MASK) event.xkey.state |= ShiftMask;
  if (modifiers & GDK_LOCK_MASK) event.xkey.state |= LockMask;
  if (modifiers & GDK_CONTROL_MASK) event.xkey.state |= ControlMask;
  if (modifiers & GDK_ALT_MASK) event.xkey.state |= Mod1Mask;
  if (modifiers & GDK_SUPER_MASK) event.xkey.state |= Mod4Mask;
  XSendEvent(s.display, s.child, false, type == GDK_KEY_PRESS ? KeyPressMask : KeyReleaseMask, &event);
  XFlush(s.display);
}
void Page::dismiss_context_menu() {
  if (state_->context_menu) gtk_popover_popdown(GTK_POPOVER(state_->context_menu));
}
void Page::set_zoom(double factor) {
  if (is_closed() || !std::isfinite(factor)) return;
  state_->zoom_requested = std::clamp(factor,.1,10.);
  ++state_->zoom_revision; state_->zoom_pending = true; apply_zoom();
}
void Page::receive_zoom(double factor) {
  auto& s = *state_;
  if (s.disposed || loading || s.zoom_busy || s.zoom_pending || !std::isfinite(factor) || factor < .1 || factor > 10) return;
  s.zoom_requested = s.zoom_applied = factor;
  if (zoom_changed) zoom_changed(factor);
}
void Page::apply_zoom() {
  auto& s = *state_;
  if (s.disposed || loading || s.zoom_busy || !s.zoom_pending || !s.attached || s.window < 0 || !s.process->integration_ready) return;
  if (uri == "about:blank" && s.zoom_requested == 1) { s.zoom_pending = false; return; }
  s.zoom_pending = false; s.zoom_busy = true;
  auto weak = weak_from_this();
  const guint64 revision = s.zoom_revision;
  const std::string expected = uri;
  // Browser and extension window IDs share Chromium's SessionID. App windows
  // have exactly one tab; never fall back to whichever tab happens to be active.
  const std::string script = "(async()=>{const tabs=await chrome.tabs.query({windowId:" + std::to_string(s.window) +
    "});if(tabs.length!==1)throw Error('The native browser window has no unique tab');const id=tabs[0].id;"
    "await chrome.tabs.setZoomSettings(id,{mode:'automatic',scope:'per-tab'});await chrome.tabs.setZoom(id," +
    std::to_string(s.zoom_requested) + ");return {tabId:id,factor:await chrome.tabs.getZoom(id)};})()";
  s.process->command("Runtime.evaluate","{\"expression\":" + quote(script) + ",\"awaitPromise\":true,\"returnByValue\":true}",
    s.process->integration_session,[weak,revision,expected](JsonObject* result,const std::string& error) {
      auto self = weak.lock(); if (!self || self->is_closed()) return;
      auto& s = *self->state_; s.zoom_busy = false;
      auto* value = object(object(result,"result"),"value");
      const bool current = expected == self->uri && revision == s.zoom_revision;
      if (!value || object(result,"exceptionDetails")) {
        if (current) {
          g_warning("Native Brave zoom failed: %s",error.empty() ? text(object(object(result,"exceptionDetails"),"exception"),"description") : error.c_str());
          s.zoom_requested = s.zoom_applied;
          if (self->zoom_changed) self->zoom_changed(s.zoom_applied);
        }
      } else {
        const int tab = json_object_get_int_member_with_default(value,"tabId",-1);
        const double factor = json_object_get_double_member_with_default(value,"factor",0);
        if (tab >= 0 && std::isfinite(factor) && factor >= .1 && factor <= 10) {
          if (s.native_tab >= 0) s.process->zoom_tabs.erase(s.native_tab);
          s.native_tab = tab; s.process->zoom_tabs[tab] = self; s.zoom_applied = factor;
          if (current && self->zoom_changed) self->zoom_changed(factor);
        }
      }
      self->apply_zoom();
    });
}
void Page::show_context_menu(JsonObject* hit, int context) {
  auto& s = *state_;
  if (s.disposed || !s.surface || !gtk_widget_get_mapped(s.surface)) return;
  dismiss_context_menu();
  auto* popover = gtk_popover_menu_new_from_model(nullptr);
  s.context_menu = popover;
  gtk_widget_set_parent(popover, s.surface);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  auto* actions = g_simple_action_group_new();
  auto* menu = g_menu_new();
  auto weak = weak_from_this();
  const std::string expected = uri;
  const std::string document = s.menu_contexts.at(context);
  auto current = [weak, expected, context, document]() -> std::shared_ptr<Page> {
    auto page = weak.lock();
    if (!page || page->is_closed() || page->uri != expected || !page->state_->mapped) return {};
    auto found = page->state_->menu_contexts.find(context);
    return found != page->state_->menu_contexts.end() && found->second == document ? page : nullptr;
  };
  int index = 0;
  auto append = [&](GMenu* section, const char* label, std::function<void(Page&)> callback, bool enabled = true, const char* icon = nullptr) {
    const std::string name = "item" + std::to_string(index++);
    auto* action = g_simple_action_new(name.c_str(), nullptr);
    g_simple_action_set_enabled(action, enabled);
    auto* invoke = new std::function<void()>([current, callback = std::move(callback)] {
      if (auto page = current()) {
        page->dismiss_context_menu();
        // The popover must release its keyboard grab before native edit chords.
        auto* pending = new std::function<void()>([current, callback] {
          if (auto page = current()) callback(*page);
        });
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
          (*static_cast<std::function<void()>*>(data))(); return G_SOURCE_REMOVE;
        }, pending, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
      }
    });
    g_signal_connect_data(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
      (*static_cast<std::function<void()>*>(data))();
    }), invoke, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, GConnectFlags(0));
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action)); g_object_unref(action);
    auto* item = g_menu_item_new(label, ("native-context." + name).c_str());
    if (icon) {
      auto* themed = g_themed_icon_new(icon);
      auto* serialized = g_icon_serialize(G_ICON(themed));
      g_menu_item_set_attribute_value(item,"verb-icon",serialized);
      g_variant_unref(serialized); g_object_unref(themed);
    }
    g_menu_append_item(section,item); g_object_unref(item);
  };
  auto section = [&] { return g_menu_new(); };
  auto finish = [&](GMenu* value) {
    if (g_menu_model_get_n_items(G_MENU_MODEL(value))) g_menu_append_section(menu, nullptr, G_MENU_MODEL(value));
    g_object_unref(value);
  };
  auto shell_action = [](const char* name, std::string value) {
    return [name, value](Page& page) {
      gtk_widget_activate_action(page.widget(), name, "s", value.c_str());
    };
  };
  auto copy = [](std::string value) {
    return [value](Page& page) { gdk_clipboard_set_text(gtk_widget_get_clipboard(page.widget()), value.c_str()); };
  };
  auto navigable = [](const std::string& value) {
    g_autofree char* scheme = g_uri_parse_scheme(value.c_str());
    return scheme && (!g_ascii_strcasecmp(scheme,"https") || !g_ascii_strcasecmp(scheme,"http"));
  };
  const std::string link = text(hit,"link"), image = text(hit,"image"), selection = text(hit,"selection");
  const bool editable = json_object_get_boolean_member_with_default(hit,"editable",false);
  auto* links = section();
  if (!link.empty()) {
    if (navigable(link)) {
      append(links, _("Open Link in New _Tab"), shell_action("popup.open-selection-in-new-tab",link));
      append(links, _("Open Link in New _Window"), shell_action("popup.open-selection-in-new-window",link));
      if (!s.ephemeral) append(links, _("Open Link in I_ncognito Window"), shell_action("popup.open-selection-in-incognito-window",link));
    }
    append(links, _("Copy Link _Address"), copy(link));
  }
  if (!image.empty()) {
    if (navigable(image)) append(links, _("Open Image in New Tab"), shell_action("popup.open-selection-in-new-tab",image));
    append(links, _("Copy Image _Address"), copy(image));
  }
  finish(links);
  auto* editing = section();
  if (editable) {
    append(editing, _("_Undo"), [](Page& page) { page.edit("Undo"); });
    append(editing, _("Re_do"), [](Page& page) { page.edit("Redo"); });
    append(editing, _("Cu_t"), [](Page& page) { page.edit("Cut"); }, !selection.empty());
    append(editing, _("_Copy"), [](Page& page) { page.edit("Copy"); }, !selection.empty());
    append(editing, _("_Paste"), [](Page& page) { page.edit("Paste"); });
    append(editing, _("Select _All"), [](Page& page) { page.edit("SelectAll"); });
  } else if (!selection.empty()) append(editing, _("_Copy"), copy(selection));
  if (!selection.empty()) append(editing, _("Search the Web for Selection"), shell_action("popup.search-selection",selection));
  finish(editing);
  if (link.empty() && image.empty() && selection.empty() && !editable) {
    auto* navigation = section();
    append(navigation, _("_Back"), [](Page& page) { page.back(); }, can_back,"go-previous-symbolic");
    append(navigation, _("_Forward"), [](Page& page) { page.forward(); }, can_forward,"go-next-symbolic");
    append(navigation, _("_Reload"), [](Page& page) { page.reload(); }, true,"view-refresh-symbolic");
    append(navigation, _("Add Boo_kmark…"), [](Page& page) { gtk_widget_activate_action(page.widget(), "popup.context-bookmark-page", nullptr); },true,"non-starred-symbolic");
    auto* navigation_row = g_menu_item_new_section(nullptr,G_MENU_MODEL(navigation));
    g_menu_item_set_attribute(navigation_row,"display-hint","s","horizontal-buttons");
    g_menu_append_item(menu,navigation_row); g_object_unref(navigation_row); g_object_unref(navigation);
    auto* page_actions = section();
    append(page_actions, _("Select _All"), [](Page& page) { page.edit("SelectAll"); });
    finish(page_actions);
  }
  gtk_widget_insert_action_group(popover,"native-context",G_ACTION_GROUP(actions)); g_object_unref(actions);
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(popover),G_MENU_MODEL(menu)); g_object_unref(menu);
  // GTK's horizontal menu section distributes equal-width buttons across the
  // row. Keep them flat rather than using the section's default linked style.
  std::function<void(GtkWidget*)> widen_navigation = [&](GtkWidget* widget) {
    if (gtk_widget_has_css_class(widget,"horizontal-buttons")) {
      gtk_widget_remove_css_class(widget,"linked");
      for (auto* button = gtk_widget_get_first_child(widget); button; button = gtk_widget_get_next_sibling(button)) {
        gtk_widget_add_css_class(button,"flat");
        gtk_widget_set_hexpand(button,TRUE);
        gtk_widget_set_size_request(button,48,-1);
      }
      return;
    }
    for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) widen_navigation(child);
  };
  widen_navigation(popover);
  g_signal_connect_data(popover,"closed",G_CALLBACK(+[](GtkPopover* popover, gpointer data) {
    if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock(); page && page->state_->context_menu == GTK_WIDGET(popover))
      page->state_->context_menu = nullptr;
    gtk_widget_unparent(GTK_WIDGET(popover));
  }), new std::weak_ptr<Page>(weak), +[](gpointer data,GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, GConnectFlags(0));
  double x = json_object_get_double_member_with_default(hit,"x",0), y = json_object_get_double_member_with_default(hit,"y",0);
  if (json_object_get_boolean_member_with_default(hit,"pointer",false))
    jupiter_compositor_view_context_position(JUPITER_COMPOSITOR_VIEW(s.surface),&x,&y);
  GdkRectangle rect = {int(std::clamp(x,0.0,double(gtk_widget_get_width(s.surface)))),
                       int(std::clamp(y,0.0,double(gtk_widget_get_height(s.surface)))),1,1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover),&rect);
  gtk_widget_set_halign(popover, GTK_ALIGN_START);
  auto* scroller = gtk_popover_get_child(GTK_POPOVER(popover));
  int natural_width = 0, natural_height = 0;
  // The popover itself is still hidden, so GTK would measure it as zero.
  // Its visible menu child supplies the actual natural content dimensions.
  gtk_widget_measure(scroller, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural_width, nullptr, nullptr);
  gtk_widget_measure(scroller, GTK_ORIENTATION_VERTICAL, natural_width, nullptr, &natural_height, nullptr, nullptr);
  graphene_point_t anchor = GRAPHENE_POINT_INIT(float(rect.x),float(rect.y)), root_anchor;
  auto* root = GTK_WIDGET(gtk_widget_get_root(s.surface));
  if (root && gtk_widget_compute_point(s.surface,root,&anchor,&root_anchor)) {
    const double above = root_anchor.y, below = gtk_widget_get_height(root) - root_anchor.y;
    gtk_popover_set_position(GTK_POPOVER(popover), natural_height > below && above > below ? GTK_POS_TOP : GTK_POS_BOTTOM);
  }
  // GTK's own scrolled menu remains natural-sized until the monitor limit.
  // Its popup layout asks the desktop compositor to flip vertically before
  // resizing. Wayland intentionally does not expose the window's screen origin.
  auto* native = gtk_widget_get_native(s.surface);
  auto* monitor = native ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(s.surface),gtk_native_get_surface(native)) : nullptr;
  if (GTK_IS_SCROLLED_WINDOW(scroller) && monitor) {
    GdkRectangle geometry; gdk_monitor_get_geometry(monitor,&geometry);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller),std::max(1,geometry.height));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
  }
  jupiter_compositor_view_release_buttons(JUPITER_COMPOSITOR_VIEW(s.surface));
  gtk_popover_popup(GTK_POPOVER(popover));
}
void Page::set_visible(bool visible) {
  if (!visible) dismiss_context_menu();
  state_->mapped = visible;
  if (state_->surface) gtk_widget_set_visible(state_->surface, visible);
  if (state_->display && state_->container) { if (visible) layout(); else XUnmapWindow(state_->display, state_->container); }
}
void Page::close() {
  if (state_->process) std::erase_if(state_->process->frame_owners, [this](const auto& entry) {
    auto page = entry.second.lock(); return !page || page.get() == this;
  });
  if (!state_ || state_->disposed) return;
  auto& s = *state_; s.disposed = true;
  if (s.process && s.native_tab >= 0) s.process->zoom_tabs.erase(s.native_tab);
  dismiss_context_menu();
  if (s.keys) { gtk_widget_remove_controller(s.content, s.keys); s.keys = nullptr; }
  changed = {}; closed = {}; failed = {}; navigation_started = {}; open_uri = {}; load_finished = {}; tls_changed = {}; fullscreen_changed = {}; zoom_changed = {}; event = {};
  if (s.timer) { g_source_remove(s.timer); s.timer = 0; }
  if (s.surface) {
    g_signal_handlers_disconnect_by_data(s.surface, this);
    const bool keep = s.process && !s.process->closing && !s.native_destroyed && !target.empty() && !session.empty() && s.process->parked.empty();
    if (keep) {
      g_object_ref(s.surface);
      s.process->command("Page.stopLoading", "{}", session);
      s.process->command("Page.navigate", "{\"url\":\"about:blank\"}", session);
      s.process->parked.push_back({target, session, 0, s.surface});
    } else if (s.process && !s.process->closing && !s.native_destroyed && !target.empty()) {
      s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
    }
    gtk_box_remove(GTK_BOX(s.content), s.surface); s.surface = nullptr;
    if (s.process) { s.process->pages.erase(s.token); s.process->targets.erase(target); s.process->sessions.erase(session); }
    return;
  }
  const bool park = s.process && !s.process->closing && !s.native_destroyed && s.display && s.child && !target.empty() && !session.empty();
  if (park) {
    XUnmapWindow(s.display, s.child);
    XReparentWindow(s.display, s.child, DefaultRootWindow(s.display), 0, 0);
    XDeleteProperty(s.display, s.child, XInternAtom(s.display, "WM_TRANSIENT_FOR", false));
    s.process->command("Page.stopLoading", "{}", session);
    s.process->command("Page.navigate", "{\"url\":\"about:blank\"}", session);
    // One reusable hidden page is sufficient to retain Chromium's app lifetime.
    // Close further parked pages so closing many tabs releases their renderers.
    if (s.process->parked.empty()) s.process->parked.push_back({target, session, s.child});
    else s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
  } else if (s.process && !s.process->closing && !s.native_destroyed && !target.empty()) {
    s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
  }
  if (s.process) { s.process->pages.erase(s.token); s.process->targets.erase(target); s.process->sessions.erase(session); }
  if (s.display) { if (s.container) { XUnmapWindow(s.display, s.container); XDestroyWindow(s.display, s.container); } XFlush(s.display); XCloseDisplay(s.display); s.display = nullptr; }
}
void Page::snapshot(std::function<void(GdkTexture*, const std::string&)> done, bool full_document) {
  auto weak = weak_from_this();
  const std::string expected = uri;
  Reply receive = [weak, expected, done = std::move(done)](JsonObject* result, const std::string& error) {
    auto self = weak.lock();
    if (!self || self->is_closed() || self->uri != expected) { done(nullptr, "The page changed during snapshot capture"); return; }
    if (!result) { done(nullptr, error); return; }
    gsize size = 0; guchar* data = g_base64_decode(text(result, "data"), &size);
    auto* bytes = g_bytes_new_take(data, size); GError* failure = nullptr; auto* texture = gdk_texture_new_from_bytes(bytes, &failure); g_bytes_unref(bytes);
    done(texture, failure ? failure->message : ""); g_clear_error(&failure); g_clear_object(&texture);
  };
  if (!full_document) { command("Page.captureScreenshot", "{\"format\":\"png\",\"captureBeyondViewport\":false}", std::move(receive)); return; }
  command("Page.getLayoutMetrics", "{}", [weak, expected, receive = std::move(receive)](JsonObject* result, const std::string& error) {
    auto self = weak.lock();
    if (!self || self->is_closed() || self->uri != expected) { receive(nullptr, "The page changed during snapshot capture"); return; }
    auto* viewport = object(result, "cssLayoutViewport"); auto* content = object(result, "cssContentSize");
    if (!viewport || !content) { receive(nullptr, error.empty() ? "The page has no snapshot dimensions" : error); return; }
    const double width = json_object_get_double_member_with_default(viewport, "clientWidth", 0);
    const double available = json_object_get_double_member_with_default(content, "height", 0);
    // Epiphany caches 650x540 snapshots and crops their top into overview
    // cards. A wide viewport alone is too short; include more document below
    // it without resizing/reflowing the live page or capturing its full length.
    const double height = std::min(available, width * 540.0 / 650.0);
    if (width <= 0 || height <= 0) { receive(nullptr, "Invalid snapshot dimensions"); return; }
    const double scale = std::min(1.0, 1300.0 / width);
    std::string params = "{\"format\":\"png\",\"captureBeyondViewport\":true,\"clip\":{\"x\":0,\"y\":0,\"width\":" +
      std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"scale\":" + std::to_string(scale) + "}}";
    self->command("Page.captureScreenshot", params, std::move(receive));
  });
}
bool Page::tls_info(GTlsCertificate** certificate, GTlsCertificateFlags* errors) const {
  if (certificate) *certificate = state_->certificate ? G_TLS_CERTIFICATE(g_object_ref(state_->certificate)) : nullptr;
  if (errors) *errors = state_->tls_errors;
  return state_->certificate != nullptr;
}
bool Page::tls_secure() const { return state_->certificate && state_->tls_errors == 0 && state_->security_state == "secure"; }
}
