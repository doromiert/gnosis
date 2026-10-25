/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <glib-unix.h>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);

class FilterResult final : public CefRunFileDialogCallback {
 public:
  explicit FilterResult(std::function<void(const std::vector<CefString>&)> done) : done_(std::move(done)) {}
  void OnFileDialogDismissed(const std::vector<CefString>& paths) override { done_(paths); }
 private:
  std::function<void(const std::vector<CefString>&)> done_;
  IMPLEMENT_REFCOUNTING(FilterResult);
};

struct Test {
  EphyEngineView *view;
  GMainLoop *loop;
  int stage = -2;
  bool files_only = false;
  bool started = false, finished = false, failed = false;
  EphyEngineView *private_view = nullptr;
  bool memory_write = false, memory_read = false;
  bool regular_ready = false, private_ready = true;
  unsigned memory_step = 0;
  void maybe_start() {
    if (started || !regular_ready || !private_ready) return;
    started = true;
    gtk_widget_grab_focus(GTK_WIDGET(view));
    next();
  }
  void memory_next() {
    static const char *write_steps[] = {"MEMORY_REGULAR_PICK", "MEMORY_REGULAR_REUSE",
      "MEMORY_PRIVATE_PICK", "MEMORY_PRIVATE_REUSE", "MEMORY_REGULAR_AFTER_PRIVATE",
      "MEMORY_EXPLICIT", "MEMORY_REGULAR_AFTER_CANCEL"};
    static const char *read_steps[] = {"MEMORY_RESTART_REGULAR", "MEMORY_RESTART_PRIVATE"};
    const auto count = memory_write ? std::size(write_steps) : std::size(read_steps);
    if (memory_step == count) {
      finished = true;
      g_main_loop_quit(loop);
      return;
    }
    const bool private_request = memory_write ? (memory_step == 2 || memory_step == 3) : memory_step == 1;
    auto browser = ephy_engine_view_browser(private_request ? private_view : view);
    if (!browser) { fail("No native Chromium browser for upload-folder memory"); return; }
    const char *name = memory_write ? write_steps[memory_step] : read_steps[memory_step];
    std::string default_path;
    if (memory_write && memory_step == 5)
      default_path = std::string(g_getenv("JUPITER_TEST_SCRATCH")) + "/files/alpha.txt";
    const auto step = memory_step;
    browser->GetHost()->RunFileDialog(FILE_DIALOG_OPEN, name, default_path, {},
      new FilterResult([this, step](const auto& paths) {
        if (finished) return;
        if (memory_write && (step == 0 || step == 2)) {
          const std::string suffix = step == 0 ? "/native-filter/alpha.JUP" : "/private-files/private.txt";
          char *contents = nullptr;
          const char *expected = step == 0 ? "native filter payload" : "private folder payload";
          const bool valid = paths.size() == 1 && paths.front().ToString().ends_with(suffix) &&
            g_file_get_contents(paths.front().ToString().c_str(), &contents, nullptr, nullptr) &&
            !g_strcmp0(contents, expected);
          g_free(contents);
          if (!valid) { fail("Upload-folder memory picker returned a different file or bytes"); return; }
        } else if (!paths.empty()) {
          fail("A cancelled upload-folder memory chooser returned files"); return;
        }
        ++memory_step;
        // Return from CEF's dismissal callback before requesting the next
        // dialog; Chromium may still be unwinding its per-browser dialog slot.
        g_idle_add_once(+[](gpointer user) {
          auto *test = static_cast<Test*>(user);
          if (!test->finished) test->memory_next();
        }, this);
      }));
    g_print("PORTAL_%s_READY\n", name);
  }

