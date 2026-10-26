/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-session.h"
#include "ephy-engine-scheme.h"
#include "ephy-engine-settings.h"
#include "brave/browser.hpp"
#include "ephy-engine-origin.h"
#include "ephy-engine-certificate.hpp"
#include "include/cef_cookie.h"
#include "include/cef_browser.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr const char *permission_migration_pref = "jupiter.permission_policy_version";
constexpr const char *upload_folder_pref = "jupiter.last_upload_folder";
using PermissionKey = std::pair<EphyEnginePermissionKind, std::pair<std::string, std::string>>;
std::optional<cef_content_setting_types_t> permission_type(EphyEnginePermissionKind kind) {
  switch (kind) {
    case EPHY_ENGINE_PERMISSION_NOTIFICATIONS: return CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS;
    case EPHY_ENGINE_PERMISSION_LOCATION: return CEF_CONTENT_SETTING_TYPE_GEOLOCATION;
    case EPHY_ENGINE_PERMISSION_MICROPHONE: return CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC;
    case EPHY_ENGINE_PERMISSION_CAMERA: return CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA;
    default: return {};
  }
}
cef_content_setting_values_t permission_value(EphyEnginePermissionState state) {
  return state == EPHY_ENGINE_PERMISSION_STATE_ALLOW ? CEF_CONTENT_SETTING_VALUE_ALLOW :
    state == EPHY_ENGINE_PERMISSION_STATE_DENY ? CEF_CONTENT_SETTING_VALUE_BLOCK : CEF_CONTENT_SETTING_VALUE_DEFAULT;
}
std::optional<PermissionKey> permission_key(EphyEnginePermissionKind kind, const char *uri, const char *top_uri) {
  if (!permission_type(kind)) return {};
  g_autoptr(EphyEngineOrigin) origin = ephy_engine_origin_new_for_uri(uri);
  if (!origin) return {};
  g_autoptr(EphyEngineOrigin) top = top_uri ? ephy_engine_origin_new_for_uri(top_uri) : nullptr;
  if (top_uri && !top) return {};
  g_autofree char *canonical = ephy_engine_origin_to_string(origin);
  g_autofree char *canonical_top = top ? ephy_engine_origin_to_string(top) : nullptr;
  return PermissionKey{kind, {canonical, canonical_top ? canonical_top : ""}};
}
struct Session {
  bool ephemeral = false, ready = false, storage = true, storage_overridden = false;
  std::atomic<bool> alive{true};
  std::atomic<bool> storage_allowed{true};
  CefRefPtr<CefRequestContext> context;
  std::set<std::pair<std::string, std::string>> certificates;
  std::mutex tls_hosts_mutex;
  std::set<std::string> tls_hosts;
  std::map<PermissionKey, EphyEnginePermissionState> pending_permissions, legacy_permissions;
  bool import_permissions = false;
  std::optional<std::string> upload_folder;
  bool upload_folder_dirty = false;
  bool apply_upload_folder() {
    if (ephemeral || !upload_folder_dirty || !ready || !context) return true;
    auto value = CefValue::Create(); value->SetString(upload_folder.value_or(""));
    CefString error;
    if (!context->SetPreference(upload_folder_pref, value, error)) {
      g_warning("Could not remember Chromium upload folder: %s", error.ToString().c_str());
      return false;
    }
    upload_folder_dirty = false;
    return true;
  }

