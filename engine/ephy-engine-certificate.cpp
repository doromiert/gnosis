/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-certificate.hpp"
#include "include/cef_parser.h"
#include <limits>
#include <string>

GTlsCertificate *ephy_engine_certificate_to_tls(CefRefPtr<CefX509Certificate> certificate) {
  if (!certificate) return nullptr;
  auto leaf = certificate->GetPEMEncoded();
  if (!leaf || !leaf->GetSize()) return nullptr;
  std::string pem;
  auto append = [&](CefRefPtr<CefBinaryValue> bytes) {
    if (!bytes || !bytes->GetSize()) return;
    auto offset = pem.size();
    pem.resize(offset + bytes->GetSize());
    bytes->GetData(pem.data() + offset, bytes->GetSize(), 0);
    pem += '\n';
  };
  append(leaf);
  CefX509Certificate::IssuerChainBinaryList chain;
  certificate->GetPEMEncodedIssuerChain(chain);
  for (const auto& issuer : chain) append(issuer);
  return g_tls_certificate_new_from_pem(pem.data(), pem.size(), nullptr);
}

GTlsCertificateFlags ephy_engine_certificate_errors(cef_cert_status_t status,
                                                   CefRefPtr<CefX509Certificate> certificate) {
  // CEF reserves the high 16 bits for informational statuses, including EV.
  const auto errors = static_cast<unsigned>(status) & 0xffff;
  unsigned result = 0;
  if (errors & (CERT_STATUS_COMMON_NAME_INVALID | CERT_STATUS_NON_UNIQUE_NAME))
    result |= G_TLS_CERTIFICATE_BAD_IDENTITY;
  if (errors & CERT_STATUS_AUTHORITY_INVALID) result |= G_TLS_CERTIFICATE_UNKNOWN_CA;
  if (errors & CERT_STATUS_REVOKED) result |= G_TLS_CERTIFICATE_REVOKED;
  if (errors & (CERT_STATUS_WEAK_SIGNATURE_ALGORITHM | CERT_STATUS_WEAK_KEY))
    result |= G_TLS_CERTIFICATE_INSECURE;
  if (errors & CERT_STATUS_DATE_INVALID) {
    const auto now = CefBaseTime::Now().val;
    const auto start = certificate ? certificate->GetValidStart().val : 0;
    const auto expiry = certificate ? certificate->GetValidExpiry().val : 0;
    if (start && start > now) result |= G_TLS_CERTIFICATE_NOT_ACTIVATED;
    if (expiry && expiry < now) result |= G_TLS_CERTIFICATE_EXPIRED;
    if ((!start || start <= now) && (!expiry || expiry >= now))
      result |= G_TLS_CERTIFICATE_GENERIC_ERROR;
  }
  constexpr auto mapped = CERT_STATUS_COMMON_NAME_INVALID | CERT_STATUS_NON_UNIQUE_NAME |
    CERT_STATUS_AUTHORITY_INVALID | CERT_STATUS_REVOKED | CERT_STATUS_WEAK_SIGNATURE_ALGORITHM |
    CERT_STATUS_WEAK_KEY | CERT_STATUS_DATE_INVALID;
  if (errors & ~mapped) result |= G_TLS_CERTIFICATE_GENERIC_ERROR;
  return static_cast<GTlsCertificateFlags>(result);
}

bool ephy_engine_certificate_reset_cached_decisions(CefRefPtr<CefRequestContext> context, const CefString& uri, bool ephemeral) {
  if (!context) return false;
  CefURLParts parts;
  if (!CefParseURL(uri, parts) || CefString(&parts.scheme) != "https") return false;
  auto host = CefString(&parts.host).ToString();
  if (host.empty()) return false;
  g_autofree char *scope = g_uri_join(G_URI_FLAGS_NONE, "https", nullptr, host.c_str(), -1, "/", nullptr, nullptr);
  if (!scope) return false;
  // Chromium 150 inherits regular SSL decisions in private contexts. A valid empty
  // policy masks that inheritance so our exact certificate/session check runs.
  auto dictionary = CefDictionaryValue::Create();
  dictionary->SetInt("version", 1);
  const auto expiry = ephemeral ? std::numeric_limits<int64_t>::max() : CefBaseTime::Now().val + 604800LL * 1000000;
  dictionary->SetString("decision_expiration_time", std::to_string(expiry));
  dictionary->SetDictionary("cert_exceptions_map", CefDictionaryValue::Create());
  auto value = CefValue::Create();
  value->SetDictionary(dictionary);
  context->SetWebsiteSetting(scope, "", CEF_CONTENT_SETTING_TYPE_SSL_CERT_DECISIONS, value);
  auto configured = context->GetWebsiteSetting(scope, scope, CEF_CONTENT_SETTING_TYPE_SSL_CERT_DECISIONS);
  return configured && configured->IsEqual(value);
}
