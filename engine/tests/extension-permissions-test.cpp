/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "extension-permissions.hpp"
int main() {
  g_assert_true(extension_host_permission_matches("http://127.0.0.1/*", "http://127.0.0.1:4321/page"));
  g_assert_false(extension_host_permission_matches("http://127.0.0.1/*", "http://localhost:4321/page"));
  g_assert_true(extension_host_permission_matches("*://*.example.com/path/*", "https://example.com/other"));
  g_assert_true(extension_host_permission_matches("*://*.example.com/*", "http://a.b.example.com:90/"));
  g_assert_false(extension_host_permission_matches("*://*.example.com/*", "https://example.com.attacker.test/"));
  g_assert_false(extension_host_permission_matches("*://*.example.com/*", "https://badexample.com/"));
  g_assert_false(extension_host_permission_matches("https://example.com/*", "http://example.com/"));
  g_assert_true(extension_host_permission_matches("https://EXAMPLE.com/*", "https://example.com/"));
  g_assert_true(extension_host_permission_matches("http://[::1]/*", "http://[::1]:8080/"));
  g_assert_true(extension_host_permission_matches("https://bücher.example/*", "https://xn--bcher-kva.example/"));
  g_assert_false(extension_host_permission_matches("tabs", "https://example.com/"));
  g_assert_false(extension_host_permission_matches("https://example.com:443/*", "https://example.com/"));
  for (const char *uri : {"file:///etc/passwd", "chrome://settings/", "chrome-extension://abc/page.html", "data:text/html,hi", "javascript:alert(1)"})
    g_assert_false(extension_host_permission_matches("<all_urls>", uri));
  g_print("Extension host permissions, ports, schemes and domain boundaries: PASS\n");
}
