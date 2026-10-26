/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <libsoup/soup.h>
#include "ephy-engine-download.h"
#include "ephy-engine-permission.h"

G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_SESSION (ephy_engine_session_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineSession, ephy_engine_session, EPHY, ENGINE_SESSION, GObject)
EphyEngineSession *ephy_engine_session_new(gboolean ephemeral);
EphyEngineSession *ephy_engine_session_get_default(void);
gboolean ephy_engine_session_is_ephemeral(EphyEngineSession *);
/* UI-thread upload chooser memory. Returns a full reference to an existing local
 * directory, or NULL. Regular profiles persist through their CEF preference;
 * private sessions retain only their own memory until session/engine teardown.
 * A successful set may be queued until the regular request context is ready. */
gboolean ephy_engine_session_uses_brave(EphyEngineSession *);
void ephy_engine_session_clear_website_storage(EphyEngineSession *, const char *const *types,
                                               GCancellable *, GAsyncReadyCallback, gpointer);
gboolean ephy_engine_session_clear_website_storage_finish(EphyEngineSession *, GAsyncResult *, GError **);
GFile *ephy_engine_session_dup_last_upload_folder(EphyEngineSession *);
gboolean ephy_engine_session_set_last_upload_folder(EphyEngineSession *, GFile *folder);

typedef struct {
  EphyEnginePermissionKind kind;
  const char *origin;
  EphyEnginePermissionState state;
} EphyEnginePermissionRecord;
/* UI-thread APIs. A NULL top origin means wildcard scope when setting and the
 * requesting origin when querying. Unsupported kinds and opaque origins cannot be set. */
gboolean ephy_engine_session_set_permission(EphyEngineSession *, EphyEnginePermissionKind,
  const char *origin, const char *top_origin, EphyEnginePermissionState);
EphyEnginePermissionState ephy_engine_session_get_permission(EphyEngineSession *, EphyEnginePermissionKind,
  const char *origin, const char *top_origin);
/* Copies records and imports them only if this CEF profile has not migrated yet. */
void ephy_engine_session_import_permissions(EphyEngineSession *, const EphyEnginePermissionRecord *, gsize count);
void ephy_engine_session_clear_cache(EphyEngineSession *, GCancellable *, GAsyncReadyCallback, gpointer);
gboolean ephy_engine_session_clear_cache_finish(EphyEngineSession *, GAsyncResult *, GError **);
void ephy_engine_session_set_storage_enabled(EphyEngineSession *, gboolean enabled);
void ephy_engine_session_prefetch_dns(EphyEngineSession *, const char *host);
void ephy_engine_session_allow_tls_certificate_for_host(EphyEngineSession *, GTlsCertificate *, const char *host);
void ephy_engine_session_get_cookies(EphyEngineSession *, const char *uri, GCancellable *, GAsyncReadyCallback, gpointer);
GList *ephy_engine_session_get_cookies_finish(EphyEngineSession *, GAsyncResult *, GError **);
void ephy_engine_session_add_cookie(EphyEngineSession *, SoupCookie *, GCancellable *, GAsyncReadyCallback, gpointer);
gboolean ephy_engine_session_add_cookie_finish(EphyEngineSession *, GAsyncResult *, GError **);
void ephy_engine_session_delete_cookie(EphyEngineSession *, SoupCookie *, GCancellable *, GAsyncReadyCallback, gpointer);
gboolean ephy_engine_session_delete_cookie_finish(EphyEngineSession *, GAsyncResult *, GError **);
/* A NULL domain clears all cookies in this session, including partitioned cookies. */
void ephy_engine_session_clear_cookies(EphyEngineSession *, const char *domain, GCancellable *, GAsyncReadyCallback, gpointer);
gboolean ephy_engine_session_clear_cookies_finish(EphyEngineSession *, GAsyncResult *, GError **);
G_END_DECLS

#ifdef __cplusplus
#include "include/cef_request_context.h"
#include "include/cef_x509_certificate.h"
#include "include/cef_resource_request_handler.h"
CefRefPtr<CefRequestContext> ephy_engine_session_context(EphyEngineSession *);
CefRefPtr<CefCookieAccessFilter> ephy_engine_session_cookie_filter(EphyEngineSession *);
CefRefPtr<CefResourceRequestHandler> ephy_engine_session_request_handler(EphyEngineSession *);
bool ephy_engine_session_certificate_allowed(EphyEngineSession *, const CefString& uri, CefRefPtr<CefX509Certificate>);
void ephy_engine_sessions_initialize();
void ephy_engine_sessions_preferences_changed();
void ephy_engine_sessions_shutdown();
void ephy_engine_sessions_register_preferences(cef_preferences_type_t, CefRawPtr<CefPreferenceRegistrar>);
#endif
