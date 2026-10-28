/* SPDX-License-Identifier: GPL-3.0-or-later
 * Profile-owned updates of a trusted packaged unpacked extension. The absolute
 * directory stays unchanged: Chromium derives keyless unpacked IDs from it.
 */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <unistd.h>
#include "../../lib/gnosis-rebrand.h"

namespace GnosisBrave {
class BundledExtensionUpdate {
  struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& from) noexcept : value(from.value) { from.value = -1; }
  };
  struct Manifest { std::string version, key, name; };
  static constexpr const char* target_name = "builtin-ublock";
  static constexpr const char* backup_name = ".builtin-ublock.previous";
  Fd parent_, lock_;
  std::string directory_, id_, version_, staging_, previous_;
  bool changed_ = false, armed_ = false, committed_ = false;

  static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }
  static void checked(bool success, const char* message) {
    if (!success) throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
  }
  static Fd open_directory(int parent, const char* name, bool owned) {
    Fd result(openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    checked(result.value >= 0, "Cannot open bundled extension directory");
    struct stat info{};
    checked(!fstat(result.value, &info), "Cannot inspect bundled extension directory");
    require(S_ISDIR(info.st_mode), "Bundled extension path is not a directory");
    if (owned) require(info.st_uid == getuid() && !(info.st_mode & (S_IWGRP | S_IWOTH)),
                       "Bundled extension directory has unsafe ownership or permissions");
    return result;
  }
  static bool exists(int parent, const char* name) {
    struct stat info{};
    if (!fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW)) return true;
    checked(errno == ENOENT, "Cannot inspect bundled extension path"); return false;
  }
  static std::array<unsigned, 4> version_numbers(const std::string& value) {
    std::array<unsigned, 4> numbers{};
    size_t start = 0;
    for (unsigned i = 0; ; ++i) {
      require(i < numbers.size(), "Invalid packaged extension version");
      auto end = value.find('.', start);
      const auto size = (end == std::string::npos ? value.size() : end) - start;
      require(size > 0 && size <= 5, "Invalid packaged extension version");
      for (size_t j = start; j < start + size; ++j) {
        require(value[j] >= '0' && value[j] <= '9', "Invalid packaged extension version");
        numbers[i] = numbers[i] * 10 + unsigned(value[j] - '0');
      }
      require(numbers[i] <= 65535, "Invalid packaged extension version");
      if (end == std::string::npos) break;
      start = end + 1;
    }
    return numbers;
  }
  static Manifest manifest(int directory) {
    Fd file(openat(directory, "manifest.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    checked(file.value >= 0, "Cannot read bundled extension manifest");
    struct stat info{};
    checked(!fstat(file.value, &info), "Cannot inspect bundled extension manifest");
    require(S_ISREG(info.st_mode) && info.st_size >= 0 && info.st_size <= 1024 * 1024,
            "Invalid bundled extension manifest file");
    std::string bytes; char chunk[16384];
    for (;;) {
      auto size = read(file.value, chunk, sizeof chunk);
      if (size < 0 && errno == EINTR) continue;
      checked(size >= 0, "Cannot read bundled extension manifest");
      if (!size) break;
      bytes.append(chunk, size);
      require(bytes.size() <= 1024 * 1024, "Bundled extension manifest is too large");
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    require(json_parser_load_from_data(parser, bytes.data(), bytes.size(), nullptr), "Invalid bundled extension manifest JSON");
    auto* root = json_parser_get_root(parser);
    require(root && JSON_NODE_HOLDS_OBJECT(root), "Invalid bundled extension manifest object");
    auto* object = json_node_get_object(root);
    auto text = [&](const char* name) {
      auto* node = json_object_get_member(object, name);
      return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
        std::string(json_node_get_string(node)) : std::string();
    };
    auto* format = json_object_get_member(object, "manifest_version");
    require(format && JSON_NODE_HOLDS_VALUE(format) && json_node_get_value_type(format) == G_TYPE_INT64 &&
      json_node_get_int(format) == 2, "The bundled provider must be full Manifest V2 uBlock Origin");
    Manifest result{text("version"), text("key"), text("name")};
    version_numbers(result.version);
    require(!result.name.empty(), "Bundled extension name is missing");
    return result;
  }
  static std::string identity(const Manifest& value, const std::string& directory) {
    gsize size = 0;
    g_autofree guchar* key = value.key.empty() ? nullptr : g_base64_decode(value.key.c_str(), &size);
    if (!value.key.empty()) require(size > 0, "Invalid bundled extension identity key");
    auto path = std::filesystem::absolute(directory).lexically_normal().string();
    const auto* bytes = key ? key : reinterpret_cast<const guchar*>(path.data());
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, bytes, key ? size : path.size());
    guchar digest[32]; gsize length = sizeof digest; g_checksum_get_digest(checksum, digest, &length);
    std::string id;
    for (unsigned i = 0; i < 16; ++i) { id += char('a' + (digest[i] >> 4)); id += char('a' + (digest[i] & 15)); }
    return id;
  }
  struct Budget { guint64 bytes = 0; unsigned entries = 0; };
  static void tree(int input, int output, bool owned, Budget& budget, unsigned depth = 0) {
    require(depth <= 64, "Bundled extension tree is too deep");
    Fd listing(openat(input, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    checked(listing.value >= 0, "Cannot list bundled extension");
    auto* raw = fdopendir(listing.value); checked(raw != nullptr, "Cannot list bundled extension");
    listing.value = -1;
    std::unique_ptr<DIR, decltype(&closedir)> entries(raw, closedir);
    for (;;) {
      errno = 0; auto* entry = readdir(entries.get());
      if (!entry) { checked(!errno, "Cannot list bundled extension"); break; }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      require(++budget.entries <= 20000, "Bundled extension has too many files");
      struct stat info{};
      checked(!fstatat(input, entry->d_name, &info, AT_SYMLINK_NOFOLLOW), "Cannot inspect bundled extension entry");
      require(!owned || (info.st_uid == getuid() && !(info.st_mode & (S_IWGRP | S_IWOTH))),
              "Bundled extension entry has unsafe ownership or permissions");
      if (S_ISDIR(info.st_mode)) {
        auto from = open_directory(input, entry->d_name, owned);
        if (output >= 0) {
          checked(!mkdirat(output, entry->d_name, 0700), "Cannot create bundled extension staging directory");
          auto to = open_directory(output, entry->d_name, true);
          tree(from.value, to.value, owned, budget, depth + 1);
          checked(!fsync(to.value), "Cannot flush bundled extension directory");
        } else tree(from.value, -1, owned, budget, depth + 1);
      } else {
        require(S_ISREG(info.st_mode), "Symlinks and special files are not allowed in bundled extensions");
        Fd from(openat(input, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        checked(from.value >= 0, "Cannot open bundled extension file");
        checked(!fstat(from.value, &info), "Cannot inspect opened bundled extension file");
        require(S_ISREG(info.st_mode) && info.st_size >= 0 && guint64(info.st_size) <= 256 * 1024 * 1024 - budget.bytes,
                "Bundled extension exceeds its size limit");
        if (output < 0) { budget.bytes += info.st_size; continue; }
        Fd to(openat(output, entry->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        checked(to.value >= 0, "Cannot create bundled extension file");
        char bytes[65536];
        for (;;) {
          auto size = read(from.value, bytes, sizeof bytes);
          if (size < 0 && errno == EINTR) continue;
          checked(size >= 0, "Cannot read bundled extension file");
          if (!size) break;
          budget.bytes += size; require(budget.bytes <= 256 * 1024 * 1024, "Bundled extension exceeds its size limit");
          for (ssize_t written = 0; written < size;) {
            auto count = write(to.value, bytes + written, size - written);
            if (count < 0 && errno == EINTR) continue;
            checked(count > 0, "Cannot write bundled extension file"); written += count;
          }
        }
        checked(!fsync(to.value), "Cannot flush bundled extension file");
      }
    }
  }
  static void remove_tree(int parent, const std::string& name) {
    auto directory = open_directory(parent, name.c_str(), true);
    Fd listing(openat(directory.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    checked(listing.value >= 0, "Cannot list retired bundled extension");
    auto* raw = fdopendir(listing.value); checked(raw != nullptr, "Cannot list retired bundled extension");
    listing.value = -1;
    std::unique_ptr<DIR, decltype(&closedir)> entries(raw, closedir);
    for (;;) {
      errno = 0; auto* entry = readdir(entries.get());
      if (!entry) { checked(!errno, "Cannot list retired bundled extension"); break; }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      struct stat info{};
      checked(!fstatat(directory.value, entry->d_name, &info, AT_SYMLINK_NOFOLLOW), "Cannot inspect retired bundled extension");
      require(info.st_uid == getuid(), "Retired bundled extension has unsafe ownership");
      if (S_ISDIR(info.st_mode)) remove_tree(directory.value, entry->d_name);
      else checked(!unlinkat(directory.value, entry->d_name, 0), "Cannot remove retired bundled extension file");
    }
    checked(!unlinkat(parent, name.c_str(), AT_REMOVEDIR), "Cannot remove retired bundled extension directory");
  }
  void exchange(const std::string& left, const std::string& right) {
    checked(!syscall(SYS_renameat2, parent_.value, left.c_str(), parent_.value, right.c_str(), RENAME_EXCHANGE),
            "Cannot atomically exchange bundled extension directories");
  }
  void unlock() {
    if (lock_.value >= 0) { ::close(lock_.value); lock_.value = -1; }
  }
  explicit BundledExtensionUpdate(const std::string& profile) :
    parent_(open_directory(AT_FDCWD, profile.c_str(), true)),
    lock_(openat(parent_.value, ".builtin-ublock.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600)),
    directory_(std::filesystem::absolute(profile).lexically_normal().string() + '/' + target_name) {
    checked(lock_.value >= 0, "Cannot open bundled extension update lock");
    struct stat info{}; checked(!fstat(lock_.value, &info), "Cannot inspect bundled extension update lock");
    require(S_ISREG(info.st_mode) && info.st_uid == getuid() && !(info.st_mode & (S_IWGRP | S_IWOTH)),
            "Unsafe bundled extension update lock");
    checked(!flock(lock_.value, LOCK_EX | LOCK_NB), "Another bundled extension update is in progress");
  }
public:
  ~BundledExtensionUpdate() {
    if (!committed_) try { rollback(); }
    catch (const std::exception& error) { g_warning("Bundled extension recovery: %s", error.what()); }
  }
  BundledExtensionUpdate(const BundledExtensionUpdate&) = delete;
  BundledExtensionUpdate& operator=(const BundledExtensionUpdate&) = delete;
  static std::string packaged_id(const std::string& profile, const std::string& source) {
    g_autofree char* identity_profile = gnosis_legacy_extension_profile(profile.c_str());
    auto directory = open_directory(AT_FDCWD, source.c_str(), false);
    return identity(manifest(directory.value), std::string(identity_profile) + '/' + target_name);
  }
  /* Empty native_version means a requested first registration. A newer already
   * installed provider is never downgraded during an application rollback. */
  static std::shared_ptr<BundledExtensionUpdate> prepare(const std::string& profile, const std::string& source,
                                                        const std::string& native_version) {
    g_autofree char* identity_profile = gnosis_legacy_extension_profile(profile.c_str());
    auto update = std::shared_ptr<BundledExtensionUpdate>(new BundledExtensionUpdate(identity_profile));
    auto input = open_directory(AT_FDCWD, source.c_str(), false);
    const auto packaged = manifest(input.value);
    update->id_ = identity(packaged, update->directory_);
    if (!native_version.empty() && version_numbers(native_version) > version_numbers(packaged.version)) {
      update->committed_ = true; return nullptr;
    }
    bool present = exists(update->parent_.value, target_name);
    if (exists(update->parent_.value, backup_name)) {
      auto previous = open_directory(update->parent_.value, backup_name, true);
      const auto value = manifest(previous.value);
      require(value.key == packaged.key && value.name == packaged.name, "Retired bundled extension identity changed");
      Budget budget; tree(previous.value, -1, true, budget);
      if (!present) {
        checked(!renameat(update->parent_.value, backup_name, update->parent_.value, target_name), "Cannot recover bundled extension");
        checked(!fsync(update->parent_.value), "Cannot flush recovered bundled extension"); present = true;
      } else update->previous_ = backup_name;
    }
    Manifest installed;
    if (present) {
      auto current = open_directory(update->parent_.value, target_name, true);
      installed = manifest(current.value);
      require(installed.key == packaged.key && installed.name == packaged.name, "Bundled extension identity changed");
      Budget budget; tree(current.value, -1, true, budget);
    }
    update->version_ = present && version_numbers(installed.version) > version_numbers(packaged.version) ? installed.version : packaged.version;
    if (present && version_numbers(installed.version) >= version_numbers(packaged.version)) {
      if (native_version == installed.version) { update->commit(); return nullptr; }
      update->armed_ = !update->previous_.empty();
      return update;
    }
    g_autofree char* unique = g_uuid_string_random();
    update->staging_ = std::string(".builtin-ublock.stage-") + unique;
    checked(!mkdirat(update->parent_.value, update->staging_.c_str(), 0700), "Cannot create bundled extension staging directory");
    auto output = open_directory(update->parent_.value, update->staging_.c_str(), true);
    Budget budget; tree(input.value, output.value, false, budget);
    const auto copied = manifest(output.value);
    require(copied.version == packaged.version && copied.key == packaged.key && copied.name == packaged.name,
            "Packaged extension changed while copying");
    checked(!fsync(output.value), "Cannot flush bundled extension staging tree");
    if (present) {
      update->exchange(update->staging_, target_name);
      update->changed_ = update->armed_ = true;
      auto old_backup = std::exchange(update->previous_, update->staging_);
      if (!old_backup.empty()) remove_tree(update->parent_.value, old_backup);
      checked(!renameat(update->parent_.value, update->staging_.c_str(), update->parent_.value, backup_name), "Cannot retain previous bundled extension");
      update->previous_ = backup_name;
    } else {
      checked(!renameat(update->parent_.value, update->staging_.c_str(), update->parent_.value, target_name), "Cannot publish bundled extension");
      update->changed_ = update->armed_ = true;
    }
    update->staging_.clear();
    checked(!fsync(update->parent_.value), "Cannot flush bundled extension publication");
    return update;
  }
  const std::string& directory() const { return directory_; }
  const std::string& id() const { return id_; }
  const std::string& version() const { return version_; }
  void commit() {
    committed_ = true;
    if (!previous_.empty()) { remove_tree(parent_.value, previous_); previous_.clear(); }
    checked(!fsync(parent_.value), "Cannot flush bundled extension completion");
    unlock();
  }
  void rollback() {
    if (armed_ && !previous_.empty()) {
      exchange(previous_, target_name);
      const auto retired = std::exchange(previous_, "");
      changed_ = armed_ = false; committed_ = true;
      if (staging_ == retired) staging_.clear();
      remove_tree(parent_.value, retired);
    } else if (changed_) remove_tree(parent_.value, target_name);
    if (!staging_.empty() && exists(parent_.value, staging_.c_str())) remove_tree(parent_.value, staging_);
    staging_.clear(); changed_ = armed_ = false; committed_ = true;
    checked(!fsync(parent_.value), "Cannot flush bundled extension rollback");
    unlock();
  }
};
}
