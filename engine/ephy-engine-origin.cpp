/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-origin.h"
#include "include/cef_parser.h"
#include <string>

struct _EphyEngineOrigin {
  gatomicrefcount refs;
  std::string scheme, host, serialized;
  guint16 port;
};
EphyEngineOrigin *ephy_engine_origin_new_for_uri(const char *uri) {
  if (!uri) return nullptr;
  std::string url(uri);
  // Blob and filesystem documents inherit the embedded URL's origin.
  if (g_ascii_strncasecmp(uri, "blob:", 5) == 0) url.erase(0, 5);
  else if (g_ascii_strncasecmp(uri, "filesystem:", 11) == 0) url.erase(0, 11);
  CefURLParts parts;
  if (!CefParseURL(url, parts)) return nullptr;
  auto serialized = CefString(&parts.origin).ToString();
  auto host = CefString(&parts.host).ToString();
  // Opaque/local origins must never share a persistent permission record.
  if (serialized.empty() || host.empty() || CefString(&parts.scheme).ToString() == "file") return nullptr;
  auto *origin = new EphyEngineOrigin;
  g_atomic_ref_count_init(&origin->refs);
  origin->scheme = CefString(&parts.scheme).ToString();
  origin->host = host;
  origin->serialized = serialized;
  if (origin->serialized.back() == '/') origin->serialized.pop_back();
  auto port = CefString(&parts.port).ToString();
  origin->port = port.empty() ? 0 : static_cast<guint16>(std::stoul(port));
  return origin;
}
EphyEngineOrigin *ephy_engine_origin_new(const char *scheme, const char *host, guint16 port) {
  if (!scheme || !host || !*host) return nullptr;
  std::string hostname(host);
  if (hostname.find(':') != std::string::npos && hostname.front() != '[') hostname = "[" + hostname + "]";
  return ephy_engine_origin_new_for_uri((std::string(scheme) + "://" + hostname + (port ? ":" + std::to_string(port) : "") + "/").c_str());
}
EphyEngineOrigin *ephy_engine_origin_ref(EphyEngineOrigin *origin) {
  g_return_val_if_fail(origin, nullptr);
  g_atomic_ref_count_inc(&origin->refs);
  return origin;
}
void ephy_engine_origin_unref(EphyEngineOrigin *origin) {
  if (origin && g_atomic_ref_count_dec(&origin->refs)) delete origin;
}
const char *ephy_engine_origin_get_protocol(EphyEngineOrigin *origin) { return origin ? origin->scheme.c_str() : nullptr; }
const char *ephy_engine_origin_get_host(EphyEngineOrigin *origin) { return origin ? origin->host.c_str() : nullptr; }
guint16 ephy_engine_origin_get_port(EphyEngineOrigin *origin) { return origin ? origin->port : 0; }
char *ephy_engine_origin_to_string(EphyEngineOrigin *origin) { return origin ? g_strdup(origin->serialized.c_str()) : nullptr; }
