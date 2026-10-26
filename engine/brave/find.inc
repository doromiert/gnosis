/* SPDX-License-Identifier: GPL-3.0-or-later */
void Page::find(const std::string& query, bool forward, bool sensitive, bool match, Reply callback) {
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
    [weak = weak_from_this(), generation, navigation, owner, callback = std::move(callback)](JsonObject* result, const std::string& error) {
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
      callback(result, "");
    });
}
void Page::stop_find() {
  ++state_->find_generation;
  if (state_->process && !target.empty())
    state_->process->find_request(target, "", true, false, false, {}, true);
}
