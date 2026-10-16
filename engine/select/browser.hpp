#pragma once
#include "include/cef_client.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_frame_handler.h"
#include "include/cef_permission_handler.h"
#include "ephy-engine-permission.h"
#include "include/cef_render_handler.h"
#include <adwaita.h>
#include <atomic>
#include <epoxy/gl.h>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

class Window;
class ExtensionHost;

class Browser final : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRenderHandler,
                      public CefDisplayHandler,
                      public CefLoadHandler,
                      public CefRequestHandler,
                      public CefResourceRequestHandler,
                      public CefDownloadHandler,
                      public CefContextMenuHandler,
                      public CefJSDialogHandler,
                      public CefPermissionHandler,
                      public CefFrameHandler,
                      public CefDialogHandler {
public:
  explicit Browser(std::string url, bool software = false);
  GtkWidget* widget() const { return overlay_; }
  GtkWidget* surface() const { return area_; }
  std::string url;
  std::string name;
  std::string group_id;
  int native_tab_id = -1;
  GdkPaintable* thumbnail();
  bool loading = false;
  double progress = 0;
  bool closing = false;
  bool loaded = false;
  bool defer_start = false;
  bool gpu_frames = false;
  bool custom_cursor_seen = false;
  bool is_extension_popup = false;
  bool is_auth_popup = false;
  bool is_home = false;
  void start();
  void navigate(const std::string& address);
  void reload();
  void back();
  void forward();
  void close();
  void shutdown();
  void set_visible(bool visible);
  void refresh_theme();
  void set_permissions();
  void set_user_agent_override(const char *agent) {
    request_user_agent_.store(agent ? std::make_shared<const std::string>(agent) : nullptr);
  }
  void detach_callbacks();
  void cancel_permissions();
  void activate_clipboard() {
    gtk_widget_grab_focus(area_);
    clipboard_until_ = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    if (!closing && browser_ && clipboard_allowed()) browser_->GetHost()->SetFocus(true);
  }
  bool clipboard_focused() const {
    auto* root = gtk_widget_get_root(area_);
    return root && GTK_IS_WINDOW(root) && gtk_window_is_active(GTK_WINDOW(root)) &&
           gtk_widget_has_focus(area_);
  }
  bool clipboard_allowed() const {
    return clipboard_focused() && g_get_monotonic_time() < clipboard_until_;
  }
  bool can_back() const { return browser_ && browser_->CanGoBack(); }
  bool can_forward() const { return browser_ && browser_->CanGoForward(); }
  CefRefPtr<CefBrowser> cef_browser() const { return browser_; }
  Window* owner() const;
  Window* host = nullptr;
  std::function<void(int)> on_load_end;
  std::function<bool(const std::string&, const std::string&)> on_load_error;
  std::function<void(const std::string&)> on_load_start;
  std::function<void()> prepare_navigation;
  std::function<void(const std::string&, CefRefPtr<CefResponse>)> on_response;
  std::function<void()> on_changed;
  std::function<void()> on_closed;
  std::function<void(const std::string&, const std::string&, GdkTexture*)> on_favicon;
  std::function<void()> on_close_cancelled;
  std::function<bool(EphyEnginePermission*)> on_permission;
  std::function<void(const std::string&, const std::string&, const std::string&)> on_page_message;
  std::string page_message_context;
  static unsigned active_count;

  CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
  CefRefPtr<CefFrameHandler> GetFrameHandler() override { return this; }
  CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>) override { return cookie_filter_; }
  bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&, const CefString&,
                    const std::vector<CefString>&, const std::vector<CefString>&,
                    const std::vector<CefString>&, CefRefPtr<CefFileDialogCallback>) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefProcessId,
                                CefRefPtr<CefProcessMessage>) override;
  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t prompt_id, const CefString& origin,
                              uint32_t permissions, CefRefPtr<CefPermissionPromptCallback> callback) override;
  void OnDismissPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, cef_permission_request_result_t) override;
  void OnFrameDetached(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) override;
  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&,
                                      uint32_t, CefRefPtr<CefMediaAccessCallback>) override;
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override;
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const void* buffer, int width,
               int height) override;
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                          const CefAcceleratedPaintInfo& info) override;
  void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
  void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_rect_ = rect; }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser>) override;
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString& target_url,
                     const CefString&, cef_window_open_disposition_t, bool user_gesture,
                     const CefPopupFeatures&, CefWindowInfo& window_info, CefRefPtr<CefClient>& client,
                     CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override;
  void OnFaviconURLChange(CefRefPtr<CefBrowser>, const std::vector<CefString>&) override;
  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override;
  void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& address) override;
  void OnLoadingProgressChange(CefRefPtr<CefBrowser>, double) override;
  void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool is_loading, bool, bool) override;
  void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, TransitionType) override;
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int status) override;
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode error, const CefString& text,
                   const CefString& failed_url) override;
  void OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) override;
  bool OnTooltip(CefRefPtr<CefBrowser>, CefString& text) override;
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message, const CefString&,
                        int) override;
  bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type,
                      const CefCursorInfo&) override;
  bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
                      bool user_gesture, bool is_redirect) override;
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
    CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) override;
  bool OnResourceResponse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
                          CefRefPtr<CefResponse>) override;
  cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
                                          CefRefPtr<CefCallback>) override;
  bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& target_url,
                        cef_window_open_disposition_t, bool user_gesture) override;
  bool OnCertificateError(CefRefPtr<CefBrowser>, cef_errorcode_t, const CefString&,
                          CefRefPtr<CefSSLInfo>, CefRefPtr<CefCallback>) override;
  bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override;
  void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                         CefRefPtr<CefDownloadItemCallback>) override;
  void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
                           CefRefPtr<CefMenuModel> model) override;
  bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params,
                      CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback) override;
  bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType type, const CefString& message,
                  const CefString& default_prompt, CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress) override;
  bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool,
                            CefRefPtr<CefJSDialogCallback> callback) override;

