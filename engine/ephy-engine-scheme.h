/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ephy-engine-view.h"
G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_SCHEME_REQUEST (ephy_engine_scheme_request_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineSchemeRequest,ephy_engine_scheme_request,EPHY,ENGINE_SCHEME_REQUEST,GObject)
typedef void (*EphyEngineSchemeRequestCallback)(EphyEngineSchemeRequest*, gpointer);
void ephy_engine_context_register_uri_scheme(WebKitWebContext*,const char*,EphyEngineSchemeRequestCallback,gpointer,GDestroyNotify);
const char* ephy_engine_uri_scheme_request_get_uri(EphyEngineSchemeRequest*);
const char* ephy_engine_uri_scheme_request_get_path(EphyEngineSchemeRequest*);
EphyEngineView* ephy_engine_uri_scheme_request_get_web_view(EphyEngineSchemeRequest*);
void ephy_engine_uri_scheme_request_finish(EphyEngineSchemeRequest*,GInputStream*,gint64,const char*);
void ephy_engine_uri_scheme_request_finish_error(EphyEngineSchemeRequest*,GError*);
G_END_DECLS
#ifdef __cplusplus
#include "include/cef_scheme.h"
void ephy_engine_register_schemes(CefRawPtr<CefSchemeRegistrar>);
void ephy_engine_install_scheme_handlers();
void ephy_engine_clear_scheme_handlers();
#endif
