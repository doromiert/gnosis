/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-authentication.h"
#include "ephy-engine-session.h"
#include <string>
#include <utility>
namespace {
struct Authentication {
  std::string origin_url, host, realm, scheme;
  int port = 0;
  bool proxy = false, resolved = false;
  EphyEngineSession *session = nullptr;
  GCancellable *cancellable = g_cancellable_new();
  std::function<bool(const char *, const char *)> reply;
};
}
struct _EphyEngineAuthentication { GObject parent; Authentication *data; };
G_DEFINE_TYPE(EphyEngineAuthentication, ephy_engine_authentication, G_TYPE_OBJECT)
static void ephy_engine_authentication_class_init(EphyEngineAuthenticationClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *data = EPHY_ENGINE_AUTHENTICATION(object)->data;
    data->resolved = true;
    auto reply = std::move(data->reply);
    if (reply) reply(nullptr, nullptr);
    g_cancellable_cancel(data->cancellable);
    g_clear_object(&data->cancellable); g_clear_object(&data->session);
    delete data;
    G_OBJECT_CLASS(ephy_engine_authentication_parent_class)->finalize(object);
  };
  g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void ephy_engine_authentication_init(EphyEngineAuthentication *self) { self->data = new Authentication; }
EphyEngineAuthentication *ephy_engine_authentication_new(const char *origin_url, const char *host, int port,
  const char *realm, const char *scheme, bool proxy, EphyEngineSession *session,
  std::function<bool(const char *, const char *)> reply) {
  if (!EPHY_IS_ENGINE_SESSION(session) || !host || !*host || port < 1 || port > 65535) {
    if (reply) reply(nullptr, nullptr);
    return nullptr;
  }
  auto *self = EPHY_ENGINE_AUTHENTICATION(g_object_new(EPHY_TYPE_ENGINE_AUTHENTICATION, nullptr));
  auto *data = self->data;
  data->origin_url = origin_url ? origin_url : ""; data->host = host ? host : "";
  data->port = port; data->realm = realm ? realm : ""; data->scheme = scheme ? scheme : "";
  data->proxy = proxy; data->session = EPHY_ENGINE_SESSION(g_object_ref(session));
  data->reply = std::move(reply);
  return self;
}
const char *ephy_engine_authentication_get_origin_url(EphyEngineAuthentication *self) { return self->data->origin_url.c_str(); }
const char *ephy_engine_authentication_get_host(EphyEngineAuthentication *self) { return self->data->host.c_str(); }
gint ephy_engine_authentication_get_port(EphyEngineAuthentication *self) { return self->data->port; }
const char *ephy_engine_authentication_get_realm(EphyEngineAuthentication *self) { return self->data->realm.c_str(); }
const char *ephy_engine_authentication_get_scheme(EphyEngineAuthentication *self) { return self->data->scheme.c_str(); }
gboolean ephy_engine_authentication_is_proxy(EphyEngineAuthentication *self) { return self->data->proxy; }
gboolean ephy_engine_authentication_is_ephemeral(EphyEngineAuthentication *self) { return ephy_engine_session_is_ephemeral(self->data->session); }
gboolean ephy_engine_authentication_is_resolved(EphyEngineAuthentication *self) { return self->data->resolved; }
GCancellable *ephy_engine_authentication_get_cancellable(EphyEngineAuthentication *self) { return self->data->cancellable; }
namespace {
bool finish(EphyEngineAuthentication *self, const char *username, const char *password) {
  auto *data = self->data;
  if (data->resolved) return false;
  g_autoptr(EphyEngineAuthentication) keep_alive = EPHY_ENGINE_AUTHENTICATION(g_object_ref(self));
  data->resolved = true;
  auto reply = std::move(data->reply);
  const bool accepted = reply && reply(username, password);
  g_cancellable_cancel(data->cancellable);
  g_signal_emit_by_name(self, "finished");
  return username ? accepted : true;
}
}
gboolean ephy_engine_authentication_authenticate(EphyEngineAuthentication *self, const char *username, const char *password) {
  g_return_val_if_fail(EPHY_IS_ENGINE_AUTHENTICATION(self), false);
  g_return_val_if_fail(username && password, false);
  return finish(self, username, password);
}
gboolean ephy_engine_authentication_cancel(EphyEngineAuthentication *self) {
  g_return_val_if_fail(EPHY_IS_ENGINE_AUTHENTICATION(self), false);
  return finish(self, nullptr, nullptr);
}
