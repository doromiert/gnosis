/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <string>

// Chromium host permissions ignore paths and ports. File access needs a separate
// user grant, which Jupiter's extension manager does not provide yet.
inline bool extension_host_permission_matches(const std::string& pattern, const std::string& url) {
  auto *uri = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!uri) return false;
  const char *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
  bool web = scheme && (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "https"));
  bool allowed = false;
  if (web && host) {
    if (pattern == "<all_urls>") allowed = true;
    else {
      auto split = pattern.find("://"), slash = pattern.find('/', split == std::string::npos ? 0 : split + 3);
      if (split != std::string::npos && slash != std::string::npos) {
        auto protocol = pattern.substr(0, split), target = pattern.substr(split + 3, slash - split - 3);
        if (protocol == "*" || !g_ascii_strcasecmp(protocol.c_str(), scheme)) {
          if (target == "*") allowed = true;
          else {
            bool subdomains = target.starts_with("*.");
            if (subdomains) target.erase(0, 2);
            if (target.starts_with('[') && target.ends_with(']')) target = target.substr(1, target.size() - 2);
            char *ascii = g_hostname_to_ascii(target.c_str());
            char *actual = g_hostname_to_ascii(host);
            if (ascii && actual && target.find('*') == std::string::npos) {
              auto expected = std::string(ascii), name = std::string(actual);
              allowed = !g_ascii_strcasecmp(expected.c_str(), name.c_str()) ||
                (subdomains && name.size() > expected.size() && name[name.size() - expected.size() - 1] == '.' &&
                 !g_ascii_strcasecmp(name.c_str() + name.size() - expected.size(), expected.c_str()));
            }
            g_free(ascii); g_free(actual);
          }
        }
      }
    }
  }
  g_uri_unref(uri);
  return allowed;
}
