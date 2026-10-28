/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-download.h"
#include "ephy-engine-session.h"
#include "ephy-engine-settings.h"
#include "select/browser.hpp"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::set<EphyEngineDownload*> live;
unsigned pending_browsers = 0;
struct Download {
  EphyEngineSession *session = nullptr;
  GApplication *application = nullptr;
  CefRefPtr<CefBeforeDownloadCallback> before;
  CefRefPtr<CefDownloadItemCallback> control;
  std::function<void()> stop;
  std::function<void()> native_cancel;
  std::string uri, response_uri, mime, destination, partial;
  guint64 total = 0, received = 0;
  double progress = 0;
  gint64 started = g_get_monotonic_time();
  gint64 ended = 0;
  bool overwrite = false, cancelled = false, announced = false, terminal = false, target_started = false;
  bool native = false, native_completed = false;
  GError *error = nullptr;
  ~Download() {
    if (!partial.empty()) unlink(partial.c_str());
    g_clear_error(&error);
    g_clear_object(&session);
    if (application) { g_application_release(application); g_object_unref(application); }
  }
};
using Key = std::pair<int, uint32_t>;
std::map<Key, EphyEngineDownload*> active;
std::set<Key> completed;
enum { PROP_NONE, PROP_DESTINATION, PROP_MIME, PROP_PROGRESS, PROP_COUNT };
GParamSpec *properties[PROP_COUNT];
enum { DECIDE_DESTINATION, CREATED_DESTINATION, FINISHED, FAILED, SIGNAL_COUNT };
guint signals[SIGNAL_COUNT];
}
struct _EphyEngineDownload { GObject parent; Download *data; };
G_DEFINE_TYPE(EphyEngineDownload, ephy_engine_download, G_TYPE_OBJECT)
static void ephy_engine_download_class_init(EphyEngineDownloadClass *klass) {
  auto *object = G_OBJECT_CLASS(klass);
  object->finalize = +[](GObject *object) {
    live.erase(EPHY_ENGINE_DOWNLOAD(object));
    delete EPHY_ENGINE_DOWNLOAD(object)->data;
    G_OBJECT_CLASS(ephy_engine_download_parent_class)->finalize(object);
  };
  object->get_property = +[](GObject *object, guint id, GValue *value, GParamSpec *spec) {
    auto *data = EPHY_ENGINE_DOWNLOAD(object)->data;
    switch (id) {
    case PROP_DESTINATION: g_value_set_string(value, data->destination.empty() ? nullptr : data->destination.c_str()); break;
    case PROP_MIME: g_value_set_string(value, data->mime.c_str()); break;
    case PROP_PROGRESS: g_value_set_double(value, data->progress); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
  };
  properties[PROP_DESTINATION] = g_param_spec_string("destination", nullptr, nullptr, nullptr, G_PARAM_READABLE);
  properties[PROP_MIME] = g_param_spec_string("mime-type", nullptr, nullptr, nullptr, G_PARAM_READABLE);
  properties[PROP_PROGRESS] = g_param_spec_double("estimated-progress", nullptr, nullptr, 0, 1, 0, G_PARAM_READABLE);
  g_object_class_install_properties(object, PROP_COUNT, properties);
  signals[DECIDE_DESTINATION] = g_signal_new("decide-destination", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
  signals[CREATED_DESTINATION] = g_signal_new("created-destination", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  signals[FAILED] = g_signal_new("failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_ERROR);
}
static void ephy_engine_download_init(EphyEngineDownload *download) { download->data = new Download; }
namespace {
EphyEngineDownload *create(EphyEngineSession *session, const std::string& uri) {
  auto *download = EPHY_ENGINE_DOWNLOAD(g_object_new(EPHY_TYPE_ENGINE_DOWNLOAD, nullptr));
  auto *data = download->data;
  live.insert(download);
  data->session = EPHY_ENGINE_SESSION(g_object_ref(session));
  data->uri = data->response_uri = uri;
  if (auto *application = g_application_get_default()) {
    data->application = G_APPLICATION(g_object_ref(application));
    g_application_hold(application);
  }
  return download;
}
void announce(EphyEngineDownload *download) {
  if (download->data->announced) return;
  download->data->announced = true;
  g_signal_emit_by_name(download->data->session, "download-started", download);
}
void set_error(Download *data, GIOErrorEnum code, const char *message) {
  if (!data->error) data->error = g_error_new_literal(G_IO_ERROR, code, message);
}
void finish(EphyEngineDownload *download, bool complete);
void start_target(EphyEngineDownload *download) {
  auto *data = download->data;
  if (data->native) {
    if (data->destination.empty() || data->terminal || data->cancelled || data->target_started) return;
    data->target_started = true;
    g_autofree char *uri = g_filename_to_uri(data->destination.c_str(), nullptr, nullptr);
    g_signal_emit(download, signals[CREATED_DESTINATION], 0, uri);
    if (data->native_completed && !data->terminal) finish(download, true);
    return;
  }
  if (!data->before || data->destination.empty() || data->terminal || data->cancelled || data->target_started) return;
  // Chromium writes a private sibling file. Publishing it must never clobber an
  // existing destination unless the user explicitly approved replacement.
  auto *directory = g_path_get_dirname(data->destination.c_str());
  auto *pattern = g_build_filename(directory, ".gnosis-download-XXXXXX", nullptr);
  int file = g_mkstemp_full(pattern, O_RDWR | O_CLOEXEC, 0600);
  g_free(directory);
  if (file < 0) {
    set_error(data, g_io_error_from_errno(errno), "Cannot create the download file");
    g_free(pattern);
    ephy_engine_download_cancel(download);
    return;
  }
  close(file);
  data->partial = pattern; g_free(pattern);
  data->target_started = true;
  auto before = data->before; data->before = nullptr;
  auto *uri = g_filename_to_uri(data->destination.c_str(), nullptr, nullptr);
  g_signal_emit(download, signals[CREATED_DESTINATION], 0, uri);
  g_free(uri);
  if (data->cancelled) return;
  before->Continue(data->partial, false);
}
void finish(EphyEngineDownload *download, bool complete) {
  g_object_ref(download);
  auto *data = download->data;
  data->terminal = true;
  data->ended = g_get_monotonic_time();
  live.erase(download);
  announce(download);
  g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_MIME]);
  if (complete && !data->cancelled && !data->error) {
    auto *source = g_file_new_for_path(data->partial.c_str());
    auto *destination = g_file_new_for_path(data->destination.c_str());
    g_file_move(source, destination, data->overwrite ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE,
                nullptr, nullptr, nullptr, &data->error);
    g_object_unref(source); g_object_unref(destination);
  } else if (!data->error) {
    set_error(data, data->cancelled ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
              data->cancelled ? "Download cancelled" : "Chromium interrupted the download");
  }
  if (!data->partial.empty()) { unlink(data->partial.c_str()); data->partial.clear(); }
  data->before = nullptr; data->control = nullptr;
  data->stop = nullptr;
  data->native_cancel = nullptr;
  if (data->error) g_signal_emit(download, signals[FAILED], 0, data->error);
  else g_signal_emit(download, signals[FINISHED], 0);
  g_clear_object(&data->session);
  if (data->application) {
    g_application_release(data->application);
    g_clear_object(&data->application);
  }
  std::erase_if(active, [download](const auto& entry) {
    if (entry.second != download) return false;
    g_object_unref(entry.second); return true;
  });
  g_object_unref(download);
}
EphyEngineDownload *lookup(EphyEngineSession *session, CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDownloadItem> item, EphyEngineDownload *existing) {
  Key key{browser->GetIdentifier(), item->GetId()};
  auto found = active.find(key);
  if (found != active.end()) return found->second;
  auto *download = existing ? EPHY_ENGINE_DOWNLOAD(g_object_ref(existing)) : create(session, item->GetOriginalUrl());
  active[key] = download;
  return download;
}
void metadata(EphyEngineDownload *download, CefRefPtr<CefDownloadItem> item) {
  auto *data = download->data;
  if (!item->GetOriginalUrl().empty()) data->uri = item->GetOriginalUrl();
  data->response_uri = item->GetURL();
  data->total = std::max<int64_t>(0, item->GetTotalBytes());
  data->received = std::max<int64_t>(0, item->GetReceivedBytes());
  auto mime = item->GetMimeType().ToString();
  if (data->mime != mime) { data->mime = mime; g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_MIME]); }
  data->progress = item->GetPercentComplete() < 0 ? 0 : std::clamp(item->GetPercentComplete() / 100., 0., 1.);
  g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_PROGRESS]);
}
class DownloadClient final : public CefClient, public CefLifeSpanHandler, public CefRenderHandler,
                             public CefDownloadHandler, public CefRequestHandler {
public:
  explicit DownloadClient(EphyEngineDownload *download) : download_(EPHY_ENGINE_DOWNLOAD(g_object_ref(download))),
    cookie_filter_(ephy_engine_session_cookie_filter(download->data->session)),
    request_handler_(ephy_engine_settings_request_handler(cookie_filter_,
      ephy_engine_session_request_handler(download->data->session))) { ++pending_browsers; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
      CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) override {
    return request_handler_;
  }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1, 1); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    ++Browser::active_count;
    download_->data->stop = [browser] { browser->GetHost()->CloseBrowser(true); };
    if (download_->data->cancelled) {
      finish(download_, false);
      browser->GetHost()->CloseBrowser(true);
      return;
    }
    browser->GetHost()->StartDownload(download_->data->uri);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    ephy_engine_download_browser_closed(browser->GetIdentifier());
    --Browser::active_count;
    --pending_browsers; closed_ = true;
  }
  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item, const CefString& name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override {
    ephy_engine_download_before(download_->data->session, browser, item, name, callback, download_); return true;
  }
  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item,
                         CefRefPtr<CefDownloadItemCallback> callback) override {
    ephy_engine_download_updated(download_->data->session, browser, item, callback, download_);
    if (item->IsComplete() || item->IsCanceled() || item->IsInterrupted()) browser->GetHost()->CloseBrowser(true);
  }
