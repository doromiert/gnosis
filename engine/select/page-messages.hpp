/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "include/cef_render_process_handler.h"
#include <string>
class Browser;
namespace PageMessages {
CefRefPtr<CefRenderProcessHandler> renderer_handler();
// Internal documents only; expected_uri is the actual captured frame URL.
bool send(Browser&, const std::string& expected_uri, const std::string& name, const std::string& json);
bool receive(Browser&, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage>);
}
