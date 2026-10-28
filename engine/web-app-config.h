/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <string.h>

#define GNOSIS_WEB_APP_CONFIG "web-app.ini"

static inline gboolean gnosis_web_app_extension_id_valid(const char *id) {
  if (!id) return FALSE;
  if (!strcmp(id, "ublock") || !strcmp(id, "keepassxc")) return TRUE;
  return strlen(id) == 32 && strspn(id, "abcdefghijklmnop") == 32;
}

static inline gboolean gnosis_web_app_config_validate(GKeyFile *config, GError **error) {
  g_auto(GStrv) enabled = NULL;
  gsize count = 0;
  if (!g_key_file_has_group(config, "Extensions")) return TRUE;
  enabled = g_key_file_get_string_list(config, "Extensions", "enabled", &count, error);
  if (!enabled) return FALSE;
  if (count > 256) goto invalid;
  for (gsize i = 0; i < count; i++) {
    g_autofree char *path = NULL;
    if (!gnosis_web_app_extension_id_valid(enabled[i])) goto invalid;
    if (!strcmp(enabled[i], "ublock") || !strcmp(enabled[i], "keepassxc")) continue;
    path = g_key_file_get_string(config, enabled[i], "path", NULL);
    if (!path || !g_path_is_absolute(path)) goto invalid;
  }
  return TRUE;
invalid:
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid web app extension selection");
  return FALSE;
}

static inline GKeyFile *gnosis_web_app_config_load(const char *profile, GError **error) {
  g_autofree char *path = g_build_filename(profile, GNOSIS_WEB_APP_CONFIG, NULL);
  g_autofree char *contents = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GKeyFile) config = g_key_file_new();
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, &local_error)) {
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) return g_steal_pointer(&config);
    g_propagate_error(error, g_steal_pointer(&local_error)); return NULL;
  }
  if (length > 512 * 1024) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app configuration is too large"); return NULL;
  }
  if (!g_key_file_load_from_data(config, contents, length, G_KEY_FILE_NONE, error) ||
      !gnosis_web_app_config_validate(config, error)) return NULL;
  return g_steal_pointer(&config);
}

static inline gboolean gnosis_web_app_config_save(const char *profile, GKeyFile *config, GError **error) {
  g_autofree char *path = g_build_filename(profile, GNOSIS_WEB_APP_CONFIG, NULL);
  g_autofree char *contents = NULL;
  gsize length;
  if (!gnosis_web_app_config_validate(config, error)) return FALSE;
  contents = g_key_file_to_data(config, &length, error);
  return contents && g_file_set_contents_full(path, contents, length,
    (GFileSetContentsFlags)(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, error);
}

static inline gboolean gnosis_web_app_extension_enabled(GKeyFile *config, const char *id) {
  g_auto(GStrv) enabled = g_key_file_get_string_list(config, "Extensions", "enabled", NULL, NULL);
  return enabled && g_strv_contains((const char *const *)enabled, id);
}
