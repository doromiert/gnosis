#pragma once
#include <string>
#include <vector>
#include <memory>
#include <gio/gio.h>
struct Extension {
  std::string id, name, version, path, popup, options;
  int manifest = 0;
  bool enabled = true, builtin = false, has_action = false;
  bool path_derived_id = true;
  bool restart_required = false;
  std::vector<std::string> permissions;
};
class Extensions {
public:
  struct PendingInstall {
    Extension extension;
    std::string staging_path;
    ~PendingInstall();
  };
  explicit Extensions(std::string data);
  std::vector<Extension> installed;
  std::string load_paths() const;
  Extension inspect(const std::string& path) const;
  static Extension read_manifest(const std::string& path);
  const std::string& directory() const { return directory_; }
  /* Preparation owns only immutable paths; safe on a worker thread. */
  static std::unique_ptr<PendingInstall> prepare_install(const std::string& source,
                                                        const std::string& directory,
                                                        GCancellable*);
  /* UI-thread publication after duplicate/profile/cancellation checks. */
  void commit_install(PendingInstall&);
  void set_enabled(const Extension&, bool);
  static std::string id_for_path(const std::string& path);

private:
  std::string directory_;
};
