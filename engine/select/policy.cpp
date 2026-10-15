#include "policy.hpp"
#include <algorithm>
#include <glib.h>
bool Policy::is_new_tab(const std::string& s) {
  return s == new_tab_url;
}
bool Policy::is_http(const std::string& s) {
  return s.starts_with("https://") || s.starts_with("http://");
}
bool Policy::navigable(const std::string& s) {
  if (s.starts_with("view-source:"))
    return is_http(s.substr(12));
  auto* scheme = g_uri_parse_scheme(s.c_str());
  if (!scheme)
    return false;
  bool ok = !g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "https") ||
            !g_ascii_strcasecmp(scheme, "ephy-about") || !g_ascii_strcasecmp(scheme, "ephy-reader") ||
            !g_ascii_strcasecmp(scheme, "ephy-resource") || !g_ascii_strcasecmp(scheme, "file") || !g_ascii_strcasecmp(scheme, "about") ||
            !g_ascii_strcasecmp(scheme, "chrome-extension") || !g_ascii_strcasecmp(scheme, "blob") ||
            !g_ascii_strcasecmp(scheme, "data");
  g_free(scheme);
  return ok;
}
std::string Policy::normalize(const std::string& value) {
  char* raw = g_strdup(value.c_str());
  g_strstrip(raw);
  std::string s(raw);
  g_free(raw);
  if (s.empty())
    return new_tab_url;
  if (navigable(s))
    return s;
  if (s.find_first_of(" \t\r\n") == std::string::npos &&
      (s.find('.') != std::string::npos || s.starts_with("localhost") || s.starts_with("[::1]")))
    return (s.starts_with("localhost") || s.starts_with("127.") || s.starts_with("[::1]") ? "http://"
                                                                                          : "https://") +
           s;
  char* encoded = g_uri_escape_string(s.c_str(), nullptr, true);
  s = "https://duckduckgo.com/?q=" + std::string(encoded);
  g_free(encoded);
  return s;
}
std::string Policy::title(const std::string& url, const std::string& title) {
  if (is_new_tab(url))
    return "New Tab";
  if (!title.empty() && title != url)
    return title;
  GUri* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  std::string s = uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : url;
  if (uri)
    g_uri_unref(uri);
  return s;
}
