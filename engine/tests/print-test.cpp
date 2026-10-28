/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include "ephy-engine-view.h"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <glib-unix.h>

static void wait_for(const std::function<bool()>& predicate) {
  auto deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false); g_usleep(1000);
  }
  g_assert_true(predicate());
}
int main(int argc, char** argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int child = ephy_engine_execute_process(argc, argv); if (child >= 0) return child;
  auto* app = adw_application_new("com.neg-zero.Gnosis.PrintTest", G_APPLICATION_NON_UNIQUE);
  GError* error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error)); g_assert_no_error(error);
  char profile[] = "/tmp/gnosis-print-test-XXXXXX"; g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true)); disable_diagnostic_core_dumps();
  auto* window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_title(GTK_WINDOW(window), "Gnosis print portal contract");
  gtk_window_set_default_size(GTK_WINDOW(window), 700, 500);
  auto* view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool* loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, g_getenv("GNOSIS_TEST_URL"));
  gtk_window_present(GTK_WINDOW(window)); wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, handler);
  auto* page = gtk_page_setup_new(); auto* settings = gtk_print_settings_new();
  const char* output = g_getenv("GNOSIS_TEST_PRINT_FILE");
  g_assert_nonnull(output);
  char* output_uri = g_filename_to_uri(output, nullptr, &error);
  g_assert_no_error(error); g_assert_nonnull(output_uri);
  // This test must never submit a job to a physical printer.
  gtk_print_settings_set_printer(settings, "Print to File");
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, output_uri);
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
  g_free(output_uri);
  for (const char* phase : {"SAVE", "CANCEL", "HIDE", "CLOSE"}) {
    struct Result { bool ready = false; GtkPrintSetup* setup = nullptr; GError* error = nullptr; } result;
    struct Action { GtkWidget* view; GtkWindow* window; const char* phase; } action{GTK_WIDGET(view), GTK_WINDOW(window), phase};
    guint signal = g_unix_signal_add(SIGUSR1, +[](gpointer data) -> gboolean {
      auto* action = static_cast<Action*>(data);
      if (!g_strcmp0(action->phase, "HIDE")) gtk_widget_set_visible(action->view, false);
      if (!g_strcmp0(action->phase, "CLOSE")) gtk_window_destroy(action->window);
      return G_SOURCE_CONTINUE;
    }, &action);
    g_print("PRINT_%s_READY\n", phase); fflush(stdout);
    ephy_engine_web_view_print(view, page, settings, +[](GObject* source, GAsyncResult* reply, gpointer data) {
      auto* result = static_cast<Result*>(data);
      result->setup = ephy_engine_web_view_print_finish(EPHY_ENGINE_VIEW(source), reply, &result->error);
      result->ready = true;
    }, &result);
    wait_for([&] { return result.ready; }); g_source_remove(signal);
    if (!g_strcmp0(phase, "SAVE")) {
      g_assert_no_error(result.error); g_assert_nonnull(result.setup);
      g_set_object(&page, gtk_print_setup_get_page_setup(result.setup));
      g_set_object(&settings, gtk_print_setup_get_print_settings(result.setup));
    } else if (!g_strcmp0(phase, "CANCEL")) {
      g_assert_null(result.setup); g_assert_error(result.error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
    } else {
      g_assert_null(result.setup); g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
    if (result.setup) gtk_print_setup_unref(result.setup);
    g_clear_error(&result.error);
    g_print("PRINT_%s_PASS\n", phase); fflush(stdout);
    if (!g_strcmp0(phase, "HIDE")) gtk_widget_set_visible(GTK_WIDGET(view), true);
  }
  wait_for([] { return Browser::active_count == 0; });
  g_object_unref(page); g_object_unref(settings);
  ephy_engine_shutdown(); g_object_unref(app); std::filesystem::remove_all(profile);
}
