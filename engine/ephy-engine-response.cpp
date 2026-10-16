/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-response.h"

struct _EphyEngineResponse {
  GObject parent;
  char *uri, *mime, *filename;
  guint status;
  gint64 length;
  SoupMessageHeaders *headers;
};
G_DEFINE_TYPE(EphyEngineResponse, ephy_engine_response, G_TYPE_OBJECT)
static void ephy_engine_response_class_init(EphyEngineResponseClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *response = EPHY_ENGINE_RESPONSE(object);
    g_free(response->uri); g_free(response->mime); g_free(response->filename);
    soup_message_headers_unref(response->headers);
    G_OBJECT_CLASS(ephy_engine_response_parent_class)->finalize(object);
  };
}
static void ephy_engine_response_init(EphyEngineResponse *response) {
  response->headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
  response->length = -1;
}
EphyEngineResponse *ephy_engine_response_new(const std::string& uri, CefRefPtr<CefResponse> source) {
  auto *response = EPHY_ENGINE_RESPONSE(g_object_new(EPHY_TYPE_ENGINE_RESPONSE, nullptr));
  response->uri = g_strdup(uri.c_str());
  response->mime = g_strdup(source->GetMimeType().ToString().c_str());
  response->status = source->GetStatus();
  CefResponse::HeaderMap headers;
  source->GetHeaderMap(headers);
  for (const auto& [name, value] : headers)
    soup_message_headers_append(response->headers, name.ToString().c_str(), value.ToString().c_str());
  if (soup_message_headers_get_one(response->headers, "Content-Length"))
    response->length = soup_message_headers_get_content_length(response->headers);
  GHashTable *parameters = nullptr;
  if (soup_message_headers_get_content_disposition(response->headers, nullptr, &parameters)) {
    if (auto *filename = static_cast<const char*>(g_hash_table_lookup(parameters, "filename")); filename && *filename)
      response->filename = g_strdup(filename);
    g_hash_table_unref(parameters);
  }
  return response;
}
const char *ephy_engine_response_get_uri(EphyEngineResponse *r) { return r->uri; }
const char *ephy_engine_response_get_mime_type(EphyEngineResponse *r) { return r->mime; }
const char *ephy_engine_response_get_suggested_filename(EphyEngineResponse *r) { return r->filename; }
guint ephy_engine_response_get_status_code(EphyEngineResponse *r) { return r->status; }
gint64 ephy_engine_response_get_content_length(EphyEngineResponse *r) { return r->length; }
SoupMessageHeaders *ephy_engine_response_get_http_headers(EphyEngineResponse *r) { return r->headers; }
