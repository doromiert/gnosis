/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "include/cef_x509_certificate.h"
#include "include/cef_request_context.h"
#include <gio/gio.h>

GTlsCertificate *ephy_engine_certificate_to_tls(CefRefPtr<CefX509Certificate>);
GTlsCertificateFlags ephy_engine_certificate_errors(cef_cert_status_t, CefRefPtr<CefX509Certificate>);
bool ephy_engine_certificate_reset_cached_decisions(CefRefPtr<CefRequestContext>, const CefString& uri, bool ephemeral);