private:
  std::atomic<uint64_t> main_request_{0};
  bool navigation_started_ = false;
  CefRefPtr<CefCookieAccessFilter> cookie_filter_;
  std::atomic<std::shared_ptr<const std::string>> request_user_agent_;
  ~Browser() override;
  CefRefPtr<CefBrowser> browser_;
  GApplication* held_app_ = nullptr;
  CefRefPtr<ExtensionHost> attached_host_;
  GtkWidget* overlay_;
  GtkWidget* area_;
  GtkWidget* error_page_;
  GtkIMContext* im_;
  bool software_;
  bool creating_ = false;
  bool waiting_context_ = false;
  std::set<uint32_t> downloads_;
  std::vector<GCancellable*> file_dialogs_;
  std::map<uint64_t, std::shared_ptr<EphyEnginePermission>> permission_prompts_;
  struct MediaRequest { std::string frame; std::shared_ptr<EphyEnginePermission> request; };
  std::vector<MediaRequest> media_requests_;
  void cancel_media_permissions(const std::string& frame);
  void present_permission(EphyEnginePermission *);
  void cancel_file_dialogs();
  int browser_id_ = 0;
  std::string pending_navigation_;
  bool owns(CefRefPtr<CefBrowser> source) const;
  bool popup_visible_ = false;
  bool auth_has_document_ = false;
  bool auth_return_pending_ = false;
  CefRect popup_rect_;
  guint thumbnail_idle_ = 0;
  GdkPaintable* thumbnail_source_ = nullptr;
  GdkPaintable* thumbnail_ = nullptr;
  GLuint texture_ = 0, popup_texture_ = 0, program_ = 0, vao_ = 0;
  int texture_width_ = 0, texture_height_ = 0;
  bool texture_bgra_ = false, popup_bgra_ = false;
  GBytes* account_pixels_ = nullptr;
  int account_width_ = 0, account_height_ = 0;
  gint64 clipboard_until_ = 0;
  double mouse_x_ = 0, mouse_y_ = 0;
  int drag_button_ = 0;
  void setup_input();
  void changed();
  bool gl_ready();
  void init_gl();
  void clear_gl();
  void render();
  void upload(PaintElementType type, const void* pixels, int width, int height);
  void show_error(const std::string& title, const std::string& description);
  CefMouseEvent mouse(GdkModifierType modifiers = static_cast<GdkModifierType>(0));
  void send_key(guint keyval, guint keycode, GdkModifierType state, bool release);
  bool route(const std::string& target, bool user_gesture, bool new_tab);
  IMPLEMENT_REFCOUNTING(Browser);
};
