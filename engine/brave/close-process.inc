/* SPDX-License-Identifier: GPL-3.0-or-later; included inside Process. */
  gint64 check_beforeunload(const std::string& target, Reply callback) {
    if (native_fd < 0 || closing) { callback(nullptr, _("The page close service is unavailable")); return 0; }
    const auto id = ++native_sequence;
    native_replies.emplace(id, NativeReply{std::move(callback), 0});
    const std::string message = "{\"kind\":\"beforeunload-check\",\"id\":" + std::to_string(id) + ",\"target\":" + quote(target) + "}";
    if (send(native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size())) { close_native_ui(); return 0; }
    return id;
  }
  void cancel_beforeunload(const std::string& target, gint64 id) {
    if (id <= 0) return;
    auto found = native_replies.find(id);
    if (found != native_replies.end()) {
      if (found->second.timeout) g_source_remove(found->second.timeout);
      native_replies.erase(found);
    }
    if (native_fd < 0 || closing) return;
    const std::string message = "{\"kind\":\"beforeunload-cancel\",\"id\":" + std::to_string(id) + ",\"target\":" + quote(target) + "}";
    if (send(native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size())) close_native_ui();
  }
