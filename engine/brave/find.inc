/* SPDX-License-Identifier: GPL-3.0-or-later */
void Page::find(const std::string& query, bool forward, bool sensitive, bool match, Reply callback) {
  if (!adapter_enabled("find")) { callback(nullptr, _("Native find is disabled in Debugging settings")); return; }
  if (is_closed() || !state_->attached || !state_->process || target.empty()) {
    callback(nullptr, _("The search page is not available")); return;
  }
  if (query.size() > 8192 || !g_utf8_validate(query.data(), query.size(), nullptr)) {
    callback(nullptr, _("Search text is invalid or too long")); return;
  }
  const auto generation = ++state_->find_generation;
  const auto navigation = state_->navigation_generation;
  const auto owner = target;
  state_->process->find_request(owner, query, forward, sensitive, match,
    [weak = weak_from_this(), generation, navigation, owner, match, callback = std::move(callback)](JsonObject* result, const std::string& error) {
      auto page = weak.lock();
      if (!page || page->is_closed() || page->target != owner ||
          page->state_->navigation_generation != navigation || page->state_->find_generation != generation) {
        callback(nullptr, _("Search was canceled")); return;
      }
      if (!error.empty() || !result) { callback(nullptr, error.empty() ? _("Native find is unavailable") : error); return; }
      auto* count = json_object_get_member(result, "count");
      auto* ordinal = json_object_get_member(result, "ordinal");
      if (std::string(text(result, "target")) != owner || !count || !ordinal ||
          json_node_get_value_type(count) != G_TYPE_INT64 || json_node_get_value_type(ordinal) != G_TYPE_INT64 ||
          json_node_get_int(count) < 0 || json_node_get_int(count) > G_MAXINT ||
          json_node_get_int(ordinal) < 0 || json_node_get_int(ordinal) > json_node_get_int(count)) {
        callback(nullptr, _("Invalid native search result")); return;
      }
      if (vim_mode && match && json_node_get_int(ordinal) > 0) {
        // Retain Chromium's real match rather than running a second JS text
        // search (which disagrees on hidden text, shadow roots and frames).
        // StopFinding keeps that match selected; the isolated adapters adopt
        // its enclosing control after the renderer's selection changes.
        page->state_->process->find_request(owner, "", true, false, false, {}, true, true);
        for (const auto& [context, unique] : page->state_->metadata_unique) {
          auto source = page->state_->menu_sessions.find(context);
          if (source == page->state_->menu_sessions.end()) continue;
          page->state_->process->command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(unique) +
            ",\"expression\":" + quote("globalThis.gnosisVim?.adoptSearch()") + "}", source->second);
        }
      }
      callback(result, "");
    });
}
void Page::stop_find() {
  if (!adapter_enabled("find")) return;
  ++state_->find_generation;
  if (state_->process && !target.empty())
    state_->process->find_request(target, "", true, false, false, {}, true, vim_mode);
}
