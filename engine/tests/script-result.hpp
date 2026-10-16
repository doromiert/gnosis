/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <json-glib/json-glib.h>
#include <string>

// Diagnostic formatting of Chromium's serialized results; no JavaScript runtime.
inline std::string script_result_text(JsonObject *result) {
  g_assert_nonnull(result);
  if (json_object_has_member(result, "unserializableValue"))
    return json_object_get_string_member(result, "unserializableValue");
  if (g_str_equal(json_object_get_string_member(result, "type"), "undefined")) return "undefined";
  auto *value = json_object_get_member(result, "value");
  if (!value) return json_object_get_string_member(result,
    json_object_has_member(result, "description") ? "description" : "type");
  if (json_node_get_value_type(value) == G_TYPE_STRING) return json_node_get_string(value);
  char *json = json_to_string(value, false);
  std::string text(json); g_free(json); return text;
}
