/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2019-2020 Jan-Michael Brummer <jan.brummer@tabos.org>
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

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define EPHY_TYPE_WEB_EXTENSION (ephy_web_extension_get_type ())
G_DECLARE_FINAL_TYPE (EphyWebExtension, ephy_web_extension, EPHY, WEB_EXTENSION, GObject)

EphyWebExtension *ephy_web_extension_new_native (JsonObject *metadata);
gboolean ephy_web_extension_is_native (EphyWebExtension *self);
gboolean ephy_web_extension_native_is_enabled (EphyWebExtension *self);
gboolean ephy_web_extension_native_is_available (EphyWebExtension *self);
gboolean ephy_web_extension_native_is_pending (EphyWebExtension *self);
gboolean ephy_web_extension_native_is_restart_required (EphyWebExtension *self);
gboolean ephy_web_extension_native_is_builtin (EphyWebExtension *self);
void ephy_web_extension_native_set_state (EphyWebExtension *self, gboolean enabled, gboolean available, gboolean pending);
void ephy_web_extension_native_set_restart_required (EphyWebExtension *self, gboolean restart_required);
GdkPixbuf *ephy_web_extension_get_icon (EphyWebExtension *self, gint64 size);
const char *ephy_web_extension_get_name (EphyWebExtension *self);
const char *ephy_web_extension_get_short_name (EphyWebExtension *self);
const char *ephy_web_extension_get_version (EphyWebExtension *self);
const char *ephy_web_extension_get_description (EphyWebExtension *self);
const char *ephy_web_extension_get_homepage_url (EphyWebExtension *self);
const char *ephy_web_extension_get_author (EphyWebExtension *self);
const char *ephy_web_extension_get_guid (EphyWebExtension *self);
const char *ephy_web_extension_get_browser_popup (EphyWebExtension *self);
const char *ephy_web_extension_get_option_ui_page (EphyWebExtension *self);
gboolean ephy_web_extension_has_browser_action (EphyWebExtension *self);
char *ephy_web_extension_parse_command_key (const char *suggested_key);

G_END_DECLS
