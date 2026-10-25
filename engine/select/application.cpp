#include "application.hpp"
Application::Application(const std::string& profile) : extensions(profile) {
  extension_host = new ExtensionHost;
  const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  download_folder = downloads ? downloads : g_get_home_dir();
}
void Application::export_completed(const std::string& path, const std::string&, const std::string&) {
  g_message("Download completed: %s", path.c_str());
}
