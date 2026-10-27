/* SPDX-License-Identifier: GPL-3.0-or-later
 * An owned temporary directory. Only the creator's exact directory is removed,
 * and symlinks inside it are unlinked without ever following their targets.
 */
#pragma once
#include <glib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <string>
#include <utility>

namespace GnosisBrave {
class PrivateProfile {
  std::string path_, name_;
  int parent_ = -1;
  dev_t device_ = 0;
  ino_t inode_ = 0;
  static bool clear(int directory, unsigned depth = 0) {
    if (depth > 128) return false;
    int listing = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listing < 0) return false;
    DIR* entries = fdopendir(listing);
    if (!entries) { close(listing); return false; }
    bool success = true;
    for (;;) {
      errno = 0;
      auto* entry = readdir(entries);
      if (!entry) { if (errno) success = false; break; }
      if (!g_strcmp0(entry->d_name,".") || !g_strcmp0(entry->d_name,"..")) continue;
      struct stat value{};
      if (fstatat(directory, entry->d_name, &value, AT_SYMLINK_NOFOLLOW)) { if (errno != ENOENT) success = false; continue; }
      if (S_ISDIR(value.st_mode)) {
        int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) { success = false; continue; }
        bool emptied = clear(child, depth + 1); close(child);
        if (!emptied || unlinkat(directory,entry->d_name,AT_REMOVEDIR)) success = false;
      } else if (unlinkat(directory,entry->d_name,0) && errno != ENOENT) success = false;
    }
    closedir(entries);
    return success;
  }
  explicit PrivateProfile(std::string path) : path_(std::move(path)) {
    g_autofree char* parent = g_path_get_dirname(path_.c_str());
    g_autofree char* name = g_path_get_basename(path_.c_str());
    name_ = name;
    parent_ = open(parent,O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat value{};
    if (parent_ >= 0 && !fstatat(parent_,name_.c_str(),&value,AT_SYMLINK_NOFOLLOW) &&
        S_ISDIR(value.st_mode) && value.st_uid == getuid()) { device_ = value.st_dev; inode_ = value.st_ino; }
  }
public:
  static std::shared_ptr<PrivateProfile> create() {
    g_autoptr(GError) error = nullptr;
    g_autofree char* path = g_dir_make_tmp("gnosis-brave-private-XXXXXX",&error);
    if (!path) { g_warning("Cannot create private browsing profile: %s",error->message); return {}; }
    auto result = std::shared_ptr<PrivateProfile>(new PrivateProfile(path));
    if (result->parent_ < 0 || !result->inode_) { g_warning("Cannot identify the owned private browsing profile"); return {}; }
    return result;
  }
  PrivateProfile(const PrivateProfile&) = delete;
  PrivateProfile& operator=(const PrivateProfile&) = delete;
  ~PrivateProfile() {
    struct stat value{};
    if (parent_ >= 0 && inode_ && !fstatat(parent_,name_.c_str(),&value,AT_SYMLINK_NOFOLLOW)) {
      if (S_ISDIR(value.st_mode) && value.st_uid == getuid() && value.st_dev == device_ && value.st_ino == inode_) {
        int directory = openat(parent_,name_.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat opened{}, final{};
        bool identified = directory >= 0 && !fstat(directory,&opened) &&
          opened.st_dev == device_ && opened.st_ino == inode_;
        bool emptied = identified && clear(directory);
        if (directory >= 0) close(directory);
        bool unchanged = !fstatat(parent_,name_.c_str(),&final,AT_SYMLINK_NOFOLLOW) &&
          S_ISDIR(final.st_mode) && final.st_dev == device_ && final.st_ino == inode_;
        if (!emptied || !unchanged || unlinkat(parent_,name_.c_str(),AT_REMOVEDIR))
          g_warning("Could not finish removing private browsing data");
      } else g_warning("Private profile path changed; refusing to remove an unowned directory");
    }
    if (parent_ >= 0) close(parent_);
  }
  const std::string& path() const { return path_; }
};
}
