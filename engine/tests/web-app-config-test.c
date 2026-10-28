/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "web-app-config.h"
#include <glib/gstdio.h>
int main(void) {
  g_autofree char *first = g_dir_make_tmp("gnosis-webapp-config-a-XXXXXX", NULL);
  g_autofree char *second = g_dir_make_tmp("gnosis-webapp-config-b-XXXXXX", NULL);
  g_autoptr(GKeyFile) config = gnosis_web_app_config_load(first, NULL);
  g_autoptr(GKeyFile) other = NULL;
  g_autoptr(GError) error = NULL;
  const char *enabled[] = {"keepassxc", "abcdefghijklmnopabcdefghijklmnop"};
  g_autofree char *path = g_build_filename(first, GNOSIS_WEB_APP_CONFIG, NULL);
  g_autofree char *second_path = g_build_filename(second, GNOSIS_WEB_APP_CONFIG, NULL);
  g_assert_nonnull(config);
  g_assert_false(g_key_file_has_group(config, "Extensions"));
  g_key_file_set_string_list(config, "Extensions", "enabled", enabled, 2);
  g_key_file_set_string(config, enabled[1], "path", "/tmp/installed-extension");
  g_assert_true(gnosis_web_app_config_save(first, config, &error));
  g_assert_no_error(error);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_no_error(error);
  g_assert_true(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_assert_false(gnosis_web_app_extension_enabled(other, "ublock"));
  g_key_file_set_string_list(other, "Extensions", "enabled", NULL, 0);
  g_assert_true(gnosis_web_app_config_save(second, other, &error));
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(second, &error);
  g_assert_true(g_key_file_has_group(other, "Extensions"));
  g_assert_false(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_key_file_set_string(config, enabled[1], "path", "relative/path");
  g_assert_false(gnosis_web_app_config_save(first, config, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_true(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_assert_false(gnosis_web_app_extension_id_valid("../../outside"));
  g_file_set_contents(path, "[Extensions]\nenabled=../../outside;\n", -1, NULL);
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_null(other);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_unlink(path); g_unlink(second_path); g_rmdir(first); g_rmdir(second);
  g_print("WEB_APP_CONFIG_PASS isolated profiles, explicit no-extensions, invalid selection rejected without overwriting\n");
}
