/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
class Browser;
namespace WebstoreGuard {
// Temporary containment for CEF's unsupported browser-modal install prompt.
bool install(CefRefPtr<CefFrame>, CefRefPtr<CefV8Context>);
bool receive(Browser&, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage>);
}
