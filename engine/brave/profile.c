/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "profile.h"
#include <json-glib/json-glib.h>
#include <errno.h>

static JsonObject *object_member(JsonObject *parent, const char *name, GError **error) {
  if (!json_object_has_member(parent, name)) {
    JsonObject *child = json_object_new(); json_object_set_object_member(parent, name, child); return child;
  }
  JsonNode *node = json_object_get_member(parent, name);
  if (JSON_NODE_HOLDS_OBJECT(node)) return json_node_get_object(node);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Brave Local State member %s is not an object", name);
  return NULL;
}
gboolean jupiter_brave_profile_prepare(const char *directory, GError **error) {
  if (g_mkdir_with_parents(directory, 0700) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create Jupiter's Brave profile directory"); return FALSE;
  }
  char *path = g_build_filename(directory, "Local State", NULL);
  JsonParser *parser = json_parser_new(); JsonNode *root = NULL;
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    if (!json_parser_load_from_file(parser, path, error)) { g_object_unref(parser); g_free(path); return FALSE; }
    root = json_node_copy(json_parser_get_root(parser));
  } else {
    root = json_node_new(JSON_NODE_OBJECT); json_node_take_object(root, json_object_new());
  }
  g_object_unref(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Brave Local State is not a JSON object");
    json_node_free(root); g_free(path); return FALSE;
  }
  JsonObject *brave = object_member(json_node_get_object(root), "brave", error);
  JsonObject *p3a = brave ? object_member(brave, "p3a", error) : NULL;
  if (!p3a) { json_node_free(root); g_free(path); return FALSE; }
  // Brave v1.97.56's BraveConfirmP3AInfoBarDelegate checks these actual local-
  // state preferences before creating its analytics infobar. Jupiter owns its
  // UI and does not enable Brave's product analytics in its isolated profiles.
  json_object_set_boolean_member(p3a, "enabled", FALSE);
  json_object_set_boolean_member(p3a, "notice_acknowledged", TRUE);
  char *json = json_to_string(root, TRUE);
  gboolean result = g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
  g_free(json); json_node_free(root); g_free(path); return result;
}
