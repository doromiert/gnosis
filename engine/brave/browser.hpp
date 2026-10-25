/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <functional>
#include <memory>
#include <string>
namespace JupiterBrave {
bool enabled();
bool composited();
void initialize(const std::string& profile);
void shutdown();
std::string quote(const std::string&);
using Reply = std::function<void(JsonObject*, const std::string&)>;
using ResourceReply = std::function<void(GBytes*, const std::string&, const std::string&)>;
using DownloadProgress = std::function<void(guint64, guint64, const std::string&)>;
using DownloadBegin = std::function<DownloadProgress(const std::string&, const std::string&,
  const std::string&, std::function<void()>)>;
class Process;
class Page : public std::enable_shared_from_this<Page> {
public:
  static std::shared_ptr<Page> create(GtkWidget* content, bool ephemeral);
  ~Page();
  std::string uri = "about:blank", title, target, session;
  bool loading = false, can_back = false, can_forward = false;
  double progress = 0;
  std::function<void()> changed, closed, tls_changed;
  std::function<void(bool)> fullscreen_changed;
  std::function<void(double)> zoom_changed;
  std::function<void(const std::string&)> failed;
  std::function<void(const std::string&)> navigation_started;
  std::function<bool(const std::string&)> open_uri;
  std::function<void(int)> load_finished;
  std::function<void(const std::string&, JsonObject*)> event;
  std::function<void(const std::string&, bool, ResourceReply)> internal_resource;
  std::function<void(const std::string&, const std::string&, const std::string&)> page_message;
  DownloadBegin download_begin;
  std::string canonical_uri(const std::string&) const;
  bool is_internal_page(const std::string&) const;
  bool send_page_message(const std::string&, const std::string&, const std::string&);
  void start();
  void navigate(const std::string&);
  void command(const std::string&, const std::string& = "{}", Reply = {});
  void back(); void forward(); void reload(bool bypass = false); void stop();
  void focus(); void set_visible(bool); void close();
  void send_key(GdkEvent*);
  bool edit(const std::string&);
  void set_zoom(double);
  GtkWidget* widget() const;
  bool is_closed() const;
  bool tls_info(GTlsCertificate**, GTlsCertificateFlags*) const;
  bool tls_secure() const;
  void snapshot(std::function<void(GdkTexture*, const std::string&)>, bool full_document = false);
private:
  friend class Process;
  struct State;
  std::unique_ptr<State> state_;
  explicit Page(GtkWidget*, bool);
  void attach(); void handle(const std::string&, JsonObject*); void layout();
  void adopt_surface(GtkWidget*); void surface_ready();
  void history(int); void refresh_history();
  void internal_request(JsonObject*);
  void show_context_menu(JsonObject*, int context);
  void dismiss_context_menu();
  void apply_zoom();
  void receive_zoom(double);
};
}
