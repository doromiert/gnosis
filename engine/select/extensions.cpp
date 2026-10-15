#include "extensions.hpp"
#include <filesystem>
#include <json-glib/json-glib.h>
#include <stdexcept>
namespace fs = std::filesystem;
namespace {
std::string text(JsonObject* o, const char* k) {
  auto* n = json_object_get_member(o, k);
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
             ? json_node_get_string(n)
             : "";
}
std::string extension_id(const guchar* data, gsize size) {
  guchar hash[32];
  gsize n = 32;
  auto* c = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(c, data, size);
  g_checksum_get_digest(c, hash, &n);
  g_checksum_free(c);
  std::string id;
  for (int i = 0; i < 16; i++) {
    id += char('a' + (hash[i] >> 4));
    id += char('a' + (hash[i] & 15));
  }
  return id;
}
bool local_page(const std::string& s) {
  if (s.empty())
    return true;
  fs::path p(s);
  if (p.is_absolute() || s.find(':') != std::string::npos)
    return false;
  for (auto& part : p)
    if (part == "..")
      return false;
  return true;
}
} // namespace
std::string Extensions::id_for_path(const std::string& path) {
  auto p = fs::absolute(path).lexically_normal().string();
  return extension_id(reinterpret_cast<const guchar*>(p.data()), p.size());
}
Extension Extensions::inspect(const std::string& path) const {
  Extension e;
  e.path = fs::absolute(path).lexically_normal().string();
  e.id = id_for_path(e.path);
  auto* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_file(parser, (e.path + "/manifest.json").c_str(), &error)) {
    std::string msg = error->message;
    g_error_free(error);
    g_object_unref(parser);
    throw std::runtime_error(msg);
  }
  auto* n = json_parser_get_root(parser);
  if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
    g_object_unref(parser);
    throw std::runtime_error("Invalid extension manifest");
  }
  auto* o = json_node_get_object(n);
  e.name = text(o, "name");
  e.version = text(o, "version");
  n = json_object_get_member(o, "manifest_version");
  if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64)
    e.manifest = json_node_get_int(n);
  auto key = text(o, "key");
  if (!key.empty()) {
    gsize len = 0;
    auto* bytes = g_base64_decode(key.c_str(), &len);
    e.id = extension_id(bytes, len);
    g_free(bytes);
  }
  auto* action = json_object_get_member(o, e.manifest == 2 ? "browser_action" : "action");
  if (action && JSON_NODE_HOLDS_OBJECT(action))
    e.popup = text(json_node_get_object(action), "default_popup");
  e.options = text(o, "options_page");
  auto* options = json_object_get_member(o, "options_ui");
  if (options && JSON_NODE_HOLDS_OBJECT(options))
    e.options = text(json_node_get_object(options), "page");
  for (const auto* k : {"permissions", "host_permissions"}) {
    n = json_object_get_member(o, k);
    if (n && JSON_NODE_HOLDS_ARRAY(n)) {
      auto* a = json_node_get_array(n);
      for (unsigned i = 0; i < json_array_get_length(a); i++) {
        auto* v = json_array_get_element(a, i);
        if (JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING)
          e.permissions.push_back(json_node_get_string(v));
      }
    }
  }
  if (e.name.starts_with("__MSG_")) {
    auto locale = text(o, "default_locale");
    auto token = e.name.substr(6, e.name.size() - 8);
    auto* lp = json_parser_new();
    if (local_page(locale) && json_parser_load_from_file(
                                  lp, (e.path + "/_locales/" + locale + "/messages.json").c_str(), nullptr)) {
      auto* root = json_parser_get_root(lp);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        auto* msg = json_object_get_member(json_node_get_object(root), token.c_str());
        if (msg && JSON_NODE_HOLDS_OBJECT(msg))
          e.name = text(json_node_get_object(msg), "message");
      }
    }
    g_object_unref(lp);
  }
  g_object_unref(parser);
  if ((e.manifest != 2 && e.manifest != 3) || e.name.empty() || e.name.size() > 256 || e.version.empty() ||
      !local_page(e.popup) || !local_page(e.options))
    throw std::runtime_error(
        "The manifest must describe a Manifest V2 or V3 extension with valid local pages.");
  e.enabled = !fs::exists(directory_ + "/disabled/" + e.id);
  return e;
}
Extensions::Extensions(std::string data) : directory_(data + "/extensions") {
  fs::create_directories(directory_ + "/installed");
  auto add = [&](const std::string& path, bool builtin) {
    try {
      auto e = inspect(path);
      e.builtin = builtin;
      installed.push_back(e);
    } catch (const std::exception& error) {
      g_warning("Extension %s: %s", path.c_str(), error.what());
    }
  };
  const char* ubo = g_getenv("JUPITER_UBLOCK_DIR");
  add(ubo ? ubo : JUPITER_UBLOCK_DIR, true);
  for (const auto& p : fs::directory_iterator(directory_ + "/installed"))
    if (p.is_directory())
      add(p.path().string(), false);
}
std::string Extensions::load_paths() const {
  std::string paths = JUPITER_BRIDGE_DIR;
  for (const auto& e : installed)
    if (e.enabled)
      paths += "," + e.path;
  return paths;
}
void Extensions::install(const std::string& source) {
  auto checked = inspect(source);
  std::uintmax_t total = 0;
  unsigned count = 0;
  for (const auto& p : fs::recursive_directory_iterator(source)) {
    if (p.is_symlink() || (!p.is_directory() && !p.is_regular_file()))
      throw std::runtime_error("Extensions must contain regular files and folders, without symbolic links.");
    if (p.is_regular_file()) {
      total += p.file_size();
      if (total > 256 * 1024 * 1024 || ++count > 20000)
        throw std::runtime_error("The extension exceeds the installation size limit.");
    }
  }
  char* uuid = g_uuid_string_random();
  auto target = fs::path(directory_) / "installed" / uuid;
  g_free(uuid);
  try {
    fs::copy(source, target, fs::copy_options::recursive);
    installed.push_back(inspect(target.string()));
  } catch (...) {
    fs::remove_all(target);
    throw;
  }
}
void Extensions::set_enabled(const Extension& e, bool enabled) {
  fs::create_directories(directory_ + "/disabled");
  auto marker = directory_ + "/disabled/" + e.id;
  if (enabled)
    fs::remove(marker);
  else if (!g_file_set_contents(marker.c_str(), "", 0, nullptr))
    throw std::runtime_error("Could not save the extension setting.");
  for (auto& x : installed)
    if (x.id == e.id)
      x.enabled = enabled;
}
