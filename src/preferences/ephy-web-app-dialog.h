/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <adwaita.h>
#include "ephy-web-app-utils.h"
G_BEGIN_DECLS
#define EPHY_TYPE_WEB_APP_DIALOG (ephy_web_app_dialog_get_type ())
G_DECLARE_FINAL_TYPE (EphyWebAppDialog, ephy_web_app_dialog, EPHY, WEB_APP_DIALOG, AdwPreferencesDialog)
GtkWidget *ephy_web_app_dialog_new (const char *name, const char *url, GIcon *icon, GKeyFile *config, gboolean installing);
const char *ephy_web_app_dialog_get_name (EphyWebAppDialog *self);
const char *ephy_web_app_dialog_get_url (EphyWebAppDialog *self);
GIcon *ephy_web_app_dialog_get_icon (EphyWebAppDialog *self);
GBytes *ephy_web_app_dialog_dup_icon_bytes (EphyWebAppDialog *self, GError **error);
GKeyFile *ephy_web_app_dialog_dup_config (EphyWebAppDialog *self);
gboolean ephy_web_app_dialog_apply (EphyWebAppDialog *self, EphyWebApplication *app, gboolean installing, GError **error);
G_END_DECLS
