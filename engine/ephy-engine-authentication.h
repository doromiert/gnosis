/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
typedef struct _EphyEngineSession EphyEngineSession;
#define EPHY_TYPE_ENGINE_AUTHENTICATION (ephy_engine_authentication_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineAuthentication, ephy_engine_authentication, EPHY, ENGINE_AUTHENTICATION, GObject)
const char *ephy_engine_authentication_get_origin_url(EphyEngineAuthentication *);
const char *ephy_engine_authentication_get_host(EphyEngineAuthentication *);
gint ephy_engine_authentication_get_port(EphyEngineAuthentication *);
const char *ephy_engine_authentication_get_realm(EphyEngineAuthentication *);
const char *ephy_engine_authentication_get_scheme(EphyEngineAuthentication *);
gboolean ephy_engine_authentication_is_proxy(EphyEngineAuthentication *);
gboolean ephy_engine_authentication_is_ephemeral(EphyEngineAuthentication *);
gboolean ephy_engine_authentication_is_resolved(EphyEngineAuthentication *);
/* Borrowed lifetime token, cancelled on every terminal resolution. */
GCancellable *ephy_engine_authentication_get_cancellable(EphyEngineAuthentication *);
/* UI-thread operations; FALSE means already resolved or no longer applicable. */
gboolean ephy_engine_authentication_authenticate(EphyEngineAuthentication *, const char *username, const char *password);
gboolean ephy_engine_authentication_cancel(EphyEngineAuthentication *);
G_END_DECLS
#ifdef __cplusplus
#include <functional>
EphyEngineAuthentication *ephy_engine_authentication_new(const char *origin_url, const char *host, int port,
  const char *realm, const char *scheme, bool proxy, EphyEngineSession *,
  std::function<bool(const char *, const char *)> reply);
#endif
