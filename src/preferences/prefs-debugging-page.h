/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define EPHY_TYPE_PREFS_DEBUGGING_PAGE (prefs_debugging_page_get_type ())
G_DECLARE_FINAL_TYPE (PrefsDebuggingPage, prefs_debugging_page, EPHY, PREFS_DEBUGGING_PAGE, AdwPreferencesPage)
G_END_DECLS
