/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gnosis-rebrand.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>

static gboolean
move_directory (const char *parent, gboolean preserve_extension_paths, GError **error)
{
  g_autofree char *old = g_build_filename (parent, "jupiter", NULL);
  g_autofree char *current = g_build_filename (parent, "gnosis", NULL);
  if (preserve_extension_paths && g_file_test (current, G_FILE_TEST_IS_SYMLINK)) {
    g_autofree char *target = g_file_read_link (current, error);
    if (!target) return FALSE;
    if (!strcmp (target, "jupiter") && g_file_test (old, G_FILE_TEST_IS_DIR)) return TRUE;
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Cannot migrate unexpected profile link: %s", current);
    return FALSE;
  }
  if (g_file_test (old, G_FILE_TEST_IS_SYMLINK)) {
    g_autofree char *target = g_file_read_link (old, error);
    if (!target) return FALSE;
    if (!strcmp (target, "gnosis") && g_file_test (current, G_FILE_TEST_IS_DIR)) return TRUE;
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Cannot migrate unexpected legacy profile link: %s", old);
    return FALSE;
  }
  if (!g_file_test (old, G_FILE_TEST_EXISTS)) return TRUE;
  if (!g_file_test (old, G_FILE_TEST_IS_DIR) || g_file_test (current, G_FILE_TEST_EXISTS)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Cannot migrate %s: both profile paths exist or the legacy path is not a directory", old);
    return FALSE;
  }
  /* Chromium canonicalizes extension paths before deriving their IDs. Keep
   * existing storage in place and expose it through the new profile name. */
  if (preserve_extension_paths) {
    if (symlink ("jupiter", current) == 0) return TRUE;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot create profile redirect %s: %s", current, g_strerror (errno));
    return FALSE;
  }
  if (g_rename (old, current) < 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot move %s to %s: %s", old, current, g_strerror (errno));
    return FALSE;
  }
  /* Chromium stores absolute extension paths; old file bookmarks and installed
   * launchers can also reference them. Keep a redirect, never a second profile. */
  if (symlink ("gnosis", old) < 0) {
    int saved_errno = errno;
    if (g_rename (current, old) < 0)
      g_warning ("The profile remains at %s; restoring its legacy path failed", current);
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno), "Cannot create profile redirect %s: %s", old, g_strerror (saved_errno));
    return FALSE;
  }
  return TRUE;
}

static gboolean
dconf_command (const char *program, const char *operation, const char *path,
               const char *input, char **output, GError **error)
{
  g_autoptr (GSubprocess) process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
    G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
    program, operation, path, NULL);
  if (!process || !g_subprocess_communicate_utf8 (process, input, NULL, output, NULL, error)) return FALSE;
  return g_subprocess_wait_check (process, NULL, error);
}

static gboolean
migrate_settings (GError **error)
{
  g_autofree char *program = g_find_program_in_path ("dconf");
  g_autofree char *old = NULL, *current = NULL;
  g_autoptr (GKeyFile) source = g_key_file_new ();
  g_autoptr (GKeyFile) target = g_key_file_new ();
  g_auto (GStrv) groups = NULL;
  g_autofree char *merged = NULL;
  if (!program) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "The dconf command is required to migrate the existing browser settings");
    return FALSE;
  }
  if (!dconf_command (program, "dump", "/io/github/doromiert/jupiter/", NULL, &old, error)) return FALSE;
  if (!old || !*old) return TRUE;
  if (!dconf_command (program, "dump", "/com/neg_zero/gnosis/", NULL, &current, error)) return FALSE;
  if (!g_key_file_load_from_data (source, old, -1, G_KEY_FILE_NONE, error) ||
      (current && *current && !g_key_file_load_from_data (target, current, -1, G_KEY_FILE_NONE, error))) return FALSE;
  groups = g_key_file_get_groups (source, NULL);
  for (guint i = 0; groups[i]; ++i) {
    g_auto (GStrv) keys = g_key_file_get_keys (source, groups[i], NULL, error);
    if (!keys) return FALSE;
    for (guint j = 0; keys[j]; ++j) {
      g_autofree char *value = NULL;
      if (g_key_file_has_key (target, groups[i], keys[j], NULL)) continue;
      value = g_key_file_get_value (source, groups[i], keys[j], error);
      if (!value) return FALSE;
      g_key_file_set_value (target, groups[i], keys[j], value);
    }
  }
  merged = g_key_file_to_data (target, NULL, error);
  return merged && dconf_command (program, "load", "/com/neg_zero/gnosis/", merged, NULL, error);
}

gboolean
gnosis_migrate_legacy_profile (GError **error)
{
  g_autofree char *legacy = g_build_filename (g_get_user_data_dir (), "jupiter", NULL);
  g_autofree char *marker = g_build_filename (g_get_user_data_dir (), "gnosis", ".rebrand-v1", NULL);
  g_autoptr (GDBusConnection) bus = NULL;
  g_autoptr (GVariant) reply = NULL;
  gboolean running;
  if (!g_file_test (legacy, G_FILE_TEST_EXISTS) || g_file_test (marker, G_FILE_TEST_EXISTS)) return TRUE;
  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (!bus) return FALSE;
  reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
    g_variant_new ("(s)", "io.github.doromiert.Jupiter"), G_VARIANT_TYPE ("(b)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
  if (!reply) return FALSE;
  g_variant_get (reply, "(b)", &running);
  if (running) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "Close the previous browser version before migrating its profile to Gnosis");
    return FALSE;
  }
  if (!migrate_settings (error) || !move_directory (g_get_user_data_dir (), TRUE, error) ||
      !move_directory (g_get_user_cache_dir (), FALSE, error) || !move_directory (g_get_user_config_dir (), FALSE, error)) return FALSE;
  return g_file_set_contents_full (marker, "1\n", -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
