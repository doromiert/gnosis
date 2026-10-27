/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define EPHY_TYPE_PREFS_WEB_APPS_PAGE (prefs_web_apps_page_get_type ())
G_DECLARE_FINAL_TYPE (PrefsWebAppsPage, prefs_web_apps_page, EPHY, PREFS_WEB_APPS_PAGE, AdwPreferencesPage)
G_END_DECLS
