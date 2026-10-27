/* SPDX-License-Identifier: GPL-3.0-or-later; included inside Process. */
  bool webapp_configured = false, webapp_ublock = true, webapp_keepassxc = true;
  std::vector<std::pair<std::string, std::string>> webapp_extensions;

  void read_webapp_extensions() {
    if (private_profile) return;
    g_autofree char* marker = g_build_filename(profile_root.c_str(), ".app", nullptr);
    if (!g_file_test(marker, G_FILE_TEST_IS_REGULAR)) return;
    g_autoptr(GError) error = nullptr;
    g_autoptr(GKeyFile) config = gnosis_web_app_config_load(profile_root.c_str(), &error);
    if (config && !g_key_file_has_group(config, "Extensions")) return;
    webapp_configured = true;
    webapp_ublock = config && gnosis_web_app_extension_enabled(config, "ublock");
    webapp_keepassxc = config && gnosis_web_app_extension_enabled(config, "keepassxc");
    if (!config) { g_warning("Web app extensions: %s", error->message); return; }
    g_auto(GStrv) enabled = g_key_file_get_string_list(config, "Extensions", "enabled", nullptr, nullptr);
    for (guint i = 0; enabled && enabled[i]; i++) {
      if (!strcmp(enabled[i], "ublock") || !strcmp(enabled[i], "keepassxc")) continue;
      g_autofree char* path = g_key_file_get_string(config, enabled[i], "path", nullptr);
      try {
        auto extension = Extensions::read_manifest(path);
        if (extension.id == enabled[i]) webapp_extensions.emplace_back(extension.id, extension.path);
        else g_warning("Web app extension %s changed identity; skipping it", enabled[i]);
      } catch (const std::exception& error) { g_warning("Web app extension %s: %s", enabled[i], error.what()); }
    }
  }
  void load_webapp_extension(size_t index, std::function<void()> done) {
    if (closing) return;
    if (index == webapp_extensions.size()) { done(); return; }
    const auto [id, path] = webapp_extensions[index];
    command("Extensions.loadUnpacked", "{\"path\":" + quote(path) + "}", "",
      [this, index, id, done = std::move(done)](JsonObject* result, const std::string& error) mutable {
        if (closing) return;
        if (!result || std::string(text(result, "id")) != id) {
          g_warning("Could not load web app extension %s: %s", id.c_str(), error.c_str());
          load_webapp_extension(index + 1, std::move(done)); return;
        }
        command("Runtime.evaluate", "{\"expression\":" + quote("chrome.management.setEnabled(" + quote(id) + ",true)") + ",\"awaitPromise\":true}", integration_session,
          [this, index, done = std::move(done)](JsonObject*, const std::string& error) mutable {
            if (!error.empty()) g_warning("Could not enable web app extension: %s", error.c_str());
            load_webapp_extension(index + 1, std::move(done));
          });
      });
  }
  void apply_webapp_extensions(std::function<void()> done) {
    if (!webapp_configured) { done(); return; }
    std::string allowed = "[" + quote(native_integration_id);
    if (webapp_keepassxc && adapter_enabled("keepassxc")) allowed += ",\"giohadoodmdcadnkcpdmhmmdnaidnckm\"";
    if (webapp_ublock && adapter_enabled("ublock")) {
      try { allowed += "," + quote(BundledExtensionUpdate::packaged_id(profile, GNOSIS_UBLOCK_DIR)); }
      catch (const std::exception& error) { g_warning("Web app uBlock identity: %s", error.what()); }
    }
    for (const auto& [id, path] : webapp_extensions) allowed += "," + quote(id);
    allowed += ']';
    const std::string expression = "(async()=>{const allowed=new Set(" + allowed + ");"
      "for(const item of await chrome.management.getAll())"
      "if(item.type==='extension'&&item.installType!=='other'&&item.mayDisable&&item.enabled&&!allowed.has(item.id))"
      "await chrome.management.setEnabled(item.id,false);return true})()";
    command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"awaitPromise\":true}", integration_session,
      [this, done = std::move(done)](JsonObject* result, const std::string& error) mutable {
        if (closing) return;
        if (!result || !error.empty() || object(result, "exceptionDetails")) {
          integration_failed("Could not apply this web app's extension selection"); return;
        }
        load_webapp_extension(0, std::move(done));
      });
  }
