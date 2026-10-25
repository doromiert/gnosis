/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
typedef struct _EphyEngineSession EphyEngineSession;
#define EPHY_TYPE_ENGINE_CLIENT_CERTIFICATE (ephy_engine_client_certificate_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineClientCertificate, ephy_engine_client_certificate, EPHY, ENGINE_CLIENT_CERTIFICATE, GObject)
const char *ephy_engine_client_certificate_get_host(EphyEngineClientCertificate *);
gint ephy_engine_client_certificate_get_port(EphyEngineClientCertificate *);
gboolean ephy_engine_client_certificate_is_proxy(EphyEngineClientCertificate *);
gboolean ephy_engine_client_certificate_is_ephemeral(EphyEngineClientCertificate *);
gboolean ephy_engine_client_certificate_is_resolved(EphyEngineClientCertificate *);
/* Immutable original candidate order. Strings/certificates are borrowed; a
 * missing GTK conversion does not substitute a different native candidate. */
guint ephy_engine_client_certificate_get_n_certificates(EphyEngineClientCertificate *);
const char *ephy_engine_client_certificate_get_subject(EphyEngineClientCertificate *, guint index);
const char *ephy_engine_client_certificate_get_issuer(EphyEngineClientCertificate *, guint index);
GTlsCertificate *ephy_engine_client_certificate_get_certificate(EphyEngineClientCertificate *, guint index);
/* Borrowed terminal token, cancelled on selection or cancellation. */
GCancellable *ephy_engine_client_certificate_get_cancellable(EphyEngineClientCertificate *);
/* UI-thread operations. Invalid indices do not resolve the request. */
gboolean ephy_engine_client_certificate_select(EphyEngineClientCertificate *, guint index);
gboolean ephy_engine_client_certificate_cancel(EphyEngineClientCertificate *);
G_END_DECLS

#ifdef __cplusplus
#include "include/cef_x509_certificate.h"
#include <functional>
#include <vector>
EphyEngineClientCertificate *ephy_engine_client_certificate_new(const char *host, int port, bool proxy,
  const std::vector<CefRefPtr<CefX509Certificate>> &certificates, EphyEngineSession *,
  std::function<bool(CefRefPtr<CefX509Certificate>)> reply);
#endif
