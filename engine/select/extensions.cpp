#include "extensions.hpp"
#include <filesystem>
#include <json-glib/json-glib.h>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
struct FileDescriptor {
  int value;
  explicit FileDescriptor(int fd) : value(fd) {
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Could not open extension file");
  }
  ~FileDescriptor() { close(value); }
  FileDescriptor(const FileDescriptor&) = delete;
};
void check_cancelled(GCancellable* cancellable) {
  if (cancellable && g_cancellable_is_cancelled(cancellable))
    throw std::system_error(ECANCELED, std::generic_category(), "Extension installation cancelled");
}
JsonParser* read_json(int directory, const char* name) {
  FileDescriptor file(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat info{};
  if (fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_size > 1024 * 1024)
    throw std::runtime_error("Extension metadata must be a regular file smaller than 1 MiB.");
  std::string contents;
  char buffer[16384];
  for (;;) {
    auto size = read(file.value, buffer, sizeof(buffer));
    if (size < 0 && errno == EINTR) continue;
    if (size < 0) throw std::system_error(errno, std::generic_category(), "Could not read extension metadata");
    if (!size) break;
    contents.append(buffer, size);
    if (contents.size() > 1024 * 1024) throw std::runtime_error("Extension metadata exceeds 1 MiB.");
  }
  auto* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, contents.data(), contents.size(), &error)) {
    std::string message = error->message;
    g_error_free(error); g_object_unref(parser);
    throw std::runtime_error(message);
  }
  return parser;
}
struct CopyBudget { uint64_t bytes = 0; unsigned entries = 0; };
void copy_tree(int source, int target, CopyBudget& budget, GCancellable* cancellable, unsigned depth = 0) {
  if (depth > 64) throw std::runtime_error("The extension directory is too deeply nested.");
  int listing = openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (listing < 0) throw std::system_error(errno, std::generic_category(), "Could not list extension directory");
  auto* raw = fdopendir(listing);
  if (!raw) {
    const int failure = errno; close(listing);
    throw std::system_error(failure, std::generic_category(), "Could not list extension directory");
  }
  std::unique_ptr<DIR, decltype(&closedir)> entries(raw, closedir);
  for (;;) {
    check_cancelled(cancellable);
    errno = 0;
    auto* entry = readdir(entries.get());
    if (!entry) {
      if (errno) throw std::system_error(errno, std::generic_category(), "Could not list extension directory");
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (++budget.entries > 20000) throw std::runtime_error("The extension exceeds 20,000 files and directories.");
    struct stat info{};
    if (fstatat(source, name.c_str(), &info, AT_SYMLINK_NOFOLLOW))
      throw std::system_error(errno, std::generic_category(), "Could not inspect extension file");
    if (S_ISDIR(info.st_mode)) {
      FileDescriptor input(openat(source, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (target >= 0) {
        if (mkdirat(target, name.c_str(), 0700))
          throw std::system_error(errno, std::generic_category(), "Could not create extension directory");
        FileDescriptor output(openat(target, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        copy_tree(input.value, output.value, budget, cancellable, depth + 1);
      } else copy_tree(input.value, -1, budget, cancellable, depth + 1);
    } else if (S_ISREG(info.st_mode)) {
      FileDescriptor input(openat(source, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      if (fstat(input.value, &info) || !S_ISREG(info.st_mode))
        throw std::runtime_error("The extension changed while it was being copied.");
      if (info.st_size < 0 || uint64_t(info.st_size) > 256 * 1024 * 1024 - budget.bytes)
        throw std::runtime_error("The extension exceeds 256 MiB.");
      if (target < 0) { budget.bytes += info.st_size; continue; }
      FileDescriptor output(openat(target, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      char buffer[65536];
      for (;;) {
        check_cancelled(cancellable);
        auto size = read(input.value, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) throw std::system_error(errno, std::generic_category(), "Could not read extension file");
        if (!size) break;
        budget.bytes += size;
        if (budget.bytes > 256 * 1024 * 1024) throw std::runtime_error("The extension exceeds 256 MiB.");
        for (ssize_t offset = 0; offset < size;) {
          auto written = write(output.value, buffer + offset, size - offset);
          if (written < 0 && errno == EINTR) continue;
          if (written <= 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "Could not copy extension file");
          offset += written;
        }
      }
    } else throw std::runtime_error("Extensions must contain regular files and folders, without symbolic links.");
  }
}
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
Extension Extensions::read_manifest(const std::string& path) {
  Extension e;
  e.path = fs::absolute(path).lexically_normal().string();
  e.id = id_for_path(e.path);
  FileDescriptor directory(open(e.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  auto* parser = read_json(directory.value, "manifest.json");
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
    e.path_derived_id = false;
    gsize len = 0;
    auto* bytes = g_base64_decode(key.c_str(), &len);
    e.id = extension_id(bytes, len);
    g_free(bytes);
  }
  auto* action = json_object_get_member(o, e.manifest == 2 ? "browser_action" : "action");
  for (const char* name : {"action", "browser_action", "page_action"}) {
    auto* item = json_object_get_member(o, name);
    if (item && JSON_NODE_HOLDS_OBJECT(item)) e.has_action = true;
  }
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
    if (!locale.empty() && locale.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos) {
      try {
        FileDescriptor locales(openat(directory.value, "_locales", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        FileDescriptor language(openat(locales.value, locale.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        g_autoptr(JsonParser) lp = read_json(language.value, "messages.json");
        auto* root = json_parser_get_root(lp);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
          auto* msg = json_object_get_member(json_node_get_object(root), token.c_str());
          if (msg && JSON_NODE_HOLDS_OBJECT(msg))
            e.name = text(json_node_get_object(msg), "message");
        }
      } catch (const std::exception&) { /* Native loading reports invalid localization on restart. */ }
    }
  }
  g_object_unref(parser);
  if ((e.manifest != 2 && e.manifest != 3) || e.name.empty() || e.name.size() > 256 || e.version.empty() ||
      !local_page(e.popup) || !local_page(e.options))
    throw std::runtime_error(
        "The manifest must describe a Manifest V2 or V3 extension with valid local pages.");
  return e;
}
Extension Extensions::inspect(const std::string& path) const {
  auto e = read_manifest(path);
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
  const char* ubo = g_getenv("GNOSIS_UBLOCK_DIR");
  add(ubo ? ubo : GNOSIS_UBLOCK_DIR, true);
  for (const auto& p : fs::directory_iterator(directory_ + "/installed"))
    if (!p.path().filename().string().starts_with('.') && p.is_directory() && !p.is_symlink())
      add(p.path().string(), false);
}
std::string Extensions::load_paths() const {
  std::string paths = std::string(GNOSIS_BRIDGE_DIR) + "," + GNOSIS_MANAGER_DIR;
  for (const auto& e : installed)
    if (e.enabled)
      paths += "," + e.path;
  return paths;
}
Extensions::PendingInstall::~PendingInstall() {
  if (!staging_path.empty()) {
    std::error_code ignored;
    fs::remove_all(staging_path, ignored);
  }
}
std::unique_ptr<Extensions::PendingInstall> Extensions::prepare_install(const std::string& source,
                                                                      const std::string& directory,
                                                                      GCancellable* cancellable) {
  check_cancelled(cancellable);
  FileDescriptor input(open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  CopyBudget validation;
  copy_tree(input.value, -1, validation, cancellable);
  // Parse before creating anything, then parse the immutable owned copy again.
  read_manifest(source);
  if (directory.find(',') != std::string::npos)
    throw std::runtime_error("Chromium cannot load extensions from a profile path containing a comma.");
  FileDescriptor parent(open((directory + "/installed").c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  g_autofree char* uuid = g_uuid_string_random();
  auto pending = std::make_unique<PendingInstall>();
  pending->extension.path = directory + "/installed/" + uuid;
  const std::string staging = directory + "/installed/.install-" + uuid;
  if (mkdirat(parent.value, (std::string(".install-") + uuid).c_str(), 0700))
    throw std::system_error(errno, std::generic_category(), "Could not stage extension installation");
  pending->staging_path = staging;
  FileDescriptor output(open(pending->staging_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  CopyBudget copying;
  copy_tree(input.value, output.value, copying, cancellable);
  auto extension = read_manifest(pending->staging_path);
  extension.path = pending->extension.path;
  if (extension.path_derived_id) extension.id = id_for_path(extension.path);
  pending->extension = std::move(extension);
  check_cancelled(cancellable);
  return pending;
}
void Extensions::commit_install(PendingInstall& pending) {
  if (pending.staging_path.empty() || fs::path(pending.extension.path).parent_path() != fs::path(directory_) / "installed")
    throw std::runtime_error("The extension installation does not belong to this profile.");
  for (const auto& extension : installed)
    if (extension.id == pending.extension.id)
      throw std::runtime_error("An extension with this ID is already installed.");
  // Allocate the catalog entry before the atomic filesystem commit.
  pending.extension.restart_required = true;
  installed.push_back(pending.extension);
  if (syscall(SYS_renameat2, AT_FDCWD, pending.staging_path.c_str(), AT_FDCWD,
              pending.extension.path.c_str(), RENAME_NOREPLACE)) {
    const int failure = errno;
    installed.pop_back();
    throw std::system_error(failure, std::generic_category(), "Could not install extension");
  }
  pending.staging_path.clear();
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
