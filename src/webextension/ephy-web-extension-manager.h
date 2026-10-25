/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2019-2026 Jan-Michael Brummer <jan.brummer@tabos.org>
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */


#pragma once

#include "ephy-web-extension.h"
#include "ephy-window.h"
#include "ephy-web-view.h"

G_BEGIN_DECLS

#define EPHY_TYPE_WEB_EXTENSION_MANAGER (ephy_web_extension_manager_get_type ())
G_DECLARE_FINAL_TYPE (EphyWebExtensionManager, ephy_web_extension_manager, EPHY, WEB_EXTENSION_MANAGER, GObject)

EphyWebExtensionManager *ephy_web_extension_manager_get_default (void);
void ephy_web_extension_manager_set_native_enabled_async (EphyWebExtensionManager *self, EphyWebExtension *extension, gboolean enabled, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
gboolean ephy_web_extension_manager_set_native_enabled_finish (EphyWebExtensionManager *self, GAsyncResult *result, GError **error);
void ephy_web_extension_manager_refresh_native_installed (EphyWebExtensionManager *self);
GPtrArray *ephy_web_extension_manager_get_web_extensions (EphyWebExtensionManager *self);
gboolean ephy_web_extension_manager_is_active (EphyWebExtensionManager *self, EphyWebExtension *extension);
GListStore *ephy_web_extension_manager_get_browser_actions (EphyWebExtensionManager *self);
GtkWidget *ephy_web_extension_manager_create_browser_popup (EphyWebExtensionManager *self, EphyWebExtension *extension, EphyWebView *owner);
void ephy_web_extension_manager_open_options (EphyWebExtensionManager *self, EphyWebExtension *extension, EphyWindow *window);

G_END_DECLS
