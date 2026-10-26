/* SPDX-License-Identifier: GPL-3.0-or-later
 * Included inside Process. Only host-owned Page targets reach this channel. */
  void find_request(const std::string& target, const std::string& query,
                    bool forward, bool sensitive, bool match, Reply callback, bool stop = false, bool keep = false) {
    if (native_fd < 0) { if (callback) callback(nullptr, _("Native find is unavailable")); return; }
    const auto id = ++native_sequence;
    const auto message = std::string("{\"kind\":") + quote(stop ? "find-stop" : "find") +
      ",\"id\":" + std::to_string(id) + ",\"target\":" + quote(target) +
      ",\"text\":" + quote(query) + ",\"forward\":" + (forward ? "true" : "false") +
      ",\"caseSensitive\":" + (sensitive ? "true" : "false") +
      ",\"findMatch\":" + (match ? "true" : "false") +
      ",\"keepSelection\":" + (keep ? "true" : "false") + "}";
    if (message.size() > 60000) { if (callback) callback(nullptr, _("Search text is too long")); return; }
    if (callback) {
      struct Timeout { std::weak_ptr<Process> process; gint64 id; };
      auto* timeout = new Timeout{weak_from_this(), id};
      guint source = g_timeout_add_full(G_PRIORITY_DEFAULT, 5000, +[](gpointer data) -> gboolean {
        auto* timeout = static_cast<Timeout*>(data);
        if (auto process = timeout->process.lock()) {
          auto found = process->native_replies.find(timeout->id);
          if (found != process->native_replies.end()) {
            auto callback = std::move(found->second.callback); process->native_replies.erase(found);
            callback(nullptr, _("Native find did not respond"));
          }
        }
        return G_SOURCE_REMOVE;
      }, timeout, +[](gpointer data) { delete static_cast<Timeout*>(data); });
      native_replies.emplace(id, NativeReply{std::move(callback), source});
    }
    if (send(native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
      close_native_ui();
  }
