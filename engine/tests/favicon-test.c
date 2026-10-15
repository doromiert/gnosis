/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-favicon.h"
#include <glib/gstdio.h>

typedef struct { GdkTexture *texture; GError *error; gboolean done; } Result;
static void loaded(GObject *source, GAsyncResult *result, gpointer user_data) {
  Result *state = user_data;
  state->texture = ephy_engine_favicon_database_get_favicon_finish(EPHY_ENGINE_FAVICON_DATABASE(source), result, &state->error);
  state->done = TRUE;
}
static Result get_icon(EphyEngineFaviconDatabase *db, const char *uri, GCancellable *cancellable) {
  Result result = {0};
  ephy_engine_favicon_database_get_favicon(db, uri, cancellable, loaded, &result);
  while (!result.done) g_main_context_iteration(NULL, TRUE);
  return result;
}
static void favicon_cache(void) {
  const char *page = "https://example.test/a";
  const char *icon = "https://example.test/favicon.png";
  const guint8 pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
  g_autoptr(GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp("jupiter-favicons-XXXXXX", &error);
  g_autoptr(GBytes) bytes = g_bytes_new_static(pixels, sizeof pixels);
  g_autoptr(GdkTexture) texture = gdk_memory_texture_new(2, 1, GDK_MEMORY_R8G8B8A8, bytes, 8);
  g_autoptr(EphyEngineFaviconDatabase) writer = ephy_engine_favicon_database_new(directory);
  g_autoptr(EphyEngineFaviconDatabase) reader = ephy_engine_favicon_database_new(directory);
  g_autoptr(EphyEngineFaviconDatabase) private_db = ephy_engine_favicon_database_new(NULL);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, page, -1);
  g_autofree char *filename = g_build_filename(directory, hash, NULL);
  g_autofree char *uri = NULL;
  Result result;
  gint64 deadline;
  g_assert_no_error(error);
  ephy_engine_favicon_database_store(writer, page, icon, texture);
  result = get_icon(writer, page, NULL);
  g_assert_no_error(result.error);
  g_assert_cmpint(gdk_texture_get_width(result.texture), ==, 2);
  g_clear_object(&result.texture);
  deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (!g_file_test(filename, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  }
  result = get_icon(reader, page, NULL);
  g_assert_no_error(result.error);
  g_assert_nonnull(result.texture);
  g_assert_cmpint(gdk_texture_get_width(result.texture), ==, 2);
  g_clear_object(&result.texture);
  uri = ephy_engine_favicon_database_get_favicon_uri(reader, page);
  g_assert_cmpstr(uri, ==, icon);
  result = get_icon(private_db, page, NULL);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&result.error);
  ephy_engine_favicon_database_store(private_db, "https://private.test/", icon, texture);
  result = get_icon(reader, "https://private.test/", NULL);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&result.error);
  g_cancellable_cancel(cancellable);
  result = get_icon(writer, page, cancellable);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&result.error);
  g_assert_true(g_file_set_contents(filename, "invalid cache", -1, &error));
  result = get_icon(reader, page, NULL);
  g_assert_error(result.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&result.error);
  g_unlink(filename);
  g_rmdir(directory);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/engine/favicon/cache-private-cancel-corruption", favicon_cache);
  return g_test_run();
}
