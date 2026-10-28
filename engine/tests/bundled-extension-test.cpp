/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "brave/bundled-extension.hpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <sys/wait.h>
namespace fs = std::filesystem;
using GnosisBrave::BundledExtensionUpdate;
struct Fixture {
  fs::path root, profile, source;
  Fixture() {
    auto* directory = g_dir_make_tmp("gnosis-bundled-test-XXXXXX", nullptr); assert(directory);
    root = directory; g_free(directory);
    profile = root / "profile"; source = root / "package";
    fs::create_directory(profile); fs::permissions(profile, fs::perms::owner_all);
    package("1.0");
  }
  ~Fixture() { fs::remove_all(root); }
  void package(const char* version, const char* key = "") {
    fs::remove_all(source); fs::create_directory(source); fs::create_directory(source / "nested");
    std::ofstream(source / "manifest.json") << "{\"name\":\"uBlock Origin\",\"manifest_version\":2,\"version\":\"" << version << "\",\"key\":\"" << key << "\"}";
    std::ofstream(source / "nested" / "asset.js") << version;
  }
  std::shared_ptr<BundledExtensionUpdate> prepare(const char* native = "") {
    return BundledExtensionUpdate::prepare(profile.string(), source.string(), native);
  }
  std::string contents(const fs::path& path) {
    std::ifstream input(path); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  void installed(const char* version) { auto update = prepare(); assert(update && update->version() == version); update->commit(); }
  fs::path target() const { return profile / "builtin-ublock"; }
};
static void rejected(const std::function<void()>& callback) {
  bool threw = false; try { callback(); } catch (const std::exception&) { threw = true; }
  assert(threw);
}
int main() {
  {
    Fixture f;
    auto identity = BundledExtensionUpdate::packaged_id(f.profile.string(), f.source.string());
    auto update = f.prepare(); assert(update && update->id() == identity && identity.size() == 32);
    assert(f.contents(f.target() / "nested/asset.js") == "1.0");
    update->rollback(); assert(!fs::exists(f.target()));
    f.installed("1.0");
    assert(!f.prepare("1.0"));
    std::ofstream(f.target() / "obsolete.txt") << "old package asset";
    f.package("1.1");
    update = f.prepare("1.0"); assert(update && update->id() == identity);
    assert(!fs::exists(f.target() / "obsolete.txt"));
    update->rollback();
    assert(f.contents(f.target() / "nested/asset.js") == "1.0" && fs::exists(f.target() / "obsolete.txt"));
    update = f.prepare("1.0"); update->commit();
    assert(f.contents(f.target() / "nested/asset.js") == "1.1" && !fs::exists(f.target() / "obsolete.txt"));
    assert(!fs::exists(f.profile / ".builtin-ublock.previous"));
    std::cout << "BUNDLE_ATOMIC_IDENTITY_ROLLBACK_PASS\n";
  }
  {
    Fixture f; f.package("1.9"); f.installed("1.9"); f.package("1.10");
    auto update = f.prepare("1.9"); assert(update && update->version() == "1.10"); update->commit();
    f.package("1.2"); assert(!f.prepare("1.10"));
    assert(f.contents(f.target() / "nested/asset.js") == "1.10");
    std::cout << "BUNDLE_NUMERIC_VERSION_NO_DOWNGRADE_PASS\n";
  }
  {
    Fixture f; f.installed("1.0"); f.package("2.0", "a2V5");
    rejected([&] { f.prepare("1.0"); });
    assert(f.contents(f.target() / "nested/asset.js") == "1.0");
    std::cout << "BUNDLE_IDENTITY_CHANGE_REJECTED_PASS\n";
  }
  {
    Fixture f; f.installed("1.0"); f.package("2.0");
    auto external = f.root / "outside"; std::ofstream(external) << "untouched";
    fs::create_symlink(external, f.source / "unsafe");
    rejected([&] { f.prepare("1.0"); });
    assert(f.contents(external) == "untouched" && f.contents(f.target() / "nested/asset.js") == "1.0");
    fs::remove(f.source / "unsafe");
    fs::create_symlink(external, f.target() / "unsafe");
    rejected([&] { f.prepare("1.0"); });
    assert(f.contents(external) == "untouched");
    fs::remove(f.target() / "unsafe");
    fs::rename(f.target(), f.profile / "real"); fs::create_directory_symlink(f.profile / "real", f.target());
    rejected([&] { f.prepare("1.0"); });
    assert(f.contents(external) == "untouched");
    std::cout << "BUNDLE_SYMLINK_NO_EXTERNAL_WRITES_PASS\n";
  }
  {
    Fixture f; f.installed("1.0"); f.package("2.0");
    fs::permissions(f.target(), fs::perms::group_write, fs::perm_options::add);
    rejected([&] { f.prepare("1.0"); });
    fs::permissions(f.target(), fs::perms::group_write, fs::perm_options::remove);
    auto update = f.prepare("1.0");
    rejected([&] { f.prepare("1.0"); });
    update->rollback();
    std::cout << "BUNDLE_PERMISSION_AND_CONCURRENT_UPDATE_PASS\n";
  }
  {
    Fixture f; f.installed("1.0"); f.package("2.0");
    auto child = fork(); assert(child >= 0);
    if (!child) { auto update = f.prepare("1.0"); assert(update); _exit(0); }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(fs::exists(f.profile / ".builtin-ublock.previous"));
    auto resumed = f.prepare("1.0"); assert(resumed && resumed->version() == "2.0");
    resumed->rollback(); assert(f.contents(f.target() / "nested/asset.js") == "1.0");
    auto update = f.prepare("1.0"); update->commit();
    assert(!f.prepare("2.0"));
    std::cout << "BUNDLE_CRASH_RECOVERY_PASS\n";
  }
  {
    Fixture f; f.installed("1.0");
    fs::rename(f.target(), f.profile / ".builtin-ublock.previous");
    assert(!f.prepare("1.0"));
    assert(f.contents(f.target() / "nested/asset.js") == "1.0");
    f.package("2.0"); auto update = f.prepare("1.0"); update.reset();
    assert(f.contents(f.target() / "nested/asset.js") == "1.0");
    std::cout << "BUNDLE_MISSING_TARGET_AND_CANCEL_RECOVERY_PASS\n";
  }
}
