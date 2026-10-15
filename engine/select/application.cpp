#include "application.hpp"
Application::Application(const std::string& profile) : extensions(profile) {
  extension_host = new ExtensionHost;
  const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  download_folder = downloads ? downloads : g_get_home_dir();
}
void Application::external(const std::string& uri) {
  auto* launcher = gtk_uri_launcher_new(uri.c_str());
  gtk_uri_launcher_launch(launcher, nullptr, nullptr, nullptr, nullptr);
  g_object_unref(launcher);
}
void Application::export_completed(const std::string& path, const std::string&, const std::string&) {
  g_message("Download completed: %s", path.c_str());
}
