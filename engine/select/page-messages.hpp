/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "include/cef_render_process_handler.h"
class Browser;
namespace PageMessages {
CefRefPtr<CefRenderProcessHandler> renderer_handler();
bool receive(Browser&, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage>);
}