  void apply_permission(const PermissionKey& key, EphyEnginePermissionState state) {
    context->SetContentSetting(key.second.first, key.second.second, *permission_type(key.first), permission_value(state));
  }
  void apply_permissions() {
    if (!ready || !context) return;
    if (import_permissions && !ephemeral) {
      auto migrated = context->GetPreference(permission_migration_pref);
      if (!migrated || migrated->GetType() != VTYPE_INT) {
        g_warning("Chromium permission migration preference is unavailable");
        return;
      }
      if (migrated->GetInt() == 0) {
        for (const auto& [key, state] : legacy_permissions) apply_permission(key, state);
        auto version = CefValue::Create(); version->SetInt(1); CefString error;
        if (!context->SetPreference(permission_migration_pref, version, error)) {
          g_warning("Could not record Chromium permission migration: %s", error.ToString().c_str());
          return;
        }
      }
      legacy_permissions.clear(); import_permissions = false;
    }
    for (const auto& [key, state] : pending_permissions) apply_permission(key, state);
    pending_permissions.clear();
  }
  void apply() {
    bool allowed = storage_overridden ? storage : ephy_engine_settings_get_storage_enabled();
    storage_allowed.store(allowed);
    if (!ready || !context) return;
    apply_permissions();
    apply_upload_folder();
    ephy_engine_settings_apply_context(context);
    auto value = CefValue::Create();
    value->SetInt(allowed ? CEF_CONTENT_SETTING_VALUE_ALLOW : CEF_CONTENT_SETTING_VALUE_BLOCK);
    CefString error;
    if (!context->SetPreference("profile.default_content_setting_values.cookies", value, error))
      g_warning("Could not apply Chromium storage policy: %s", error.ToString().c_str());
  }
};
class CookieFilter final : public CefCookieAccessFilter {
public:
  explicit CookieFilter(std::shared_ptr<Session> session) : session_(std::move(session)) {}
  bool CanSendCookie(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, const CefCookie&) override {
    return session_->storage_allowed.load();
  }
  bool CanSaveCookie(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, CefRefPtr<CefResponse>, const CefCookie&) override {
    return session_->storage_allowed.load();
  }
private:
  std::shared_ptr<Session> session_;
  IMPLEMENT_REFCOUNTING(CookieFilter);
};
class TlsPreparation final : public CefTask {
public:
  TlsPreparation(std::shared_ptr<Session> session, std::string uri, std::string host,
                 CefRefPtr<CefBrowser> browser, CefRefPtr<CefCallback> reply)
    : session_(std::move(session)), uri_(std::move(uri)), host_(std::move(host)), browser_(browser), reply_(reply) {}
  void Execute() override {
    if (!session_->alive || !session_->ready || !session_->context || (browser_ && !browser_->IsValid())) {
      reply_->Cancel(); return;
    }
    bool prepared;
    { std::lock_guard lock(session_->tls_hosts_mutex); prepared = session_->tls_hosts.contains(host_); }
    if (!prepared) {
      if (!ephy_engine_certificate_reset_cached_decisions(session_->context, uri_, true)) {
        reply_->Cancel();
        return;
      }
      // Publish only after the native setting exists; overlapping requests wait on UI.
      std::lock_guard lock(session_->tls_hosts_mutex);
      session_->tls_hosts.insert(host_);
    }
    reply_->Continue();
  }
private:
  std::shared_ptr<Session> session_;
  std::string uri_, host_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefCallback> reply_;
  IMPLEMENT_REFCOUNTING(TlsPreparation);
};
class SessionRequestHandler final : public CefResourceRequestHandler {
public:
  explicit SessionRequestHandler(std::shared_ptr<Session> session) : session_(std::move(session)) {}
  cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
      CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> reply) override {
    if (!session_->alive) return RV_CANCEL;
    if (!session_->ephemeral) return RV_CONTINUE;
    CefURLParts parts;
    auto uri = request->GetURL();
    if (!CefParseURL(uri, parts) || CefString(&parts.scheme) != "https") return RV_CONTINUE;
    auto host = CefString(&parts.host).ToString();
    if (host.empty()) return RV_CONTINUE;
    { std::lock_guard lock(session_->tls_hosts_mutex); if (session_->tls_hosts.contains(host)) return RV_CONTINUE; }
    if (!CefPostTask(TID_UI, new TlsPreparation(session_, uri.ToString(), host, browser, reply))) return RV_CANCEL;
    return RV_CONTINUE_ASYNC;
  }
private:
  std::shared_ptr<Session> session_;
  IMPLEMENT_REFCOUNTING(SessionRequestHandler);
};
std::vector<EphyEngineSession*> sessions;
bool initialized = false;
class ContextHandler final : public CefRequestContextHandler {
public:
  explicit ContextHandler(std::weak_ptr<Session> session) : session_(session) {}
  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override {
    if (auto session = session_.lock()) {
      session->context = context;
      session->alive = true;
      session->ready = true;
      session->apply();
      ephy_engine_install_scheme_handlers(context);
    }
  }
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
      CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) override {
    auto session = session_.lock();
    return session && session->ephemeral ? new SessionRequestHandler(std::move(session)) : nullptr;
  }
