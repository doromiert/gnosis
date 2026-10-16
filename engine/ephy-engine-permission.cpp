/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-permission.h"
#include "ephy-engine-origin.h"
#include "ephy-engine-session.h"
#include <string>
#include <utility>
namespace {
struct Permission {
  EphyEnginePermissionKind kind = EPHY_ENGINE_PERMISSION_OTHER;
  std::string origin, top_origin, description;
  EphyEngineSession *session = nullptr;
  bool resolved = false;
  std::function<void(EphyEnginePermissionDecision)> reply;
};
}
struct _EphyEnginePermission { GObject parent; Permission *data; };
G_DEFINE_TYPE(EphyEnginePermission, ephy_engine_permission, G_TYPE_OBJECT)
static void ephy_engine_permission_class_init(EphyEnginePermissionClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *permission = EPHY_ENGINE_PERMISSION(object)->data;
    if (permission->reply) permission->reply(EPHY_ENGINE_PERMISSION_DISMISS);
    g_clear_object(&permission->session);
    delete permission;
    G_OBJECT_CLASS(ephy_engine_permission_parent_class)->finalize(object);
  };
  g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void ephy_engine_permission_init(EphyEnginePermission *self) { self->data = new Permission; }
EphyEnginePermission *ephy_engine_permission_new(EphyEnginePermissionKind kind, const char *origin,
  const char *top_origin, EphyEngineSession *session, const char *description,
  std::function<void(EphyEnginePermissionDecision)> reply) {
  g_autoptr(EphyEngineOrigin) requester = ephy_engine_origin_new_for_uri(origin);
  if (!requester || !EPHY_IS_ENGINE_SESSION(session)) { reply(EPHY_ENGINE_PERMISSION_DENY); return nullptr; }
  g_autoptr(EphyEngineOrigin) top = ephy_engine_origin_new_for_uri(top_origin);
  if (kind == EPHY_ENGINE_PERMISSION_STORAGE_ACCESS && !top) { reply(EPHY_ENGINE_PERMISSION_DENY); return nullptr; }
  g_autofree char *requester_uri = ephy_engine_origin_to_string(requester);
  g_autofree char *top_uri = top ? ephy_engine_origin_to_string(top) : nullptr;
  auto *self = EPHY_ENGINE_PERMISSION(g_object_new(EPHY_TYPE_ENGINE_PERMISSION, nullptr));
  auto *data = self->data;
  data->kind = kind; data->origin = requester_uri; data->top_origin = top_uri ? top_uri : "";
  data->session = EPHY_ENGINE_SESSION(g_object_ref(session));
  data->description = description ? description : "";
  data->reply = std::move(reply);
  return self;
}
EphyEnginePermissionKind ephy_engine_permission_get_kind(EphyEnginePermission *self) { return self->data->kind; }
const char *ephy_engine_permission_get_origin(EphyEnginePermission *self) { return self->data->origin.c_str(); }
const char *ephy_engine_permission_get_top_origin(EphyEnginePermission *self) { return self->data->top_origin.c_str(); }
const char *ephy_engine_permission_get_description(EphyEnginePermission *self) { return self->data->description.c_str(); }
gboolean ephy_engine_permission_is_ephemeral(EphyEnginePermission *self) { return ephy_engine_session_is_ephemeral(self->data->session); }
gboolean ephy_engine_permission_is_resolved(EphyEnginePermission *self) { return self->data->resolved; }
EphyEnginePermissionState ephy_engine_permission_get_state(EphyEnginePermission *self) {
  auto *data = self->data;
  if (data->top_origin.empty()) return EPHY_ENGINE_PERMISSION_STATE_ASK;
  auto get = [&](auto kind) { return ephy_engine_session_get_permission(data->session, kind, data->origin.c_str(), data->top_origin.c_str()); };
  if (data->kind == EPHY_ENGINE_PERMISSION_CAMERA_AND_MICROPHONE) {
    auto audio = get(EPHY_ENGINE_PERMISSION_MICROPHONE), video = get(EPHY_ENGINE_PERMISSION_CAMERA);
    if (audio == EPHY_ENGINE_PERMISSION_STATE_DENY || video == EPHY_ENGINE_PERMISSION_STATE_DENY) return EPHY_ENGINE_PERMISSION_STATE_DENY;
    return audio == video ? audio : EPHY_ENGINE_PERMISSION_STATE_ASK;
  }
  return get(data->kind);
}
gboolean ephy_engine_permission_decide(EphyEnginePermission *self, EphyEnginePermissionDecision decision) {
  g_return_val_if_fail(EPHY_IS_ENGINE_PERMISSION(self), false);
  g_return_val_if_fail(decision >= EPHY_ENGINE_PERMISSION_ALLOW && decision <= EPHY_ENGINE_PERMISSION_DISMISS, false);
  auto *data = self->data;
  if (data->resolved) return false;
  g_autoptr(EphyEnginePermission) keep_alive = EPHY_ENGINE_PERMISSION(g_object_ref(self));
  data->resolved = true;
  if (decision != EPHY_ENGINE_PERMISSION_DISMISS && !data->top_origin.empty()) {
    auto state = decision == EPHY_ENGINE_PERMISSION_ALLOW ? EPHY_ENGINE_PERMISSION_STATE_ALLOW : EPHY_ENGINE_PERMISSION_STATE_DENY;
    auto set = [&](auto kind) { ephy_engine_session_set_permission(data->session, kind, data->origin.c_str(), data->top_origin.c_str(), state); };
    if (data->kind == EPHY_ENGINE_PERMISSION_CAMERA_AND_MICROPHONE) {
      set(EPHY_ENGINE_PERMISSION_MICROPHONE); set(EPHY_ENGINE_PERMISSION_CAMERA);
    } else set(data->kind);
    if (decision == EPHY_ENGINE_PERMISSION_ALLOW && ephy_engine_permission_get_state(self) == EPHY_ENGINE_PERMISSION_STATE_DENY)
      decision = EPHY_ENGINE_PERMISSION_DENY;
  }
  auto reply = std::move(data->reply);
  if (reply) reply(decision);
  g_signal_emit_by_name(self, "finished");
  return true;
}
void ephy_engine_permission_cancel(EphyEnginePermission *self) {
  auto *data = self->data;
  if (data->resolved) return;
  g_autoptr(EphyEnginePermission) keep_alive = EPHY_ENGINE_PERMISSION(g_object_ref(self));
  data->resolved = true; data->reply = nullptr;
  g_signal_emit_by_name(self, "finished");
}