  void fail(const char *message) {
    g_printerr("Portal test: %s\n", message);
    failed = true; finished = true;
    g_main_loop_quit(loop);
  }
  void evaluate(const std::string& script, std::function<void(const std::string&)> done) {
    struct Reply { Test *test; std::function<void(const std::string&)> done; };
    ephy_engine_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer user) {
        std::unique_ptr<Reply> reply(static_cast<Reply*>(user));
        GError *error = nullptr;
        auto *value = ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(source), result, &error);
        if (reply->test->finished) { g_clear_pointer(&value,json_object_unref); g_clear_error(&error); return; }
        if (error || !value) {
          reply->test->fail(error ? error->message : "No JavaScript result");
          g_clear_error(&error); g_clear_pointer(&value,json_object_unref); return;
        }
        char *text = g_strdup(script_result_text(value).c_str());
        std::string output(text); g_free(text); json_object_unref(value);
        reply->done(output);
      }, new Reply{this, std::move(done)});
  }
  void next() {
    if (memory_write || memory_read) { memory_next(); return; }
    if (stage == -2) {
      auto browser = ephy_engine_view_browser(view);
      if (!browser) { fail("No native Chromium browser for the file filter fixture"); return; }
      browser->GetHost()->RunFileDialog(FILE_DIALOG_OPEN, "Jupiter native filter fixture", "",
        {"Jupiter native filter fixture|.jup;.jupiter"}, new FilterResult([this](const auto& paths) {
          if (finished) return;
          char *contents = nullptr;
          bool valid = paths.size() == 1 && paths.front().ToString().ends_with("/alpha.JUP") &&
            g_file_get_contents(paths.front().ToString().c_str(), &contents, nullptr, nullptr) &&
            !strcmp(contents, "native filter payload");
          g_free(contents);
          if (!valid) { fail("The named native filter did not return the selected upper-case extension file"); return; }
          g_print("Native Chromium named extension filter returns the actual upper-case file and contents: PASS\n");
          stage = -1; next();
        }));
      g_print("PORTAL_FILTER_READY\n");
      return;
    }
    if (stage == -1) {
      evaluate(R"JS(document.body.innerHTML='<button id=save style=position:fixed;left:20px;top:20px;width:400px;height:80px>Save binary fixture</button>';
        window.portalSave=new Promise((resolve,reject)=>{
          save.onclick=async()=>{
            try {
              const handle=await showSaveFilePicker({suggestedName:'saved-δ.txt',
                types:[{description:'Text fixture',accept:{'text/plain':['.txt']}}]});
              const writable=await handle.createWritable();
              await writable.write(new Uint8Array([0,10,13,65,127,128,255,206,180]));
              await writable.close();
              resolve(handle.name);
            } catch(error) { reject(error); }
          };
        });'ready')JS", [this](const auto&) {
          evaluate("window.portalSave", [this](const std::string& name) {
            if (name != "saved-δ.txt") { fail("The save portal did not preserve the chosen UTF-8 filename"); return; }
            g_print("Native Chromium writable file handle saves binary bytes through the UTF-8 portal path: PASS\n");
            stage = 0; next();
          });
          g_print("PORTAL_SAVE_READY\n");
        });
      return;
    }
    if (files_only && stage == 2) stage++;
    static const char *names[] = {"SINGLE", "MULTIPLE", "FOLDER", "CANCEL", "HIDE", "CLOSE"};
    std::string script;
    if (!stage) script = "document.body.innerHTML='<input style=position:fixed;left:20px;top:20px;width:400px;height:80px type=file id=upload accept=.txt>';";
    script += "upload.value='';upload.multiple=" + std::string(stage == 1 ? "true" : "false") +
      ";upload.webkitdirectory=" + (stage == 2 ? "true" : "false") + ";upload.focus();";
    script += R"JS(window.portalResult=new Promise((resolve,reject)=>{
      upload.onchange=async()=>{
        try {
          const files=[...upload.files];
          resolve(JSON.stringify({contents:await Promise.all(files.map(f=>f.text())),paths:files.map(f=>f.webkitRelativePath)}));
        } catch(error) {reject(error);}
      };
      upload.oncancel=()=>resolve('cancelled');
    });'ready')JS";
    evaluate(script, [this](const auto&) {
      if (stage == 5) {
        g_print("PORTAL_CLOSE_READY\n");
        return;
      }
      // Leave GTK and Chromium on their normal event loops while the chooser is open.
      evaluate("window.portalResult", [this](const std::string& output) {
        static const char *expected[] = {
          "{\"contents\":[\"portal payload alpha\"],\"paths\":[\"\"]}",
          "{\"contents\":[\"portal payload alpha\",\"portal payload beta\"],\"paths\":[\"\",\"\"]}",
          "{\"contents\":[\"folder payload\"],\"paths\":[\"folder/inside.txt\"]}",
          "cancelled", "cancelled"};
        if (output != expected[stage]) { fail(("Unexpected upload result: " + output).c_str()); return; }
        g_print("Portal %s upload/cancellation in actual Chromium: PASS\n", names[stage]);
        if (stage == 4) gtk_widget_set_visible(GTK_WIDGET(view), true);
        stage++; next();
      });
      g_print("PORTAL_%s_READY\n", names[stage]);
    });
  }
};
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  auto *app = adw_application_new("io.github.doromiert.Jupiter.PortalTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  if (!g_application_register(G_APPLICATION(app), nullptr, &error)) { g_printerr("%s\n", error->message); return 1; }
  const bool memory_write = argc > 1 && !strcmp(argv[1], "--folder-memory-write");
  const bool memory_read = argc > 1 && !strcmp(argv[1], "--folder-memory-read");
  const bool memory_mode = memory_write || memory_read;
  char temporary_profile[] = "/tmp/jupiter-portal-test-XXXXXX";
  const char *profile = memory_mode ? g_getenv("JUPITER_PORTAL_PROFILE") : mkdtemp(temporary_profile);
  if (!profile || !*profile || !ephy_engine_initialize(argc, argv, profile, true)) return 1;
  disable_diagnostic_core_dumps();
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_title(GTK_WINDOW(window), "Jupiter Portal Test");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 650);
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  Test test{view, g_main_loop_new(nullptr, false)};
  test.files_only = argc > 1 && !strcmp(argv[1], "--files-only");
  test.memory_write = memory_write; test.memory_read = memory_read;
  GtkWindow *private_window = nullptr;
  g_autoptr(EphyEngineSession) private_session = nullptr;
  if (memory_mode) {
    private_session = ephy_engine_session_new(true);
    private_window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(private_window, "Jupiter Private Portal Test");
    gtk_window_set_default_size(private_window, 900, 650);
    test.private_view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW,
      "network-session", private_session, nullptr));
    test.private_ready = false;
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(private_window), GTK_WIDGET(test.private_view));
    g_signal_connect(test.private_view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, Test *test) {
      if (event == WEBKIT_LOAD_FINISHED) { test->private_ready = true; test->maybe_start(); }
    }), &test);
    ephy_engine_web_view_load_uri(test.private_view, g_getenv("JUPITER_TEST_URL"));
    gtk_window_present(private_window);
  }

  auto close_signal = g_unix_signal_add(SIGUSR1, +[](gpointer user) -> gboolean {
    auto *test = static_cast<Test*>(user);
    if (test->stage == 4) gtk_widget_set_visible(GTK_WIDGET(test->view), false);
    else if (test->stage == 5) { test->finished = true; g_main_loop_quit(test->loop); }
    else test->fail("Hide/close requested before the pending chooser test");
    return G_SOURCE_CONTINUE;
  }, &test);
  g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, Test *test) {
    if (event != WEBKIT_LOAD_FINISHED || test->started) return;
    test->regular_ready = true;
    test->maybe_start();
  }), &test);
  ephy_engine_web_view_load_uri(view, g_getenv("JUPITER_TEST_URL"));
  gtk_window_present(GTK_WINDOW(window));
  auto timeout = g_timeout_add_seconds(30, +[](gpointer user) -> gboolean {
    static_cast<Test*>(user)->fail("Timed out waiting for a portal operation"); return G_SOURCE_REMOVE;
  }, &test);
  g_main_loop_run(test.loop);
  g_source_remove(close_signal);
  if (!test.failed) g_source_remove(timeout);
  gtk_window_destroy(GTK_WINDOW(window));
  if (private_window) gtk_window_destroy(private_window);
  auto cleanup = g_timeout_add(20, +[](gpointer user) -> gboolean {
    if (Browser::active_count) return G_SOURCE_CONTINUE;
    g_main_loop_quit(static_cast<Test*>(user)->loop); return G_SOURCE_REMOVE;
  }, &test);
  auto cleanup_timeout = g_timeout_add_seconds(5, +[](gpointer user) -> gboolean {
    g_main_loop_quit(static_cast<Test*>(user)->loop); return G_SOURCE_REMOVE;
  }, &test);
  g_main_loop_run(test.loop);
  if (Browser::active_count) { g_printerr("Browser did not close\n"); std::_Exit(1); }
  g_source_remove(cleanup_timeout); (void)cleanup;
  ephy_engine_shutdown(); g_main_loop_unref(test.loop); g_object_unref(app);
  if (!test.failed) g_print("%s", memory_mode ? "Native upload-folder memory phase closes cleanly: PASS\n" :
    "Closing a tab with a pending native portal exits cleanly: PASS\n");
  if (!memory_mode) std::filesystem::remove_all(profile);
  return test.failed ? 1 : 0;
}
