/* SPDX-License-Identifier: GPL-3.0-or-later; included inside Process. */
  void refresh_bundled_adblock(JsonArray* inventory, bool blocking, std::function<void()> finish) {
    std::string expected;
    try { expected = BundledExtensionUpdate::packaged_id(profile, JUPITER_UBLOCK_DIR); }
    catch (const std::exception& error) { g_warning("Bundled uBlock: %s", error.what()); finish(); return; }
    std::string installed;
    bool registered = false, external = false;
    for (guint i = 0; inventory && i < json_array_get_length(inventory); ++i) {
      auto* item = json_array_get_object_element(inventory, i);
      if (std::string(text(item, "id")) == expected) {
        if (std::string(text(item, "installType")) != "development") { finish(); return; }
        registered = true; installed = text(item, "version");
      } else if (std::string(text(item, "name")) == "uBlock Origin") external = true;
    }
    if (!registered && (!blocking || external)) { finish(); return; }
    struct Work {
      std::shared_ptr<Process> process;
      std::shared_ptr<PrivateProfile> storage;
      std::string installed, error;
      bool blocking;
      std::function<void()> finish;
      std::shared_ptr<BundledExtensionUpdate> update;
    };
    ++profile_writers;
    auto* work = new Work{shared_from_this(), private_storage, installed, "", blocking, std::move(finish), {}};
    auto* task = g_task_new(nullptr, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
      auto work = std::unique_ptr<Work>(static_cast<Work*>(data));
      g_task_propagate_boolean(G_TASK(result), nullptr);
      auto self = work->process;
      --self->profile_writers;
      if (self->closing) self->retire_private_profile();
      auto finish = std::move(work->finish);
      if (self->closing || !work->error.empty() || !work->update) {
        if (!work->error.empty()) g_warning("Bundled uBlock update: %s", work->error.c_str());
        finish(); return;
      }
      auto update = std::move(work->update);
      const bool blocking = work->blocking;
      auto recover = [self, update, finish, previous = work->installed](const std::string& reason) {
        g_warning("Bundled uBlock reload: %s", reason.c_str());
        try { update->rollback(); }
        catch (const std::exception& error) { g_warning("Bundled uBlock rollback: %s", error.what()); finish(); return; }
        if (self->closing || previous.empty()) { finish(); return; }
        self->command("Extensions.loadUnpacked", "{\"path\":" + quote(update->directory()) + "}", "",
          [finish](JsonObject*, const std::string& error) {
            if (!error.empty()) g_warning("Restoring bundled uBlock: %s", error.c_str());
            finish();
          });
      };
      self->command("Extensions.loadUnpacked", "{\"path\":" + quote(update->directory()) + "}", "",
        [self, update, blocking, finish, recover](JsonObject* result, const std::string& error) {
          if (self->closing || !error.empty() || std::string(text(result, "id")) != update->id()) {
            recover(error.empty() ? "The native extension identity changed" : error); return;
          }
          const auto expression = "(async()=>{const id=" + quote(update->id()) + ";let value=await chrome.management.get(id);"
            "if(value.enabled!==" + (blocking ? std::string("true") : std::string("false")) + ")await chrome.management.setEnabled(id," +
            (blocking ? "true" : "false") + ");value=await chrome.management.get(id);return {id:value.id,version:value.version,enabled:value.enabled}})()";
          self->command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"awaitPromise\":true,\"returnByValue\":true}", self->integration_session,
            [update, blocking, finish, recover](JsonObject* result, const std::string& error) {
              auto* value = object(object(result, "result"), "value");
              if (!error.empty() || !value || std::string(text(value, "id")) != update->id() ||
                  std::string(text(value, "version")) != update->version() ||
                  json_object_get_boolean_member_with_default(value, "enabled", !blocking) != blocking) {
                recover(error.empty() ? "The native extension did not retain its version and enabled state" : error); return;
              }
              try { update->commit(); }
              catch (const std::exception& error) { g_warning("Bundled uBlock completion: %s", error.what()); }
              finish();
            });
        });
    }, work);
    g_task_set_task_data(task, work, nullptr);
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
      auto* work = static_cast<Work*>(data);
      try { work->update = BundledExtensionUpdate::prepare(work->process->profile, JUPITER_UBLOCK_DIR, work->installed); }
      catch (const std::exception& error) { work->error = error.what(); }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }
