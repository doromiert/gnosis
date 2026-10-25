/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_SETTINGS (ephy_engine_settings_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineSettings, ephy_engine_settings, EPHY, ENGINE_SETTINGS, GObject)
/* Profile preferences are shared by ordinary and private request contexts. */
EphyEngineSettings *ephy_engine_settings_get_default(void);
gboolean ephy_engine_settings_get_storage_enabled(void);
gboolean ephy_engine_settings_get_hardware_acceleration(void);
char *ephy_engine_settings_dup_user_agent(void);

typedef struct {
  const char *id, *name, *category, *details;
  gboolean default_value;
  const char *enable_switch, *disable_switch, *feature_flag;
} EphyEngineFeature;
const EphyEngineFeature *ephy_engine_settings_get_features(gsize *length);
gboolean ephy_engine_settings_get_feature_enabled(EphyEngineSettings *, const EphyEngineFeature *);
void ephy_engine_settings_set_feature_enabled(EphyEngineSettings *, const EphyEngineFeature *, gboolean);
G_END_DECLS

#ifdef __cplusplus
#include "include/cef_request_context.h"
#include "include/cef_command_line.h"
#include "include/cef_request.h"
#include "include/cef_resource_request_handler.h"
void ephy_engine_settings_apply_context(CefRefPtr<CefRequestContext>);
void ephy_engine_settings_configure_command_line(CefRefPtr<CefCommandLine>);
void ephy_engine_settings_apply_request(CefRefPtr<CefRequest>, const std::string *user_agent_override = nullptr);
CefRefPtr<CefResourceRequestHandler> ephy_engine_settings_request_handler(CefRefPtr<CefCookieAccessFilter>,
  CefRefPtr<CefResourceRequestHandler> next = nullptr);
#endif
