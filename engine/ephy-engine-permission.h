/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib-object.h>
G_BEGIN_DECLS
typedef struct _EphyEngineSession EphyEngineSession;
#define EPHY_TYPE_ENGINE_PERMISSION (ephy_engine_permission_get_type())
G_DECLARE_FINAL_TYPE(EphyEnginePermission, ephy_engine_permission, EPHY, ENGINE_PERMISSION, GObject)
typedef enum {
  EPHY_ENGINE_PERMISSION_NOTIFICATIONS,
  EPHY_ENGINE_PERMISSION_LOCATION,
  EPHY_ENGINE_PERMISSION_CLIPBOARD,
  EPHY_ENGINE_PERMISSION_STORAGE_ACCESS,
  EPHY_ENGINE_PERMISSION_MICROPHONE,
  EPHY_ENGINE_PERMISSION_CAMERA,
  EPHY_ENGINE_PERMISSION_CAMERA_AND_MICROPHONE,
  EPHY_ENGINE_PERMISSION_DISPLAY,
  EPHY_ENGINE_PERMISSION_OTHER
} EphyEnginePermissionKind;
typedef enum {
  EPHY_ENGINE_PERMISSION_ALLOW,
  EPHY_ENGINE_PERMISSION_DENY,
  EPHY_ENGINE_PERMISSION_DISMISS
} EphyEnginePermissionDecision;
typedef enum {
  EPHY_ENGINE_PERMISSION_STATE_ASK = -1,
  EPHY_ENGINE_PERMISSION_STATE_DENY = 0,
  EPHY_ENGINE_PERMISSION_STATE_ALLOW = 1
} EphyEnginePermissionState;
EphyEnginePermissionKind ephy_engine_permission_get_kind(EphyEnginePermission *);
const char *ephy_engine_permission_get_origin(EphyEnginePermission *);
const char *ephy_engine_permission_get_top_origin(EphyEnginePermission *);
const char *ephy_engine_permission_get_description(EphyEnginePermission *);
gboolean ephy_engine_permission_is_ephemeral(EphyEnginePermission *);
gboolean ephy_engine_permission_is_resolved(EphyEnginePermission *);
EphyEnginePermissionState ephy_engine_permission_get_state(EphyEnginePermission *);
/* Returns FALSE for an already answered or cancelled request. */
gboolean ephy_engine_permission_decide(EphyEnginePermission *, EphyEnginePermissionDecision);
G_END_DECLS
#ifdef __cplusplus
#include <functional>
EphyEnginePermission *ephy_engine_permission_new(EphyEnginePermissionKind, const char *origin,
  const char *top_origin, EphyEngineSession *session, const char *description,
  std::function<void(EphyEnginePermissionDecision)> reply);
/* Chromium has already withdrawn the request; close its UI without answering again. */
void ephy_engine_permission_cancel(EphyEnginePermission *);
#endif
