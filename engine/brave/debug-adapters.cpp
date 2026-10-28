/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include "debug-adapters.h"
#include <map>
namespace GnosisBrave {
namespace {
std::map<std::string, bool> active_adapters;
bool configured = false;
}
void configure_debug_adapters(const std::string& preferences) {
  if (configured) return;
  configured = true;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, preferences.data(), preferences.size(), nullptr) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return;
  auto* root = json_node_get_object(json_parser_get_root(parser));
  auto* value = json_object_get_member(root,"adapters");
  if (!value || !JSON_NODE_HOLDS_OBJECT(value)) return;
  auto* overrides = json_node_get_object(value);
  for (const auto& adapter : gnosis_brave_adapters) {
    auto* node = json_object_get_member(overrides,adapter.id);
    if (node && json_node_get_value_type(node) == G_TYPE_BOOLEAN)
      active_adapters[adapter.id] = json_node_get_boolean(node);
  }
}
bool adapter_enabled(const char* id) {
  auto found = active_adapters.find(id);
  return found == active_adapters.end() || found->second;
}
bool adapter_overridden(const char* id) { return active_adapters.contains(id); }
std::string disabled_adapters() {
  std::string result;
  for (const auto& [id,enabled] : active_adapters) if (!enabled) {
    if (!result.empty()) result += ',';
    result += id;
  }
  return result;
}
}
