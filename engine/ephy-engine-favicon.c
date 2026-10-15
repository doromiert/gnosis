/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-favicon.h"
#include <glib/gstdio.h>

typedef struct {
  char *uri;
  GdkTexture *texture;
} Icon;

struct _EphyEngineFaviconDatabase {
  GObject parent;
  char *directory;
  GHashTable *icons;
};
G_DEFINE_FINAL_TYPE(EphyEngineFaviconDatabase, ephy_engine_favicon_database, G_TYPE_OBJECT)
static guint changed_signal;

static void icon_free(Icon *icon) {
  g_free(icon->uri);
  g_clear_object(&icon->texture);
  g_free(icon);
}
static void finalize(GObject *object) {
  EphyEngineFaviconDatabase *self = EPHY_ENGINE_FAVICON_DATABASE(object);
  g_free(self->directory);
  g_hash_table_unref(self->icons);
  G_OBJECT_CLASS(ephy_engine_favicon_database_parent_class)->finalize(object);
}
static void ephy_engine_favicon_database_class_init(EphyEngineFaviconDatabaseClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  changed_signal = g_signal_new("favicon-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                               0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}
static void ephy_engine_favicon_database_init(EphyEngineFaviconDatabase *self) {
  self->icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)icon_free);
}
EphyEngineFaviconDatabase *ephy_engine_favicon_database_new(const char *directory) {
  EphyEngineFaviconDatabase *self = g_object_new(EPHY_TYPE_ENGINE_FAVICON_DATABASE, NULL);
  self->directory = g_strdup(directory);
  return self;
}
EphyEngineFaviconDatabase *ephy_engine_favicon_database_get_default(void) {
  static EphyEngineFaviconDatabase *database;
  if (!database)
    database = ephy_engine_favicon_database_new(NULL);
  return database;
}
void ephy_engine_favicon_database_set_directory(EphyEngineFaviconDatabase *self, const char *directory) {
  g_return_if_fail(EPHY_IS_ENGINE_FAVICON_DATABASE(self));
  g_free(self->directory);
  self->directory = g_strdup(directory);
}
static char *filename_for(EphyEngineFaviconDatabase *self, const char *uri) {
  g_autofree char *hash = NULL;
  if (!self->directory)
    return NULL;
  hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
  return g_build_filename(self->directory, hash, NULL);
}
typedef struct { char *filename; GBytes *bytes; } Write;
static void write_free(Write *write) {
  g_free(write->filename);
  g_bytes_unref(write->bytes);
  g_free(write);
}
static void save_icon(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  Write *write = task_data;
  g_autofree char *directory = g_path_get_dirname(write->filename);
  g_autoptr(GError) error = NULL;
  gsize size;
  const char *data = g_bytes_get_data(write->bytes, &size);
  if (g_mkdir_with_parents(directory, 0700) != 0 ||
      !g_file_set_contents_full(write->filename, data, size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
    /* Cache writes must not prevent displaying an icon. */
    g_task_return_boolean(task, FALSE);
    return;
  }
  g_task_return_boolean(task, TRUE);
}
void ephy_engine_favicon_database_store(EphyEngineFaviconDatabase *self, const char *page_uri, const char *icon_uri, GdkTexture *texture) {
  Icon *icon;
  g_return_if_fail(EPHY_IS_ENGINE_FAVICON_DATABASE(self));
  g_return_if_fail(page_uri && icon_uri && GDK_IS_TEXTURE(texture));
  icon = g_new0(Icon, 1);
  icon->uri = g_strdup(icon_uri);
  icon->texture = g_object_ref(texture);
  g_hash_table_replace(self->icons, g_strdup(page_uri), icon);
  if (self->directory) {
    g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(texture);
    g_autoptr(GVariant) record = g_variant_ref_sink(g_variant_new("(s@ay)", icon_uri,
      g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), png, TRUE)));
    g_autoptr(GTask) task = g_task_new(self, NULL, NULL, NULL);
    Write *write = g_new0(Write, 1);
    write->filename = filename_for(self, page_uri);
    write->bytes = g_variant_get_data_as_bytes(record);
    g_task_set_task_data(task, write, (GDestroyNotify)write_free);
    g_task_run_in_thread(task, save_icon);
  }
  g_signal_emit(self, changed_signal, 0, page_uri, icon_uri);
}
static GVariant *load_record(const char *filename, GError **error) {
  GStatBuf file_stat;
  g_autofree char *contents = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) record = NULL;
  gsize size;
  if (!filename || g_stat(filename, &file_stat) != 0 || file_stat.st_size > 2 * 1024 * 1024) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No cached favicon for this page");
    return NULL;
  }
  if (!g_file_get_contents(filename, &contents, &size, error))
    return NULL;
  bytes = g_bytes_new_take(g_steal_pointer(&contents), size);
  record = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("(say)"), bytes, FALSE));
  if (!g_variant_is_normal_form(record)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid favicon cache record");
    return NULL;
  }
  return g_steal_pointer(&record);
}
static void load_icon(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) record = load_record(task_data, &error);
  g_autoptr(GVariant) png = NULL;
  g_autoptr(GBytes) bytes = NULL;
  GdkTexture *texture;
  if (!record) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  png = g_variant_get_child_value(record, 1);
  bytes = g_variant_get_data_as_bytes(png);
  texture = gdk_texture_new_from_bytes(bytes, &error);
  if (texture)
    g_task_return_pointer(task, texture, g_object_unref);
  else
    g_task_return_error(task, g_steal_pointer(&error));
}
void ephy_engine_favicon_database_get_favicon(EphyEngineFaviconDatabase *self, const char *page_uri, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  g_autoptr(GTask) task = NULL;
  Icon *icon;
  g_return_if_fail(EPHY_IS_ENGINE_FAVICON_DATABASE(self));
  task = g_task_new(self, cancellable, callback, user_data);
  icon = page_uri ? g_hash_table_lookup(self->icons, page_uri) : NULL;
  if (icon) {
    g_task_return_pointer(task, g_object_ref(icon->texture), g_object_unref);
  } else {
    g_task_set_task_data(task, page_uri ? filename_for(self, page_uri) : NULL, g_free);
    g_task_run_in_thread(task, load_icon);
  }
}
GdkTexture *ephy_engine_favicon_database_get_favicon_finish(EphyEngineFaviconDatabase *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
char *ephy_engine_favicon_database_get_favicon_uri(EphyEngineFaviconDatabase *self, const char *page_uri) {
  Icon *icon;
  g_autofree char *filename = NULL;
  g_autoptr(GVariant) record = NULL;
  g_return_val_if_fail(EPHY_IS_ENGINE_FAVICON_DATABASE(self), NULL);
  if (!page_uri)
    return NULL;
  icon = g_hash_table_lookup(self->icons, page_uri);
  if (icon)
    return g_strdup(icon->uri);
  filename = filename_for(self, page_uri);
  record = load_record(filename, NULL);
  if (record) {
    g_autoptr(GVariant) uri = g_variant_get_child_value(record, 0);
    return g_variant_dup_string(uri, NULL);
  }
  return NULL;
}
