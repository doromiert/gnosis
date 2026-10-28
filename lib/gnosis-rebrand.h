/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <string.h>

G_BEGIN_DECLS
gboolean gnosis_migrate_legacy_profile (GError **error);

/* Keyless unpacked extension IDs depend on the original absolute path. */
static inline char *
gnosis_legacy_extension_profile (const char *profile)
{
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "gnosis", NULL);
  g_autofree char *prefix = g_strconcat (root, G_DIR_SEPARATOR_S, NULL);
  g_autofree char *marker = g_build_filename (root, ".rebrand-v1", NULL);
  g_autofree char *old = g_build_filename (g_get_user_data_dir (), "jupiter", NULL);
  if (g_str_has_prefix (profile, prefix) && g_file_test (marker, G_FILE_TEST_EXISTS) &&
      g_file_test (root, G_FILE_TEST_IS_SYMLINK)) {
    g_autofree char *target = g_file_read_link (root, NULL);
    if (!g_strcmp0 (target, "jupiter"))
      return g_build_filename (old, profile + strlen (prefix), NULL);
  }
  return g_strdup (profile);
}
G_END_DECLS
