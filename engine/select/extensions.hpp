#pragma once
#include <string>
#include <vector>
struct Extension {
  std::string id, name, version, path, popup, options;
  int manifest = 0;
  bool enabled = true, builtin = false;
  std::vector<std::string> permissions;
};
class Extensions {
public:
  explicit Extensions(std::string data);
  std::vector<Extension> installed;
  std::string load_paths() const;
  Extension inspect(const std::string& path) const;
  void install(const std::string& path);
  void set_enabled(const Extension&, bool);
  static std::string id_for_path(const std::string& path);

private:
  std::string directory_;
};