private:
  std::weak_ptr<Session> session_;
  IMPLEMENT_REFCOUNTING(ContextHandler);
};
}
struct _EphyEngineSession { GObject parent; std::shared_ptr<Session> *data; };
G_DEFINE_TYPE(EphyEngineSession, ephy_engine_session, G_TYPE_OBJECT)
static void ephy_engine_session_class_init(EphyEngineSessionClass *klass) {
  g_signal_new("extensions-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("download-started", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               nullptr, nullptr, nullptr, G_TYPE_NONE, 1, EPHY_TYPE_ENGINE_DOWNLOAD);
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *session = EPHY_ENGINE_SESSION(object);
    std::erase(sessions, session);
    (*session->data)->alive = false;
    delete session->data;
    G_OBJECT_CLASS(ephy_engine_session_parent_class)->finalize(object);
  };
}
static void ephy_engine_session_init(EphyEngineSession *session) {
  session->data = new std::shared_ptr<Session>(std::make_shared<Session>());
  sessions.push_back(session);
}
EphyEngineSession *ephy_engine_session_new(gboolean ephemeral) {
  auto *session = EPHY_ENGINE_SESSION(g_object_new(EPHY_TYPE_ENGINE_SESSION, nullptr));
  (*session->data)->ephemeral = ephemeral;
  return session;
}
EphyEngineSession *ephy_engine_session_get_default() {
  static auto *session = ephy_engine_session_new(false);
  return session;
}
gboolean ephy_engine_session_is_ephemeral(EphyEngineSession *session) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), true);
  return (*session->data)->ephemeral;
}
GFile *ephy_engine_session_dup_last_upload_folder(EphyEngineSession *session) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), nullptr);
  auto data = *session->data;
  // Never consult a Chromium preference, even an in-memory one, for private
  // sessions. Their last directory belongs only to this exact session object.
  if (!data->ephemeral && !data->upload_folder_dirty) {
    auto context = ephy_engine_session_context(session);
    if (!context || !data->ready) return nullptr;
    auto value = context->GetPreference(upload_folder_pref);
    if (!value || value->GetType() != VTYPE_STRING) return nullptr;
    data->upload_folder = value->GetString().ToString();
  }
  const auto &path = data->upload_folder;
  if (!path || path->empty() || path->find('\0') != std::string::npos ||
      !g_utf8_validate(path->c_str(), -1, nullptr) ||
      !g_path_is_absolute(path->c_str()) || !g_file_test(path->c_str(), G_FILE_TEST_IS_DIR)) return nullptr;
  return g_file_new_for_path(path->c_str());
}
gboolean ephy_engine_session_set_last_upload_folder(EphyEngineSession *session, GFile *folder) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), false);
  g_return_val_if_fail(G_IS_FILE(folder), false);
  g_autofree char *path = g_file_get_path(folder);
  if (!path || !g_utf8_validate(path, -1, nullptr) || !g_path_is_absolute(path)) return false;
  auto data = *session->data;
  data->upload_folder = path;
  if (data->ephemeral) return true;
  data->upload_folder_dirty = true;
  ephy_engine_session_context(session);
  return data->apply_upload_folder();
}
CefRefPtr<CefCookieAccessFilter> ephy_engine_session_cookie_filter(EphyEngineSession *session) {
  auto data = *session->data;
  data->apply();
  // CEF preloads cookies outside the network service when a request handler is installed.
  return new CookieFilter(std::move(data));
}
CefRefPtr<CefResourceRequestHandler> ephy_engine_session_request_handler(EphyEngineSession *session) {
  auto data = *session->data;
  return data->ephemeral ? new SessionRequestHandler(std::move(data)) : nullptr;
}
CefRefPtr<CefRequestContext> ephy_engine_session_context(EphyEngineSession *session) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), nullptr);
  if (!initialized) return nullptr;
  auto data = *session->data;
  if (!data->context) {
    if (data->ephemeral) {
      CefRequestContextSettings settings;
      data->context = CefRequestContext::CreateContext(settings, new ContextHandler(data));
    } else {
      data->context = CefRequestContext::GetGlobalContext();
      data->alive = true;
      data->ready = true;
      data->apply();
    }
  }
  return data->context;
}
void ephy_engine_sessions_initialize() {
  initialized = true;
  for (auto *session : sessions) ephy_engine_session_context(session);
}
void ephy_engine_sessions_register_preferences(cef_preferences_type_t type, CefRawPtr<CefPreferenceRegistrar> registrar) {
  if (type != CEF_PREFERENCES_TYPE_REQUEST_CONTEXT) return;
  auto value = CefValue::Create(); value->SetInt(0);
  registrar->AddPreference(permission_migration_pref, value);
  auto folder = CefValue::Create(); folder->SetString("");
  registrar->AddPreference(upload_folder_pref, folder);
}
void ephy_engine_sessions_preferences_changed() {
  if (initialized) for (auto *session : sessions) (*session->data)->apply();
}
void ephy_engine_sessions_shutdown() {
  initialized = false;
  for (auto *session : sessions) {
    (*session->data)->alive = false;
    (*session->data)->ready = false;
    (*session->data)->context = nullptr;
    (*session->data)->certificates.clear();
    { std::lock_guard lock((*session->data)->tls_hosts_mutex); (*session->data)->tls_hosts.clear(); }
    (*session->data)->pending_permissions.clear();
    (*session->data)->legacy_permissions.clear();
    (*session->data)->import_permissions = false;
    (*session->data)->upload_folder.reset();
    (*session->data)->upload_folder_dirty = false;
  }
}
void ephy_engine_session_set_storage_enabled(EphyEngineSession *session, gboolean enabled) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session));
  (*session->data)->storage = enabled;
  (*session->data)->storage_overridden = true;
  (*session->data)->apply();
}
gboolean ephy_engine_session_set_permission(EphyEngineSession *session, EphyEnginePermissionKind kind,
  const char *uri, const char *top_uri, EphyEnginePermissionState state) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), false);
  if (state != EPHY_ENGINE_PERMISSION_STATE_ASK && state != EPHY_ENGINE_PERMISSION_STATE_ALLOW && state != EPHY_ENGINE_PERMISSION_STATE_DENY) return false;
  auto key = permission_key(kind, uri, top_uri);
  if (!key) return false;
  auto data = *session->data;
  data->pending_permissions[*key] = state;
  ephy_engine_session_context(session);
  data->apply_permissions();
  return true;
}
EphyEnginePermissionState ephy_engine_session_get_permission(EphyEngineSession *session, EphyEnginePermissionKind kind,
  const char *uri, const char *top_uri) {
  g_return_val_if_fail(EPHY_IS_ENGINE_SESSION(session), EPHY_ENGINE_PERMISSION_STATE_ASK);
  auto key = permission_key(kind, uri, top_uri ? top_uri : uri);
  if (!key) return EPHY_ENGINE_PERMISSION_STATE_ASK;
  auto context = ephy_engine_session_context(session);
  auto data = *session->data;
  if (!context || !data->ready) return EPHY_ENGINE_PERMISSION_STATE_ASK;
  auto value = context->GetContentSetting(key->second.first, key->second.second, *permission_type(kind));
  return value == CEF_CONTENT_SETTING_VALUE_ALLOW ? EPHY_ENGINE_PERMISSION_STATE_ALLOW :
    value == CEF_CONTENT_SETTING_VALUE_BLOCK ? EPHY_ENGINE_PERMISSION_STATE_DENY : EPHY_ENGINE_PERMISSION_STATE_ASK;
}
void ephy_engine_session_import_permissions(EphyEngineSession *session, const EphyEnginePermissionRecord *records, gsize count) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session));
  g_return_if_fail(records || !count);
  auto data = *session->data;
  if (data->ephemeral) return;
  data->legacy_permissions.clear();
  for (gsize i = 0; i < count; ++i) {
    const auto& record = records[i];
    auto key = permission_key(record.kind, record.origin, nullptr);
    if (key && (record.state == EPHY_ENGINE_PERMISSION_STATE_ASK || record.state == EPHY_ENGINE_PERMISSION_STATE_ALLOW || record.state == EPHY_ENGINE_PERMISSION_STATE_DENY))
      data->legacy_permissions[*key] = record.state;
  }
  data->import_permissions = true;
  ephy_engine_session_context(session);
  data->apply_permissions();
}
namespace {
class Resolve final : public CefResolveCallback {
  void OnResolveCompleted(cef_errorcode_t, const std::vector<CefString>&) override {}
  IMPLEMENT_REFCOUNTING(Resolve);
};
std::string host_for(const char *uri) {
  CefURLParts parts;
  return uri && CefParseURL(uri, parts) ? CefString(&parts.host).ToString() : "";
}
std::string digest(const void *bytes, size_t size) {
  char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, static_cast<const guchar*>(bytes), size);
  std::string result(hash); g_free(hash); return result;
}
}
void ephy_engine_session_prefetch_dns(EphyEngineSession *session, const char *host) {
  if (!host || !*host) return;
  auto context = ephy_engine_session_context(session);
  char *uri = g_uri_join(G_URI_FLAGS_NONE, "https", nullptr, host, -1, "/", nullptr, nullptr);
  if (context && uri) context->ResolveHost(uri, new Resolve);
  g_free(uri);
}
void ephy_engine_session_allow_tls_certificate_for_host(EphyEngineSession *session, GTlsCertificate *certificate, const char *host) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session) && G_IS_TLS_CERTIFICATE(certificate));
  if (!host || !*host) return;
  GByteArray *bytes = nullptr;
  g_object_get(certificate, "certificate", &bytes, nullptr);
  if (!bytes) return;
  char *uri = g_uri_join(G_URI_FLAGS_NONE, "https", nullptr, host, -1, "/", nullptr, nullptr);
  auto canonical = host_for(uri);
  g_free(uri);
  if (!canonical.empty()) (*session->data)->certificates.emplace(canonical, digest(bytes->data, bytes->len));
  g_byte_array_unref(bytes);
}
bool ephy_engine_session_certificate_allowed(EphyEngineSession *session, const CefString& uri, CefRefPtr<CefX509Certificate> certificate) {
  if (!session || !certificate) return false;
  auto der = certificate->GetDEREncoded();
  if (!der) return false;
  std::vector<unsigned char> bytes(der->GetSize());
  der->GetData(bytes.data(), bytes.size(), 0);
  return (*session->data)->certificates.contains({host_for(uri.ToString().c_str()), digest(bytes.data(), bytes.size())});
}
namespace {
void free_cookie_list(gpointer cookies) { g_list_free_full(static_cast<GList*>(cookies), reinterpret_cast<GDestroyNotify>(soup_cookie_free)); }
SoupCookie *to_soup(const CefCookie& cookie) {
  auto *result = soup_cookie_new(CefString(&cookie.name).ToString().c_str(), CefString(&cookie.value).ToString().c_str(),
    CefString(&cookie.domain).ToString().c_str(), CefString(&cookie.path).ToString().c_str(), -1);
  soup_cookie_set_secure(result, cookie.secure);
  soup_cookie_set_http_only(result, cookie.httponly);
  SoupSameSitePolicy same_site = SOUP_SAME_SITE_POLICY_NONE;
  if (cookie.same_site == CEF_COOKIE_SAME_SITE_LAX_MODE) same_site = SOUP_SAME_SITE_POLICY_LAX;
  if (cookie.same_site == CEF_COOKIE_SAME_SITE_STRICT_MODE) same_site = SOUP_SAME_SITE_POLICY_STRICT;
  soup_cookie_set_same_site_policy(result, same_site);
  if (cookie.has_expires) {
    cef_time_t time; time_t unix_time;
    if (cef_time_from_basetime(cookie.expires, &time) && cef_time_to_timet(&time, &unix_time)) {
      auto *expires = g_date_time_new_from_unix_utc(unix_time);
      if (expires) { soup_cookie_set_expires(result, expires); g_date_time_unref(expires); }
    }
  }
  return result;
}
class Complete final : public CefCompletionCallback {
public:
  explicit Complete(GTask *task) : task_(task) {}
  ~Complete() override { if (task_) { g_task_return_new_error(task_, G_IO_ERROR, G_IO_ERROR_CLOSED, "Chromium session closed before the operation completed"); g_object_unref(task_); } }
  void OnComplete() override { g_task_return_boolean(task_, true); g_clear_object(&task_); }
private:
  GTask *task_;
  IMPLEMENT_REFCOUNTING(Complete);
};
class Cookies final : public CefCookieVisitor {
public:
  Cookies(GTask *task, CefRefPtr<CefCookieManager> manager, SoupCookie *remove = nullptr) : task_(task), manager_(manager), remove_(remove ? soup_cookie_copy(remove) : nullptr) {}
  ~Cookies() override {
    if (remove_) {
      soup_cookie_free(remove_);
      if (task_) {
        CefRefPtr<Complete> callback = new Complete(task_); task_ = nullptr;
        manager_->FlushStore(callback);
      }
    } else if (task_) {
      g_task_return_pointer(task_, g_list_reverse(cookies_), free_cookie_list);
      cookies_ = nullptr; g_clear_object(&task_);
    }
    free_cookie_list(cookies_);
  }
  void fail() {
    g_task_return_new_error(task_, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read Chromium cookies");
    g_clear_object(&task_);
  }
  bool Visit(const CefCookie& cookie, int, int, bool& erase) override {
    if (remove_) {
      erase = CefString(&cookie.name).ToString() == soup_cookie_get_name(remove_) &&
              CefString(&cookie.domain).ToString() == soup_cookie_get_domain(remove_) &&
              CefString(&cookie.path).ToString() == soup_cookie_get_path(remove_);
    } else cookies_ = g_list_prepend(cookies_, to_soup(cookie));
    return !g_task_get_cancellable(task_) || !g_cancellable_is_cancelled(g_task_get_cancellable(task_));
  }
private:
  GTask *task_;
  CefRefPtr<CefCookieManager> manager_;
  SoupCookie *remove_;
  GList *cookies_ = nullptr;
  IMPLEMENT_REFCOUNTING(Cookies);
};
class SetCookie final : public CefSetCookieCallback {
public:
  explicit SetCookie(GTask *task) : task_(task) {}
  ~SetCookie() override { if (task_) fail(); }
  void fail() {
    g_task_return_new_error(task_, G_IO_ERROR, G_IO_ERROR_FAILED, "Chromium rejected the cookie");
    g_clear_object(&task_);
  }
  void OnComplete(bool success) override {
    if (!success) { fail(); return; }
    g_task_return_boolean(task_, true); g_clear_object(&task_);
  }
private:
  GTask *task_;
  IMPLEMENT_REFCOUNTING(SetCookie);
};
class ClearCookies final : public CefCookieVisitor {
public:
  ClearCookies(GTask *task, CefRefPtr<CefCookieManager> manager, const char *domain)
    : task_(task), manager_(manager), domain_(domain ? domain : "") {}
  ~ClearCookies() override { if (task_) manager_->FlushStore(new Complete(task_)); }
  void fail() {
    g_task_return_new_error(task_, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not clear Chromium cookies");
    g_clear_object(&task_);
  }
  bool Visit(const CefCookie& cookie, int, int, bool& erase) override {
    if (g_task_get_cancellable(task_) && g_cancellable_is_cancelled(g_task_get_cancellable(task_))) return false;
    erase = domain_.empty() || CefString(&cookie.domain).ToString() == domain_;
    return true;
  }
private:
  GTask *task_;
  CefRefPtr<CefCookieManager> manager_;
  std::string domain_;
  IMPLEMENT_REFCOUNTING(ClearCookies);
};
CefRefPtr<CefCookieManager> cookie_manager(EphyEngineSession *session, GTask *task) {
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return nullptr; }
  auto context = ephy_engine_session_context(session);
  auto manager = context ? context->GetCookieManager(nullptr) : nullptr;
  if (!manager) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Chromium session is not initialized");
    g_object_unref(task);
  }
  return manager;
}
}

