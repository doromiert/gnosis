/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-origin.h"
#include "include/cef_api_hash.h"
#include <initializer_list>
#include <filesystem>
#include "include/cef_app.h"

static void origins() {
  struct Case { const char *uri, *origin; guint port; };
  for (auto item : {
    Case{"https://User:secret@EXAMPLE.test:443/path?q=1#f", "https://example.test", 0},
    Case{"http://EXAMPLE.test:80/", "http://example.test", 0},
    Case{"https://example.test:8443/", "https://example.test:8443", 8443},
    Case{"https://bücher.example/", "https://xn--bcher-kva.example", 0},
    Case{"http://[2001:db8::1]:8080/", "http://[2001:db8::1]:8080", 8080},
    Case{"blob:https://example.test/id", "https://example.test", 0},
    Case{"BLOB:https://example.test/id", "https://example.test", 0},
    Case{"filesystem:https://example.test/temporary/file", "https://example.test", 0}}) {
    auto *origin = ephy_engine_origin_new_for_uri(item.uri);
    g_assert_nonnull(origin);
    char *text = ephy_engine_origin_to_string(origin);
    g_assert_cmpstr(text, ==, item.origin);
    g_assert_cmpuint(ephy_engine_origin_get_port(origin), ==, item.port);
    g_free(text);
    ephy_engine_origin_unref(origin);
  }
  for (const char *uri : {"about:blank", "data:text/html,hello", "blob:null/id", "file:///tmp/file", "file://server/share", "not a url", "https://example.test:99999/"})
    g_assert_null(ephy_engine_origin_new_for_uri(uri));
  auto *origin = ephy_engine_origin_new("https", "example.test", 443);
  char *text = ephy_engine_origin_to_string(origin);
  g_assert_cmpstr(text, ==, "https://example.test");
  g_free(text);
  ephy_engine_origin_unref(origin);
}
int main(int argc, char **argv) {
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = CefExecuteProcess(CefMainArgs(argc, argv), nullptr, nullptr);
  if (sub >= 0) return sub;
  char profile[] = "/tmp/jupiter-origin-check-XXXXXX";
  if (!mkdtemp(profile)) return 1;
  CefSettings settings;
  settings.no_sandbox = true; // Isolated diagnostic, never ordinary browsing.
  settings.windowless_rendering_enabled = true;
  settings.external_message_pump = true;
  CefString(&settings.root_cache_path) = profile;
  CefString(&settings.resources_dir_path) = CEF_RESOURCE_DIR;
  CefString(&settings.locales_dir_path) = std::string(CEF_RESOURCE_DIR) + "/locales";
  if (!CefInitialize(CefMainArgs(argc, argv), settings, nullptr, nullptr)) return 1;
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/engine/origin/chromium-canonicalization", origins);
  int result = g_test_run();
  CefShutdown();
  std::filesystem::remove_all(profile);
  return result;
}
