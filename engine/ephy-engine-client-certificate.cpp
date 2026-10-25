/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-client-certificate.h"
#include "ephy-engine-certificate.hpp"
#include "ephy-engine-session.h"
#include <string>
#include <utility>

namespace {
struct Candidate {
  CefRefPtr<CefX509Certificate> native;
  std::string subject, issuer;
  GTlsCertificate *certificate = nullptr;
};
struct ClientCertificate {
  std::string host;
  int port = 0;
  bool proxy = false, resolved = false;
  EphyEngineSession *session = nullptr;
  GCancellable *cancellable = g_cancellable_new();
  std::vector<Candidate> candidates;
  std::function<bool(CefRefPtr<CefX509Certificate>)> reply;
  ~ClientCertificate() {
    for (auto &candidate : candidates) g_clear_object(&candidate.certificate);
    g_clear_object(&cancellable);
    g_clear_object(&session);
  }
};
}

struct _EphyEngineClientCertificate { GObject parent; ClientCertificate *data; };
G_DEFINE_TYPE(EphyEngineClientCertificate, ephy_engine_client_certificate, G_TYPE_OBJECT)

static void ephy_engine_client_certificate_class_init(EphyEngineClientCertificateClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *data = EPHY_ENGINE_CLIENT_CERTIFICATE(object)->data;
    data->resolved = true;
    auto reply = std::move(data->reply);
    if (reply) reply(nullptr);
    g_cancellable_cancel(data->cancellable);
    delete data;
    G_OBJECT_CLASS(ephy_engine_client_certificate_parent_class)->finalize(object);
  };
  g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

static void ephy_engine_client_certificate_init(EphyEngineClientCertificate *self) {
  self->data = new ClientCertificate;
}

EphyEngineClientCertificate *ephy_engine_client_certificate_new(const char *host, int port, bool proxy,
  const std::vector<CefRefPtr<CefX509Certificate>> &certificates, EphyEngineSession *session,
  std::function<bool(CefRefPtr<CefX509Certificate>)> reply) {
  if (!EPHY_IS_ENGINE_SESSION(session) || !host || !*host || port < 1 || port > 65535) {
    if (reply) reply(nullptr);
    return nullptr;
  }
  auto *self = EPHY_ENGINE_CLIENT_CERTIFICATE(g_object_new(EPHY_TYPE_ENGINE_CLIENT_CERTIFICATE, nullptr));
  auto *data = self->data;
  data->host = host;
  data->port = port;
  data->proxy = proxy;
  data->session = EPHY_ENGINE_SESSION(g_object_ref(session));
  data->reply = std::move(reply);
  data->candidates.reserve(certificates.size());
  for (const auto &certificate : certificates) {
    Candidate candidate;
    candidate.native = certificate;
    if (certificate) {
      auto subject = certificate->GetSubject(), issuer = certificate->GetIssuer();
      if (subject) candidate.subject = subject->GetDisplayName();
      if (issuer) candidate.issuer = issuer->GetDisplayName();
      candidate.certificate = ephy_engine_certificate_to_tls(certificate);
    }
    data->candidates.push_back(std::move(candidate));
  }
  return self;
}

const char *ephy_engine_client_certificate_get_host(EphyEngineClientCertificate *self) { return self->data->host.c_str(); }
gint ephy_engine_client_certificate_get_port(EphyEngineClientCertificate *self) { return self->data->port; }
gboolean ephy_engine_client_certificate_is_proxy(EphyEngineClientCertificate *self) { return self->data->proxy; }
gboolean ephy_engine_client_certificate_is_ephemeral(EphyEngineClientCertificate *self) { return ephy_engine_session_is_ephemeral(self->data->session); }
gboolean ephy_engine_client_certificate_is_resolved(EphyEngineClientCertificate *self) { return self->data->resolved; }
guint ephy_engine_client_certificate_get_n_certificates(EphyEngineClientCertificate *self) { return self->data->candidates.size(); }
const char *ephy_engine_client_certificate_get_subject(EphyEngineClientCertificate *self, guint index) {
  return index < self->data->candidates.size() ? self->data->candidates[index].subject.c_str() : nullptr;
}
const char *ephy_engine_client_certificate_get_issuer(EphyEngineClientCertificate *self, guint index) {
  return index < self->data->candidates.size() ? self->data->candidates[index].issuer.c_str() : nullptr;
}
GTlsCertificate *ephy_engine_client_certificate_get_certificate(EphyEngineClientCertificate *self, guint index) {
  return index < self->data->candidates.size() ? self->data->candidates[index].certificate : nullptr;
}
GCancellable *ephy_engine_client_certificate_get_cancellable(EphyEngineClientCertificate *self) { return self->data->cancellable; }

namespace {
bool finish(EphyEngineClientCertificate *self, CefRefPtr<CefX509Certificate> certificate) {
  auto *data = self->data;
  if (data->resolved) return false;
  g_autoptr(EphyEngineClientCertificate) keep_alive = EPHY_ENGINE_CLIENT_CERTIFICATE(g_object_ref(self));
  data->resolved = true;
  auto reply = std::move(data->reply);
  const bool accepted = reply && reply(certificate);
  g_cancellable_cancel(data->cancellable);
  g_signal_emit_by_name(self, "finished");
  return certificate ? accepted : true;
}
}

gboolean ephy_engine_client_certificate_select(EphyEngineClientCertificate *self, guint index) {
  g_return_val_if_fail(EPHY_IS_ENGINE_CLIENT_CERTIFICATE(self), false);
  if (index >= self->data->candidates.size() || !self->data->candidates[index].native) return false;
  return finish(self, self->data->candidates[index].native);
}
gboolean ephy_engine_client_certificate_cancel(EphyEngineClientCertificate *self) {
  g_return_val_if_fail(EPHY_IS_ENGINE_CLIENT_CERTIFICATE(self), false);
  return finish(self, nullptr);
}
