/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gnosis-rebrand.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_file (const char *path, const char *text) {
  g_autofree char *parent = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}
static void clear_tree (const char *path) {
  if (!g_file_test (path, G_FILE_TEST_IS_SYMLINK) && g_file_test (path, G_FILE_TEST_IS_DIR)) {
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name (dir))) {
      g_autofree char *child = g_build_filename (path, name, NULL); clear_tree (child);
    }
    g_assert_cmpint (g_rmdir (path), ==, 0);
  } else g_assert_cmpint (g_unlink (path), ==, 0);
}
int main (int argc, char **argv) {
  if (argc == 3) {
    g_autofree char *state = g_build_filename (g_getenv ("GNOSIS_TEST_DCONF_STATE"),
      !strcmp (argv[2], "/io/github/doromiert/jupiter/") ? "old" : "new", NULL);
    if (!strcmp (argv[1], "dump")) {
      g_autofree char *text = NULL; g_file_get_contents (state, &text, NULL, NULL);
      if (text) fputs (text, stdout);
    } else {
      g_autoptr (GString) text = g_string_new (NULL); char buffer[1024]; size_t count;
      while ((count = fread (buffer, 1, sizeof buffer, stdin))) g_string_append_len (text, buffer, count);
      write_file (state, text->str);
    }
    return 0;
  }
  g_autofree char *root = g_dir_make_tmp ("gnosis-rebrand-test-XXXXXX", NULL);
  g_assert_nonnull (root);
  const char *variables[] = {"XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME"};
  const char *folders[] = {"data", "cache", "config"};
  for (guint i = 0; i < G_N_ELEMENTS (folders); ++i) {
    g_autofree char *path = g_build_filename (root, folders[i], NULL);
    g_setenv (variables[i], path, TRUE);
  }
  g_autoptr (GError) error = NULL;
  g_assert_true (gnosis_migrate_legacy_profile (&error)); // a new installation needs no migration tools
  g_autofree char *bin = g_build_filename (root, "bin", NULL);
  g_mkdir_with_parents (bin, 0700);
  g_autofree char *mock = g_build_filename (bin, "dconf", NULL);
  g_autofree char *self = g_file_read_link ("/proc/self/exe", NULL);
  g_assert_cmpint (symlink (self, mock), ==, 0);
  g_autofree char *path = g_strconcat (bin, ":", g_getenv ("PATH"), NULL);
  g_setenv ("PATH", path, TRUE); g_setenv ("GNOSIS_TEST_DCONF_STATE", root, TRUE);
  g_autofree char *old_settings = g_build_filename (root, "old", NULL);
  g_autofree char *new_settings = g_build_filename (root, "new", NULL);
  write_file (old_settings, "[/]\nhomepage-url='https://old.example'\n[web]\nenable-adblock=false\n");
  write_file (new_settings, "[/]\nhomepage-url='https://new.example'\n");
  for (guint i = 0; i < G_N_ELEMENTS (folders); ++i) {
    g_autofree char *file = g_build_filename (root, folders[i], "jupiter", "retained", NULL);
    write_file (file, "synthetic paired extension and profile data");
  }
  g_autoptr (GTestDBus) daemon = g_test_dbus_new (G_TEST_DBUS_NONE); g_test_dbus_up (daemon);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error); g_assert_no_error (error);
  g_autoptr (GVariant) owned = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new ("(su)", "io.github.doromiert.Jupiter", 0), NULL, 0, -1, NULL, &error);
  g_assert_no_error (error); g_assert_nonnull (owned);
  g_assert_false (gnosis_migrate_legacy_profile (&error)); g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error (&error);
  g_autoptr (GVariant) released = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", g_variant_new ("(s)", "io.github.doromiert.Jupiter"), NULL, 0, -1, NULL, &error);
  g_assert_no_error (error); g_assert_nonnull (released);
  g_autofree char *collision = g_build_filename (root, "data", "gnosis", NULL);
  g_mkdir_with_parents (collision, 0700);
  g_assert_false (gnosis_migrate_legacy_profile (&error)); g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error (&error);
  g_assert_cmpint (g_rmdir (collision), ==, 0);
  g_assert_true (gnosis_migrate_legacy_profile (&error)); g_assert_no_error (error);
  g_autofree char *brave = g_build_filename (root, "data", "gnosis", "brave", NULL);
  g_autofree char *identity = gnosis_legacy_extension_profile (brave);
  g_autofree char *expected_identity = g_build_filename (root, "data", "jupiter", "brave", NULL);
  g_assert_cmpstr (identity, ==, expected_identity);
  g_autofree char *private_identity = gnosis_legacy_extension_profile ("/tmp/gnosis-private-test");
  g_assert_cmpstr (private_identity, ==, "/tmp/gnosis-private-test");
  for (guint i = 0; i < G_N_ELEMENTS (folders); ++i) {
    g_autofree char *file = g_build_filename (root, folders[i], "gnosis", "retained", NULL);
    g_autofree char *text = NULL; g_assert_true (g_file_get_contents (file, &text, NULL, NULL));
    g_assert_cmpstr (text, ==, "synthetic paired extension and profile data");
    g_autofree char *legacy = g_build_filename (root, folders[i], "jupiter", NULL);
    if (i == 0) {
      g_assert_false (g_file_test (legacy, G_FILE_TEST_IS_SYMLINK));
      g_autofree char *current = g_build_filename (root, folders[i], "gnosis", NULL);
      g_assert_true (g_file_test (current, G_FILE_TEST_IS_SYMLINK));
    } else g_assert_true (g_file_test (legacy, G_FILE_TEST_IS_SYMLINK));
  }
  g_autoptr (GKeyFile) settings = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (settings, new_settings, G_KEY_FILE_NONE, &error)); g_assert_no_error (error);
  g_autofree char *homepage = g_key_file_get_value (settings, "/", "homepage-url", NULL);
  g_assert_cmpstr (homepage, ==, "'https://new.example'");
  g_assert_false (g_key_file_get_boolean (settings, "web", "enable-adblock", &error)); g_assert_no_error (error);
  g_unlink (mock); // completed migrations must not depend on the old settings or run again
  g_assert_true (gnosis_migrate_legacy_profile (&error)); g_assert_no_error (error);
  g_clear_object (&bus); g_test_dbus_down (daemon); clear_tree (root);
  g_print ("REBRAND_PASS profile/cache/config retained, extension paths preserved, settings merged, running-browser guard, collision safety, idempotence\n");
}