namespace {
std::string cookie_json(JsonObject* object) {
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object);
  g_autofree char* json = json_to_string(node, false); return json;
}
void native_cookie_done(GTask* task, const std::string& error) {
  if (!g_task_return_error_if_cancelled(task)) {
    if (error.empty()) g_task_return_boolean(task, true);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str());
  }
  g_object_unref(task);
}
struct NativeCookieDeletion : std::enable_shared_from_this<NativeCookieDeletion> {
  GTask* task;
  bool private_profile;
  std::vector<std::string> cookies;
  size_t index = 0;
  ~NativeCookieDeletion() { g_object_unref(task); }
  void next() {
    if (g_task_return_error_if_cancelled(task)) return;
    if (index == cookies.size()) { g_task_return_boolean(task, true); return; }
    JupiterBrave::browser_command(private_profile, "Network.deleteCookies", cookies[index++],
      [self = shared_from_this()](JsonObject*, const std::string& error) {
        if (!error.empty()) g_task_return_new_error(self->task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str());
        else self->next();
      });
  }
};
void native_delete_cookies(EphyEngineSession* session, GTask* task, const char* domain,
                           const char* name = nullptr, const char* path = nullptr) {
  if (!domain || !*domain || (name && (!path || !*path))) {
    native_cookie_done(task, "A cookie domain, name and path are required for selective deletion"); return;
  }
  auto deletion = std::make_shared<NativeCookieDeletion>();
  deletion->task = task; deletion->private_profile = ephy_engine_session_is_ephemeral(session);
  const std::string wanted_domain = domain ? domain : "", wanted_name = name ? name : "", wanted_path = path ? path : "";
  const bool individual = name != nullptr;
  JupiterBrave::browser_command(deletion->private_profile, "Storage.getCookies", "{}",
    [deletion, wanted_domain, wanted_name, wanted_path, individual](JsonObject* result, const std::string& error) {
      if (!error.empty()) { g_task_return_new_error(deletion->task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str()); return; }
      auto* cookies = result ? json_object_get_array_member(result, "cookies") : nullptr;
      for (guint i = 0; cookies && i < json_array_get_length(cookies); ++i) {
        auto* cookie = json_array_get_object_element(cookies, i);
        auto str = [cookie](const char* key) { return std::string(json_object_get_string_member_with_default(cookie, key, "")); };
        if (!wanted_domain.empty() && wanted_domain != str("domain")) continue;
        if (individual && (wanted_name != str("name") || wanted_path != str("path"))) continue;
        if (json_object_get_boolean_member_with_default(cookie, "partitionKeyOpaque", false)) {
          g_task_return_new_error(deletion->task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "An opaque partition cookie cannot be deleted individually"); return;
        }
        g_autoptr(JsonObject) parameters = json_object_new();
        for (const char* key : {"name", "domain", "path"}) json_object_set_string_member(parameters, key, str(key).c_str());
        if (auto* partition = json_object_get_member(cookie, "partitionKey"))
          json_object_set_member(parameters, "partitionKey", json_node_copy(partition));
        deletion->cookies.push_back(cookie_json(parameters));
      }
      deletion->next();
    });
}
}
gboolean ephy_engine_session_uses_brave(EphyEngineSession*) { return JupiterBrave::enabled(); }
void ephy_engine_session_clear_website_storage(EphyEngineSession* session, const char* const* types,
                                               GCancellable* cancel, GAsyncReadyCallback callback, gpointer user) {
  auto* task = g_task_new(session, cancel, callback, user);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  g_autoptr(JsonArray) array = json_array_new();
  for (unsigned i = 0; types && types[i]; ++i) json_array_add_string_element(array, types[i]);
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, array);
  g_autofree char* json = json_to_string(node, false);
  JupiterBrave::clear_website_data(ephy_engine_session_is_ephemeral(session), json,
    [task](JsonObject*, const std::string& error) { native_cookie_done(task, error); });
}
gboolean ephy_engine_session_clear_website_storage_finish(EphyEngineSession* session, GAsyncResult* result, GError** error) {
  g_return_val_if_fail(g_task_is_valid(result, session), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
void ephy_engine_session_get_cookies(EphyEngineSession *session, const char *uri, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  auto *task = g_task_new(session, cancel, callback, user);
  if (JupiterBrave::enabled()) {
    if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
    const std::string filter = uri ? uri : "";
    JupiterBrave::browser_command(ephy_engine_session_is_ephemeral(session), "Storage.getCookies", "{}",
      [task, filter](JsonObject* result, const std::string& error) {
        if (!error.empty()) { native_cookie_done(task, error); return; }
        GList* list = nullptr;
        g_autoptr(GUri) uri = filter.empty() ? nullptr : g_uri_parse(filter.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
        auto* cookies = result ? json_object_get_array_member(result, "cookies") : nullptr;
        for (guint i = 0; cookies && i < json_array_get_length(cookies); ++i) {
          auto* value = json_array_get_object_element(cookies, i);
          auto* cookie = soup_cookie_new(json_object_get_string_member(value, "name"), json_object_get_string_member(value, "value"),
            json_object_get_string_member(value, "domain"), json_object_get_string_member(value, "path"), -1);
          if (!cookie) continue;
          soup_cookie_set_secure(cookie, json_object_get_boolean_member_with_default(value, "secure", false));
          soup_cookie_set_http_only(cookie, json_object_get_boolean_member_with_default(value, "httpOnly", false));
          const char* same_site = json_object_get_string_member_with_default(value, "sameSite", "None");
          soup_cookie_set_same_site_policy(cookie, !strcmp(same_site,"Strict") ? SOUP_SAME_SITE_POLICY_STRICT :
            !strcmp(same_site,"Lax") ? SOUP_SAME_SITE_POLICY_LAX : SOUP_SAME_SITE_POLICY_NONE);
          const double expires = json_object_get_double_member_with_default(value, "expires", -1);
          if (expires > 0) { auto* date = g_date_time_new_from_unix_utc((gint64)expires); soup_cookie_set_expires(cookie, date); g_date_time_unref(date); }
          if (!filter.empty() && (!uri || !soup_cookie_applies_to_uri(cookie, uri))) soup_cookie_free(cookie);
          else list = g_list_prepend(list, cookie);
        }
        list = g_list_reverse(list);
        if (g_task_return_error_if_cancelled(task)) g_list_free_full(list, (GDestroyNotify)soup_cookie_free);
        else g_task_return_pointer(task, list, +[](gpointer value) { g_list_free_full(static_cast<GList*>(value), (GDestroyNotify)soup_cookie_free); });
        g_object_unref(task);
      });
    return;
  }
  auto manager = cookie_manager(session, task);
  if (!manager) return;
  CefRefPtr<Cookies> visitor = new Cookies(task, manager);
  bool ok = uri ? manager->VisitUrlCookies(uri, true, visitor) : manager->VisitAllCookies(visitor);
  if (!ok) visitor->fail();
}
GList *ephy_engine_session_get_cookies_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, session), nullptr);
  return static_cast<GList*>(g_task_propagate_pointer(G_TASK(result), error));
}
void ephy_engine_session_add_cookie(EphyEngineSession *session, SoupCookie *source, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  auto *task = g_task_new(session, cancel, callback, user);
  if (JupiterBrave::enabled()) {
    if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
    g_autoptr(JsonObject) cookie = json_object_new();
    for (auto [key, value] : {std::pair{"name",soup_cookie_get_name(source)}, {"value",soup_cookie_get_value(source)},
      {"domain",soup_cookie_get_domain(source)}, {"path",soup_cookie_get_path(source)}})
      json_object_set_string_member(cookie, key, value ? value : "");
    json_object_set_boolean_member(cookie, "secure", soup_cookie_get_secure(source));
    json_object_set_boolean_member(cookie, "httpOnly", soup_cookie_get_http_only(source));
    auto same_site = soup_cookie_get_same_site_policy(source);
    if (same_site != SOUP_SAME_SITE_POLICY_NONE || soup_cookie_get_secure(source))
      json_object_set_string_member(cookie, "sameSite", same_site == SOUP_SAME_SITE_POLICY_STRICT ? "Strict" : same_site == SOUP_SAME_SITE_POLICY_LAX ? "Lax" : "None");
    if (auto* expires = soup_cookie_get_expires(source)) json_object_set_double_member(cookie, "expires", g_date_time_to_unix(expires));
    JupiterBrave::browser_command(ephy_engine_session_is_ephemeral(session), "Storage.setCookies", "{\"cookies\":[" + cookie_json(cookie) + "]}",
      [task](JsonObject*, const std::string& error) { native_cookie_done(task, error); });
    return;
  }
  auto manager = cookie_manager(session, task);
  if (!manager) return;
  CefCookie cookie;
  const char *domain = soup_cookie_get_domain(source);
  const char *path = soup_cookie_get_path(source);
  CefString(&cookie.name) = soup_cookie_get_name(source);
  CefString(&cookie.value) = soup_cookie_get_value(source);
  if (domain && domain[0] == '.') CefString(&cookie.domain) = domain;
  CefString(&cookie.path) = path;
  cookie.secure = soup_cookie_get_secure(source);
  cookie.httponly = soup_cookie_get_http_only(source);
  auto same_site = soup_cookie_get_same_site_policy(source);
  cookie.same_site = same_site == SOUP_SAME_SITE_POLICY_STRICT ? CEF_COOKIE_SAME_SITE_STRICT_MODE :
    same_site == SOUP_SAME_SITE_POLICY_LAX ? CEF_COOKIE_SAME_SITE_LAX_MODE :
    cookie.secure ? CEF_COOKIE_SAME_SITE_NO_RESTRICTION : CEF_COOKIE_SAME_SITE_UNSPECIFIED;
  if (auto *expires = soup_cookie_get_expires(source)) {
    cef_time_t time;
    cookie.has_expires = cef_time_from_timet(g_date_time_to_unix(expires), &time) && cef_time_to_basetime(&time, &cookie.expires);
  }
  char *uri = domain ? g_uri_join(G_URI_FLAGS_ENCODED, cookie.secure ? "https" : "http", nullptr,
    domain[0] == '.' ? domain + 1 : domain, -1, path ? path : "/", nullptr, nullptr) : nullptr;
  CefRefPtr<SetCookie> done = new SetCookie(task);
  if (!uri || !manager->SetCookie(uri, cookie, done)) done->fail();
  g_free(uri);
}
gboolean ephy_engine_session_add_cookie_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, session), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
void ephy_engine_session_delete_cookie(EphyEngineSession *session, SoupCookie *cookie, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  auto *task = g_task_new(session, cancel, callback, user);
  if (JupiterBrave::enabled()) {
    native_delete_cookies(session, task, soup_cookie_get_domain(cookie), soup_cookie_get_name(cookie), soup_cookie_get_path(cookie)); return;
  }
  auto manager = cookie_manager(session, task);
  if (!manager) return;
  CefRefPtr<Cookies> visitor = new Cookies(task, manager, cookie);
  if (!manager->VisitAllCookies(visitor)) visitor->fail();
}
gboolean ephy_engine_session_delete_cookie_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, session), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
void ephy_engine_session_clear_cookies(EphyEngineSession *session, const char *domain, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session));
  auto *task = g_task_new(session, cancel, callback, user);
  if (domain && !*domain) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Cookie domain must not be empty");
    g_object_unref(task); return;
  }
  if (JupiterBrave::enabled()) {
    if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
    if (domain) native_delete_cookies(session, task, domain);
    else JupiterBrave::browser_command(ephy_engine_session_is_ephemeral(session), "Storage.clearCookies", "{}",
      [task](JsonObject*, const std::string& error) { native_cookie_done(task, error); });
    return;
  }
  auto manager = cookie_manager(session, task);
  if (!manager) return;
  CefRefPtr<ClearCookies> visitor = new ClearCookies(task, manager, domain);
  if (!manager->VisitAllCookies(visitor)) visitor->fail();
}
gboolean ephy_engine_session_clear_cookies_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, session), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
void ephy_engine_session_clear_cache(EphyEngineSession *session, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  g_return_if_fail(EPHY_IS_ENGINE_SESSION(session));
  auto *task = g_task_new(session, cancel, callback, user);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (JupiterBrave::enabled()) {
    JupiterBrave::browser_command(ephy_engine_session_is_ephemeral(session), "Network.clearBrowserCache", "{}",
      [task](JsonObject*, const std::string& error) { native_cookie_done(task, error); });
    return;
  }
  auto context = ephy_engine_session_context(session);
  if (!context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "Chromium session is not ready");
    g_object_unref(task); return;
  }
  context->ClearHttpCache(new Complete(task));
}
gboolean ephy_engine_session_clear_cache_finish(EphyEngineSession *session, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, session), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