private:
  ~DownloadClient() override { if (!closed_) --pending_browsers; g_object_unref(download_); }
  EphyEngineDownload *download_;
  CefRefPtr<CefCookieAccessFilter> cookie_filter_;
  CefRefPtr<CefResourceRequestHandler> request_handler_;
  bool closed_ = false;
  IMPLEMENT_REFCOUNTING(DownloadClient);
};
}
EphyEngineDownload *ephy_engine_download_native_begin(EphyEngineSession *session, const std::string& uri,
    const std::string& suggested, const std::string& staging, std::function<void()> cancel) {
  auto *download = create(session, uri);
  auto *data = download->data;
  data->native = true; data->native_cancel = std::move(cancel); data->partial = staging;
  g_autofree char *name = g_path_get_basename(suggested.empty() ? "download" : suggested.c_str());
  g_autofree char *content_type = g_content_type_guess(name, nullptr, 0, nullptr);
  g_autofree char *mime = content_type ? g_content_type_get_mime_type(content_type) : nullptr;
  data->mime = mime ? mime : "application/octet-stream";
  announce(download);
  g_message("Brave download connected to GTK manager");
  g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_MIME]);
  if (data->terminal) return download;
  gboolean handled = false;
  g_signal_emit(download, signals[DECIDE_DESTINATION], 0, name, &handled);
  if (!handled && data->destination.empty()) ephy_engine_download_cancel(download);
  else start_target(download);
  return download;
}
void ephy_engine_download_native_update(EphyEngineDownload *download, guint64 received, guint64 total,
                                         const std::string& state) {
  auto *data = download->data;
  if (!data->native || data->terminal) return;
  data->received = received; data->total = total;
  data->progress = total ? std::clamp(double(received) / total, 0., 1.) : 0;
  if (state == "completed") data->progress = 1;
  g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_PROGRESS]);
  if (state == "completed") {
    g_message("Brave download completed: %" G_GUINT64_FORMAT " bytes", received);
    data->native_completed = true;
    // The GTK destination chooser may still be open after a fast transfer.
    if (data->target_started) finish(download, true);
  } else if (state == "canceled") finish(download, false);
  else if (state == "failed") ephy_engine_download_native_fail(download, "The Brave download service stopped");
}
void ephy_engine_download_native_fail(EphyEngineDownload *download, const std::string& message) {
  if (download->data->terminal) return;
  set_error(download->data, G_IO_ERROR_FAILED, message.c_str()); finish(download, false);
}
void ephy_engine_download_before(EphyEngineSession *session, CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item,
                                const CefString& name, CefRefPtr<CefBeforeDownloadCallback> before, EphyEngineDownload *existing) {
  if (existing && existing->data->terminal) return;
  if (completed.contains({browser->GetIdentifier(), item->GetId()})) return;
  auto *download = lookup(session, browser, item, existing);
  auto *data = download->data;
  if (data->terminal) return;
  data->before = before;
  metadata(download, item);
  announce(download);
  // Consumers connect to MIME notifications when the session announces the download.
  g_object_notify_by_pspec(G_OBJECT(download), properties[PROP_MIME]);
  if (data->cancelled) { data->before = nullptr; if (data->control) data->control->Cancel(); return; }
  gboolean handled = false;
  g_signal_emit(download, signals[DECIDE_DESTINATION], 0, name.ToString().c_str(), &handled);
  if (!handled && data->destination.empty()) { ephy_engine_download_cancel(download); return; }
  start_target(download);
}
void ephy_engine_download_updated(EphyEngineSession *session, CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item,
                                 CefRefPtr<CefDownloadItemCallback> control, EphyEngineDownload *existing) {
  if (existing && existing->data->terminal) return;
  if (completed.contains({browser->GetIdentifier(), item->GetId()})) return;
  auto *download = lookup(session, browser, item, existing);
  auto *data = download->data;
  if (data->terminal) return;
  data->control = control;
  metadata(download, item);
  if (data->cancelled && item->IsInProgress()) control->Cancel();
  if (!item->IsComplete() && !item->IsCanceled() && !item->IsInterrupted()) return;
  if (item->IsInterrupted()) {
    switch (item->GetInterruptReason()) {
    case CEF_DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
      set_error(data, G_IO_ERROR_NO_SPACE, "Not enough disk space to finish the download"); break;
    case CEF_DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
    case CEF_DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN:
      set_error(data, G_IO_ERROR_PERMISSION_DENIED, "Permission to download this file was denied"); break;
    case CEF_DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
      set_error(data, G_IO_ERROR_TIMED_OUT, "The download connection timed out"); break;
    case CEF_DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
      set_error(data, G_IO_ERROR_NOT_CONNECTED, "The network connection was lost"); break;
    case CEF_DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED:
      set_error(data, G_IO_ERROR_PERMISSION_DENIED, "Authentication is required to download this file"); break;
    default:
      set_error(data, G_IO_ERROR_FAILED, "The download was interrupted");
    }
  }
  data->terminal = true;
  completed.insert({browser->GetIdentifier(), item->GetId()});
  if (item->IsCanceled()) data->cancelled = true;
  struct End { EphyEngineDownload *download; bool complete; };
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
    auto *end = static_cast<End*>(p);
    finish(end->download, end->complete);
    return G_SOURCE_REMOVE;
  }, new End{EPHY_ENGINE_DOWNLOAD(g_object_ref(download)), item->IsComplete()},
  +[](gpointer p) { auto *end = static_cast<End*>(p); g_object_unref(end->download); delete end; });
}
EphyEngineDownload *ephy_engine_session_download_uri(EphyEngineSession *session, const char *uri) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session) && uri && *uri, nullptr);
  auto *download = create(session, uri);
  // Start on the next GTK turn so callers can connect signals and choose a destination.
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
    auto *download = EPHY_ENGINE_DOWNLOAD(p);
    auto *data = download->data;
    if (data->cancelled) { data->terminal = true; finish(download, false); return G_SOURCE_REMOVE; }
    CefURLParts parts;
    if (!CefParseURL(data->uri, parts)) {
      set_error(data, G_IO_ERROR_INVALID_ARGUMENT, "Invalid download address");
      finish(download, false); return G_SOURCE_REMOVE;
    }
    CefWindowInfo info; info.SetAsWindowless(0);
    CefBrowserSettings settings;
    auto context = ephy_engine_session_context(data->session);
    if (!context || !CefBrowserHost::CreateBrowser(info, new DownloadClient(download), "about:blank", settings, nullptr, context)) {
      data->terminal = true;
      set_error(data, G_IO_ERROR_FAILED, "Cannot start Chromium's download service");
      finish(download, false);
    }
    return G_SOURCE_REMOVE;
  }, g_object_ref(download), g_object_unref);
  return download;
}
const char *ephy_engine_download_get_uri(EphyEngineDownload *d) { return d->data->uri.c_str(); }
const char *ephy_engine_download_get_response_uri(EphyEngineDownload *d) { return d->data->response_uri.c_str(); }
const char *ephy_engine_download_get_mime_type(EphyEngineDownload *d) { return d->data->mime.empty() ? nullptr : d->data->mime.c_str(); }
guint64 ephy_engine_download_get_content_length(EphyEngineDownload *d) { return d->data->total; }
guint64 ephy_engine_download_get_received_data_length(EphyEngineDownload *d) { return d->data->received; }
double ephy_engine_download_get_estimated_progress(EphyEngineDownload *d) { return d->data->progress; }
double ephy_engine_download_get_elapsed_time(EphyEngineDownload *d) { return ((d->data->ended ? d->data->ended : g_get_monotonic_time()) - d->data->started) / double(G_TIME_SPAN_SECOND); }
const char *ephy_engine_download_get_destination(EphyEngineDownload *d) { return d->data->destination.empty() ? nullptr : d->data->destination.c_str(); }
void ephy_engine_download_set_destination(EphyEngineDownload *d, const char *path) {
  g_return_if_fail(EPHY_IS_ENGINE_DOWNLOAD(d) && path && g_path_is_absolute(path));
  if (d->data->target_started || d->data->terminal) return;
  d->data->destination = path;
  g_object_notify_by_pspec(G_OBJECT(d), properties[PROP_DESTINATION]);
  start_target(d);
}
gboolean ephy_engine_download_get_allow_overwrite(EphyEngineDownload *d) { return d->data->overwrite; }
void ephy_engine_download_set_allow_overwrite(EphyEngineDownload *d, gboolean overwrite) { d->data->overwrite = overwrite; }
gboolean ephy_engine_download_destination_exists(EphyEngineDownload *d, const char *path) {
  if (g_file_test(path, G_FILE_TEST_EXISTS)) return true;
  return std::any_of(live.begin(), live.end(), [d, path](auto *other) {
    return other != d && other->data->destination == path;
  });
}
void ephy_engine_download_cancel(EphyEngineDownload *d) {
  if (d->data->terminal) return;
  d->data->cancelled = true;
  if (d->data->native) {
    auto cancel = std::move(d->data->native_cancel);
    if (cancel) cancel();
    finish(d, false); return;
  }
  d->data->before = nullptr;
  if (d->data->control) d->data->control->Cancel();
  else if (d->data->stop) {
    // Before response headers arrive, CEF has not supplied a download callback.
    d->data->terminal = true;
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
      auto *download = EPHY_ENGINE_DOWNLOAD(p);
      auto stop = std::move(download->data->stop);
      stop();
      finish(download, false);
      return G_SOURCE_REMOVE;
    }, g_object_ref(d), g_object_unref);
  }
}
void ephy_engine_downloads_shutdown() {
  // Native cancellation finishes synchronously and removes the object from
  // live. Hold a snapshot while callbacks update the manager and application.
  std::vector<EphyEngineDownload*> pending(live.begin(), live.end());
  for (auto *download : pending) g_object_ref(download);
  for (auto *download : pending) { ephy_engine_download_cancel(download); g_object_unref(download); }
  auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while ((!live.empty() || pending_browsers) && g_get_monotonic_time() < deadline) {
    if (!GnosisBrave::enabled()) CefDoMessageLoopWork();
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
}
void ephy_engine_download_browser_closed(int browser_id) {
  std::erase_if(completed, [browser_id](const auto& key) { return key.first == browser_id; });
}
