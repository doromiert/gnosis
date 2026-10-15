#pragma once
#include <charconv>
#include <optional>
#include <string_view>
inline std::optional<int> extension_tab_id(std::string_view message) {
  constexpr std::string_view prefix = "JUPITER_NATIVE_TAB_ID:";
  if (!message.starts_with(prefix))
    return {};
  message.remove_prefix(prefix.size());
  int id = -1;
  auto [end, error] = std::from_chars(message.data(), message.data() + message.size(), id);
  if (error != std::errc() || end != message.data() + message.size() || id < 0)
    return {};
  return id;
}
