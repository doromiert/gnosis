#include "brave/private-profile.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
using JupiterBrave::PrivateProfile;
int main() {
  auto owned=PrivateProfile::create();assert(owned);fs::path path=owned->path();
  fs::create_directories(path/"Default/nested");std::ofstream(path/"Default/nested/Cookies")<<"fixture";
  auto other=PrivateProfile::create();fs::path outside=other->path();std::ofstream(outside/"keep")<<"outside";
  fs::create_directory_symlink(outside,path/"external");
  auto successor=owned;owned.reset();assert(fs::exists(path));
  successor.reset();assert(!fs::exists(path));assert(fs::exists(outside/"keep"));
  g_print("PRIVATE_PROFILE_OWNERSHIP_TRANSFER_AND_NO_SYMLINK_FOLLOW_PASS\n");
  auto replaced=PrivateProfile::create();fs::path original=replaced->path(),moved=original.string()+"-moved";
  fs::rename(original,moved);fs::create_directory(original);std::ofstream(original/"keep")<<"replacement";
  replaced.reset();assert(fs::exists(original/"keep"));fs::remove_all(original);fs::remove_all(moved);
  other.reset();assert(!fs::exists(outside));
  g_print("PRIVATE_PROFILE_REPLACED_PATH_REFUSED_PASS\n");
}
