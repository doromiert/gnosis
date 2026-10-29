/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "brave/profile.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  g_autoptr(GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp("gnosis-pairing-store-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree char *path = g_build_filename(directory, "associations.json", NULL);
  g_autofree char *hash = g_strnfill(64, 'a');
  guchar public_key[32] = {0};
  g_autofree char *key = g_base64_encode(public_key, sizeof public_key);
  g_autofree char *json = g_strdup_printf("{\"%s\":{\"hash\":\"%s\",\"id\":\"Gnosis fixture\",\"key\":\"%s\"}}", hash, hash, key);
  g_autofree char *loaded = gnosis_brave_keepassxc_pairing_load(path, &error);
  g_assert_no_error(error);g_assert_cmpstr(loaded, ==, "{}");g_clear_pointer(&loaded, g_free);
  g_assert_true(gnosis_brave_keepassxc_pairing_save(path, json, &error));g_assert_no_error(error);
  struct stat info;g_assert_cmpint(stat(path, &info), ==, 0);g_assert_cmpuint(info.st_mode & 0777, ==, 0600);
  loaded = gnosis_brave_keepassxc_pairing_load(path, &error);
  g_assert_no_error(error);g_assert_cmpstr(loaded, ==, json);g_clear_pointer(&loaded, g_free);
  g_assert_false(gnosis_brave_keepassxc_pairing_save(path, "{\"password\":\"not an association\"}", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
  loaded = gnosis_brave_keepassxc_pairing_load(path, &error);
  g_assert_no_error(error);g_assert_cmpstr(loaded, ==, json);g_clear_pointer(&loaded, g_free);
  g_assert_cmpint(g_chmod(path, 0644), ==, 0);
  g_assert_null(gnosis_brave_keepassxc_pairing_load(path, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);g_clear_error(&error);
  g_assert_true(gnosis_brave_keepassxc_pairing_save(path, "{}", &error));g_assert_no_error(error);
  loaded = gnosis_brave_keepassxc_pairing_load(path, &error);
  g_assert_no_error(error);g_assert_cmpstr(loaded, ==, "{}");g_clear_pointer(&loaded, g_free);
  g_autofree char *link = g_build_filename(directory, "symlink", NULL);
  g_assert_cmpint(symlink(path, link), ==, 0);
  g_assert_null(gnosis_brave_keepassxc_pairing_load(link, &error));g_clear_error(&error);
  g_assert_false(gnosis_brave_keepassxc_pairing_save(link, json, &error));g_clear_error(&error);
  g_autofree char *fifo = g_build_filename(directory, "fifo", NULL);
  g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
  g_assert_null(gnosis_brave_keepassxc_pairing_load(fifo, &error));g_clear_error(&error);
  g_autofree char *oversize = g_strnfill(1024 * 1024 + 1, ' ');
  g_assert_true(g_file_set_contents(path, oversize, -1, &error));g_assert_no_error(error);g_assert_cmpint(g_chmod(path, 0600), ==, 0);
  g_assert_null(gnosis_brave_keepassxc_pairing_load(path, &error));g_clear_error(&error);
  g_unlink(fifo);g_unlink(link);g_unlink(path);g_rmdir(directory);
  g_print("KEEPASSXC_STORE_PASS atomic private records, validation, revocation, unsafe modes, symlinks, FIFOs and size limits\n");
  return 0;
}
