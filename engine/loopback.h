/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <string.h>

static inline gboolean gnosis_uri_is_loopback(const char *address) {
  g_autoptr(GUri) uri = g_uri_parse(address, G_URI_FLAGS_PARSE_RELAXED, NULL);
  const char *host = uri ? g_uri_get_host(uri) : NULL;
  g_autofree char *name = NULL;
  g_autoptr(GInetAddress) ip = NULL;
  gsize length;
  if (!host) return FALSE;
  name = g_ascii_strdown(host, -1);
  length = strlen(name);
  if (length && name[length - 1] == '.') name[length - 1] = 0;
  if (g_str_equal(name, "localhost") || g_str_has_suffix(name, ".localhost")) return TRUE;
  ip = g_inet_address_new_from_string(name);
  return ip && g_inet_address_get_is_loopback(ip);
}
