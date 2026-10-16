/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "include/cef_browser.h"
#include "include/cef_parser.h"

// Both bridges share a CEF DevTools connection. ExecuteDevToolsMethod silently
// renumbers IDs below its previous maximum, which breaks independent observers.
inline int next_devtools_message_id() {
  static int sequence = 0;
  return ++sequence;
}
inline bool send_devtools_method(CefRefPtr<CefBrowser> browser, int id, const CefString& method,
                                CefRefPtr<CefDictionaryValue> params) {
  auto message = CefDictionaryValue::Create();
  message->SetInt("id", id);
  message->SetString("method", method);
  if (params) message->SetDictionary("params", params->Copy(false));
  auto value = CefValue::Create();
  value->SetDictionary(message);
  auto json = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
  return browser->GetHost()->SendDevToolsMessage(json.data(), json.size());
}
