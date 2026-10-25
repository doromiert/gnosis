/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "application.hpp"
#include <algorithm>

void Browser::cancel_file_dialogs() {
  for (auto *cancel : file_dialogs_) g_cancellable_cancel(cancel);
}
bool Browser::OnFileDialog(CefRefPtr<CefBrowser> source, FileDialogMode mode, const CefString& title,
                           const CefString& default_path, const std::vector<CefString>& accept,
                           const std::vector<CefString>& extensions, const std::vector<CefString>& descriptions,
                           CefRefPtr<CefFileDialogCallback> callback) {
  auto *window = owner();
  if (!owns(source) || closing || !window) { callback->Cancel(); return true; }
  auto *session = host && host->session ? host->session : ephy_engine_session_get_default();
  auto *dialog = gtk_file_dialog_new();
  if (!title.empty()) gtk_file_dialog_set_title(dialog, title.ToString().c_str());
  if (!default_path.empty()) {
    auto *file = g_file_new_for_path(default_path.ToString().c_str());
    if (mode == FILE_DIALOG_SAVE) gtk_file_dialog_set_initial_file(dialog, file);
    else {
      auto *directory = g_file_test(default_path.ToString().c_str(), G_FILE_TEST_IS_DIR) ? G_FILE(g_object_ref(file)) : g_file_get_parent(file);
      if (directory) { gtk_file_dialog_set_initial_folder(dialog, directory); g_object_unref(directory); }
    }
    g_object_unref(file);
  }
  if (default_path.empty() && (mode == FILE_DIALOG_OPEN || mode == FILE_DIALOG_OPEN_MULTIPLE)) {
    g_autoptr(GFile) folder = ephy_engine_session_dup_last_upload_folder(session);
    // A portal backend can remember its own previous directory across private
    // and regular requests. Supply a safe explicit fallback instead of allowing
    // that shared portal state to override this session's empty memory.
    if (!folder) folder = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dialog, folder);
  }
  if (!accept.empty() && mode != FILE_DIALOG_OPEN_FOLDER) {
    auto *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    auto *supported = gtk_file_filter_new();
    std::string description = accept.size() == 1 && descriptions.size() == 1 ? descriptions.front().ToString() : "";
    if (description.empty() && accept.size() == 1) {
      std::string value = accept.front();
      auto separator = value.find('|');
      if (separator != std::string::npos) description = value.substr(0, separator);
    }
    gtk_file_filter_set_name(supported, description.empty() ? "Supported files" : description.c_str());
    bool has_supported_type = false;
    auto add_extension = [&](const std::string& extension) {
      if (extension.size() < 2 || !extension.starts_with('.') ||
          extension.find_first_of("*?[]{}\\/;|") != std::string::npos)
        return;
      // Suffix rules preserve literal extensions and match upper-case filenames
      // too. A web-provided extension must never become an arbitrary glob.
      gtk_file_filter_add_suffix(supported, extension.c_str() + 1);
      has_supported_type = true;
    };
    auto add_extensions = [&](const std::string& list) {
      char **expanded = g_strsplit(list.c_str(), ";", -1);
      for (char **extension = expanded; *extension; ++extension)
        add_extension(*extension);
      g_strfreev(expanded);
    };
    for (size_t i = 0; i < accept.size(); ++i) {
      std::string value = accept[i];
      auto separator = value.find('|');
      // RunFileDialog also accepts a native "Description|.ext;.ext2" group.
      if (separator != std::string::npos) add_extensions(value.substr(separator + 1));
      else if (value.starts_with('.')) add_extension(value);
      else if (value.find('/') != std::string::npos) {
        gtk_file_filter_add_mime_type(supported, value.c_str());
        has_supported_type = true;
      }
      // Chromium's expansion is authoritative when the desktop MIME database
      // does not know a type. Keep the MIME rule as well for wildcard types.
      if (i < extensions.size()) add_extensions(extensions[i]);
    }
    if (has_supported_type) {
      g_list_store_append(filters, supported);
      gtk_file_dialog_set_default_filter(dialog, supported);
    }
    g_object_unref(supported);
    auto *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all); g_object_unref(all);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
  }
  struct Request {
    CefRefPtr<Browser> browser;
    CefRefPtr<CefFileDialogCallback> callback;
    GCancellable *cancel;
    FileDialogMode mode;
    EphyEngineSession *session;
  };
  auto *request = new Request{this, callback, g_cancellable_new(), mode, EPHY_ENGINE_SESSION(g_object_ref(session))};
  file_dialogs_.push_back(request->cancel);
  auto done = +[](GObject *source, GAsyncResult *result, gpointer user) {
    auto *request = static_cast<Request*>(user);
    std::vector<CefString> paths;
    auto append = [&](GFile *file) {
      if (file) { char *path = g_file_get_path(file); if (path) paths.push_back(path); g_free(path); g_object_unref(file); }
    };
    auto *dialog = GTK_FILE_DIALOG(source);
    switch (request->mode) {
    case FILE_DIALOG_OPEN_MULTIPLE: {
      auto *files = gtk_file_dialog_open_multiple_finish(dialog, result, nullptr);
      if (files) {
        for (unsigned i = 0; i < g_list_model_get_n_items(files); i++) append(G_FILE(g_list_model_get_item(files, i)));
        g_object_unref(files);
      }
      break;
    }
    case FILE_DIALOG_OPEN_FOLDER: append(gtk_file_dialog_select_folder_finish(dialog, result, nullptr)); break;
    case FILE_DIALOG_SAVE: append(gtk_file_dialog_save_finish(dialog, result, nullptr)); break;
    default: append(gtk_file_dialog_open_finish(dialog, result, nullptr)); break;
    }
    std::erase(request->browser->file_dialogs_, request->cancel);
    if (paths.empty() || request->browser->closing || g_cancellable_is_cancelled(request->cancel)) request->callback->Cancel();
    else {
      if (request->mode == FILE_DIALOG_OPEN || request->mode == FILE_DIALOG_OPEN_MULTIPLE) {
        auto *file = g_file_new_for_path(paths.front().ToString().c_str());
        auto *folder = g_file_get_parent(file);
        g_object_unref(file);
        if (folder) {
          ephy_engine_session_set_last_upload_folder(request->session, folder);
          g_object_unref(folder);
        }
      }
      request->callback->Continue(paths);
    }
    g_object_unref(request->session);
    g_object_unref(request->cancel); delete request;
    g_object_unref(dialog);
  };
  switch (mode) {
  case FILE_DIALOG_OPEN_MULTIPLE: gtk_file_dialog_open_multiple(dialog, window->window, request->cancel, done, request); break;
  case FILE_DIALOG_OPEN_FOLDER: gtk_file_dialog_select_folder(dialog, window->window, request->cancel, done, request); break;
  case FILE_DIALOG_SAVE: gtk_file_dialog_save(dialog, window->window, request->cancel, done, request); break;
  default: gtk_file_dialog_open(dialog, window->window, request->cancel, done, request); break;
  }
  return true;
}
