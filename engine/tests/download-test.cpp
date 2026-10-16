/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "ephy-engine-download.h"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <string>

static void wait_for(const std::function<bool()>& ready) {
  auto deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
  while (!ready() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  g_assert_true(ready());
}
static std::string read(const std::string& path) {
  char *bytes = nullptr; gsize size = 0; GError *error = nullptr;
  g_assert_true(g_file_get_contents(path.c_str(), &bytes, &size, &error));
  g_assert_no_error(error);
  std::string result(bytes, size); g_free(bytes); return result;
}
struct Result {
  EphyEngineDownload *download = nullptr;
  GError *error = nullptr;
  bool done = false, progress = false, cancel_progress = false;
  ~Result() { if (download) { g_signal_handlers_disconnect_by_data(download, this); g_object_unref(download); } g_clear_error(&error); }
  void attach(EphyEngineDownload *value) {
    download = EPHY_ENGINE_DOWNLOAD(g_object_ref(value));
    g_signal_connect(download, "finished", G_CALLBACK(+[](EphyEngineDownload*, Result *r) { r->done = true; }), this);
    g_signal_connect(download, "failed", G_CALLBACK(+[](EphyEngineDownload*, GError *error, Result *r) { r->error = g_error_copy(error); r->done = true; }), this);
    g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(+[](EphyEngineDownload *d, GParamSpec*, Result *r) {
      if (ephy_engine_download_get_received_data_length(d) > 0) {
        r->progress = true;
        if (r->cancel_progress) ephy_engine_download_cancel(d);
      }
    }), this);
  }
  void wait() { wait_for([this] { return done; }); }
};
static void transfer(EphyEngineSession *session, const std::string& uri, const std::string& destination,
                      Result& result, bool overwrite = false) {
  auto *download = ephy_engine_session_download_uri(session, uri.c_str());
  result.attach(download);
  ephy_engine_download_set_allow_overwrite(download, overwrite);
  ephy_engine_download_set_destination(download, destination.c_str());
  g_object_unref(download);
}
static void set_cookie(EphyEngineSession *session, const char *value) {
  auto *cookie = soup_cookie_new("download", value, "127.0.0.1", "/", 3600);
  bool done = false;
  ephy_engine_session_add_cookie(session, cookie, nullptr,
    +[](GObject *source, GAsyncResult *result, gpointer user) {
      GError *error = nullptr;
      g_assert_true(ephy_engine_session_add_cookie_finish(EPHY_ENGINE_SESSION(source), result, &error));
      g_assert_no_error(error); *static_cast<bool*>(user) = true;
    }, &done);
  soup_cookie_free(cookie); wait_for([&] { return done; });
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.DownloadTest", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, nullptr));
  char scratch[] = "/tmp/jupiter-download-test-XXXXXX";
  g_assert_nonnull(mkdtemp(scratch));
  g_assert_true(ephy_engine_initialize(argc, argv, scratch, true));
  disable_diagnostic_core_dumps();
  auto *session = ephy_engine_session_get_default();
  std::string base = g_getenv("JUPITER_TEST_URL"), dir = scratch;
  const std::string payload("CEF download payload\0with binary bytes", 38);
  {
    Result result;
    transfer(session, base + "download-redirect", dir + "/binary", result);
    result.wait(); g_assert_no_error(result.error);
    g_assert_true(read(dir + "/binary") == payload);
    g_assert_true(result.progress);
    g_assert_true(ephy_engine_download_get_uri(result.download) == base + "download-redirect");
    g_assert_true(ephy_engine_download_get_response_uri(result.download) == base + "download");
    g_assert_cmpstr(ephy_engine_download_get_mime_type(result.download), ==, "application/octet-stream");
    g_print("CEF download bytes, redirects, metadata and progress: PASS\n");
  }
  {
    Result result;
    transfer(session, base + "download", dir + "/binary", result);
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_assert_true(read(dir + "/binary") == payload);
    g_print("Download refuses an existing destination: PASS\n");
  }
  {
    g_assert_true(g_file_set_contents((dir + "/binary").c_str(), "original", -1, nullptr));
    Result result;
    transfer(session, base + "download", dir + "/binary", result, true);
    result.wait(); g_assert_no_error(result.error);
    g_assert_true(read(dir + "/binary") == payload);
    g_print("Explicit overwrite replaces the destination: PASS\n");
  }
  {
    Result result;
    transfer(session, base + "download-slow", dir + "/cancelled", result);
    result.cancel_progress = true;
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_false(g_file_test((dir + "/cancelled").c_str(), G_FILE_TEST_EXISTS));
    g_print("Active download cancellation does not publish partial data: PASS\n");
  }
  {
    Result result;
    transfer(session, base + "download-pending", dir + "/cancel-before-headers", result);
    bool elapsed = false;
    g_timeout_add(300, +[](gpointer p) -> gboolean { *static_cast<bool*>(p) = true; return G_SOURCE_REMOVE; }, &elapsed);
    wait_for([&] { return elapsed; });
    ephy_engine_download_cancel(result.download);
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_print("Cancellation before response headers does not wait for the server: PASS\n");
  }
  {
    Result result;
    transfer(session, base + "download", dir + "/cancelled-early", result);
    ephy_engine_download_cancel(result.download);
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_print("Cancellation before Chromium starts: PASS\n");
  }
  {
    Result result;
    transfer(session, "not a URL", dir + "/invalid", result);
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_print("Invalid download addresses fail without leaving a browser running: PASS\n");
  }
  {
    Result result;
    auto *download = ephy_engine_session_download_uri(session, (base + "download").c_str());
    result.attach(download); g_object_unref(download);
    bool deciding = false;
    g_signal_connect(download, "decide-destination", G_CALLBACK(+[](EphyEngineDownload*, const char*, bool *deciding) -> gboolean {
      *deciding = true; return true;
    }), &deciding);
    wait_for([&] { return deciding; });
    ephy_engine_download_cancel(download);
    result.wait(); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_signal_handlers_disconnect_by_data(download, &deciding);
    g_print("Cancellation while the destination chooser is pending: PASS\n");
  }
  {
    Result result;
    transfer(session, base + "download-fail", dir + "/failed", result);
    result.wait(); g_assert_nonnull(result.error);
    g_assert_false(g_file_test((dir + "/failed").c_str(), G_FILE_TEST_EXISTS));
    g_print("HTTP failure reports an error and leaves no completed file: PASS\n");
  }
  {
    set_cookie(session, "regular");
    auto *private_session = ephy_engine_session_new(true);
    set_cookie(private_session, "private");
    Result normal, incognito;
    transfer(session, base + "download-cookie", dir + "/normal-cookie", normal);
    transfer(private_session, base + "download-cookie", dir + "/private-cookie", incognito);
    normal.wait(); incognito.wait(); g_assert_no_error(normal.error); g_assert_no_error(incognito.error);
    g_assert_true(read(dir + "/normal-cookie") == "download=regular");
    g_assert_true(read(dir + "/private-cookie") == "download=private");
    g_object_add_weak_pointer(G_OBJECT(private_session), reinterpret_cast<gpointer*>(&private_session));
    g_object_unref(private_session);
    g_assert_null(private_session);
    g_print("Download requests share native cookies and isolate private sessions: PASS\n");
  }
  for (bool with_callback : {false, true}) {
    auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
    adw_application_window_set_content(window, GTK_WIDGET(view));
    bool loaded = false;
    auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *ready) {
      if (event == WEBKIT_LOAD_FINISHED) *ready = true;
    }), &loaded);
    ephy_engine_web_view_load_uri(view, base.c_str()); gtk_window_present(GTK_WINDOW(window));
    wait_for([&] { return loaded; }); g_signal_handler_disconnect(view, handler);
    Result page;
    std::string destination = dir + (with_callback ? "/page-download-callback" : "/page-download");
    struct PageResult { Result *result; std::string destination; } pending{&page, destination};
    auto started = g_signal_connect(session, "download-started", G_CALLBACK(+[](EphyEngineSession*, EphyEngineDownload *d, PageResult *pending) {
      pending->result->attach(d);
      ephy_engine_download_set_destination(d, pending->destination.c_str());
    }), &pending);
    bool evaluated = false;
    GAsyncReadyCallback callback = +[](GObject *source, GAsyncResult *result, gpointer user) {
        GError *error = nullptr;
        auto *value = ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(source), result, &error);
        g_assert_no_error(error); g_assert_nonnull(value);
        g_clear_pointer(&value,json_object_unref);
        *static_cast<bool*>(user) = true;
      };
    ephy_engine_web_view_evaluate_javascript(view, "location.href='/download-slow'", -1, nullptr, nullptr, nullptr,
                                            with_callback ? callback : nullptr, &evaluated);
    if (with_callback) wait_for([&] { return evaluated; });
    wait_for([&] { return page.download && page.progress; });
    gtk_window_destroy(GTK_WINDOW(window));
    page.wait(); g_assert_no_error(page.error);
    g_assert_true(read(destination) == std::string(512 * 1024, 'x'));
    g_signal_handler_disconnect(session, started);
    g_print("Page download %s an evaluation callback completes after its tab closes: PASS\n", with_callback ? "with" : "without");
  }
  {
    Result active, queued;
    transfer(session, base + "download-slow", dir + "/shutdown-active", active);
    wait_for([&] { return active.progress; });
    transfer(session, base + "download", dir + "/shutdown-queued", queued);
    g_assert_true(ephy_engine_download_destination_exists(queued.download, (dir + "/shutdown-active").c_str()));
    g_assert_false(ephy_engine_download_destination_exists(active.download, (dir + "/shutdown-active").c_str()));
    ephy_engine_downloads_shutdown();
    g_assert_true(active.done); g_assert_true(queued.done);
    g_assert_error(active.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_error(queued.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_print("Engine shutdown drains active and queued downloads: PASS\n");
  }
  wait_for([] { return Browser::active_count == 0; });
  for (const auto& item : std::filesystem::directory_iterator(dir))
    g_assert_false(item.path().filename().string().starts_with(".jupiter-download-"));
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(dir);
  return 0;
}
