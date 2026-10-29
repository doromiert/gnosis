/* SPDX-License-Identifier: GPL-3.0-or-later; included inside Process. */
  GFileMonitor* keepassxc_pairing_monitor = nullptr;
  std::string keepassxc_pairing_path, keepassxc_pairing_json;
  bool keepassxc_pairing_publisher = false;

  void redetect_keepassxc_pages() {
    for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && !page->is_closed() &&
        page->state_->native_tab >= 0 && (page->uri.starts_with("https://") || page->uri.starts_with("http://")))
      keepassxc_call(page->state_->native_tab, "redetect", "null", [](JsonObject*, const std::string&) {});
  }
  void sync_keepassxc_pairing() {
    if (closing || !keepassxc_ready || keepassxc_pairing_path.empty() || keepassxc_pairing_publisher) return;
    g_autoptr(GError) error = nullptr;
    g_autofree char* json = gnosis_brave_keepassxc_pairing_load(keepassxc_pairing_path.c_str(), &error);
    if (!json) {
      g_warning("KeePassXC association sharing: %s", error->message);
      redetect_keepassxc_pages(); return;
    }
    if (keepassxc_pairing_json == json) return;
    std::string value = json;
    command("Runtime.evaluate", "{\"expression\":" + quote("gnosisKeePassXCPairing.apply(" + value + ")") +
      ",\"awaitPromise\":true,\"returnByValue\":true}", keepassxc_session,
      [weak = weak_from_this(), value](JsonObject* result, const std::string& error) {
        auto self = weak.lock(); if (!self || self->closing) return;
        if (!result || !error.empty() || object(result, "exceptionDetails")) {
          g_warning("Could not inherit Gnosis's KeePassXC association"); return;
        }
        self->keepassxc_pairing_json = value;
        self->redetect_keepassxc_pages();
      });
  }
  void start_keepassxc_pairing() {
    if (private_profile || !webapp_keepassxc || !adapter_enabled("keepassxc")) return;
    g_autofree char* normal = g_build_filename(g_get_user_data_dir(), "gnosis", nullptr);
    g_autofree char* current = g_canonicalize_filename(profile_root.c_str(), nullptr);
    g_autofree char* marker = g_build_filename(profile_root.c_str(), ".app", nullptr);
    const bool webapp = g_file_test(marker, G_FILE_TEST_IS_REGULAR);
    if (!webapp && profile_root != normal && strcmp(current, normal)) {
      redetect_keepassxc_pages(); return; // Explicit custom profiles retain their own association.
    }
    keepassxc_pairing_path = std::string(normal) + "/keepassxc-associations.json";
    keepassxc_pairing_json.clear();
    keepassxc_pairing_publisher = !webapp;
    if (!webapp) {
      command("Runtime.evaluate", "{\"expression\":\"gnosisKeePassXCPairing.publish()\",\"awaitPromise\":true}", keepassxc_session,
        [weak = weak_from_this()](JsonObject*, const std::string&) {
          if (auto self = weak.lock(); self && !self->closing) self->redetect_keepassxc_pages();
        });
      return;
    }
    if (!keepassxc_pairing_monitor && !g_mkdir_with_parents(normal, 0700)) {
      g_autoptr(GFile) file = g_file_new_for_path(keepassxc_pairing_path.c_str());
      keepassxc_pairing_monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
      if (keepassxc_pairing_monitor) {
        g_file_monitor_set_rate_limit(keepassxc_pairing_monitor, 100);
        g_signal_connect(keepassxc_pairing_monitor, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer data) {
          static_cast<Process*>(data)->sync_keepassxc_pairing();
        }), this);
      } else g_warning("Could not watch Gnosis's KeePassXC association");
    }
    sync_keepassxc_pairing();
  }
  void stop_keepassxc_pairing() {
    if (!keepassxc_pairing_monitor) return;
    g_signal_handlers_disconnect_by_data(keepassxc_pairing_monitor, this);
    g_file_monitor_cancel(keepassxc_pairing_monitor);
    g_clear_object(&keepassxc_pairing_monitor);
  }
