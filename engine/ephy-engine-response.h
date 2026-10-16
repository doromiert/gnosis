/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_RESPONSE (ephy_engine_response_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineResponse, ephy_engine_response, EPHY, ENGINE_RESPONSE, GObject)
const char *ephy_engine_response_get_uri(EphyEngineResponse *);
const char *ephy_engine_response_get_mime_type(EphyEngineResponse *);
const char *ephy_engine_response_get_suggested_filename(EphyEngineResponse *);
guint ephy_engine_response_get_status_code(EphyEngineResponse *);
gint64 ephy_engine_response_get_content_length(EphyEngineResponse *);
SoupMessageHeaders *ephy_engine_response_get_http_headers(EphyEngineResponse *);
G_END_DECLS

#ifdef __cplusplus
#include "include/cef_response.h"
EphyEngineResponse *ephy_engine_response_new(const std::string& uri, CefRefPtr<CefResponse>);
#endif
