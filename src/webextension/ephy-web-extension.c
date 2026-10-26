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

/* Native Chromium extension metadata for the GTK catalog. No second extension
 * execution environment or JavaScriptCore API provider is instantiated here. */
#include "config.h"
#include "ephy-web-extension.h"
#include "ephy-json-utils.h"
#include <string.h>

struct _EphyWebExtension {
  GObject parent_instance;
  JsonObject *native_metadata;
  gboolean native_pending;
};

G_DEFINE_FINAL_TYPE (EphyWebExtension, ephy_web_extension, G_TYPE_OBJECT)

static void
ephy_web_extension_dispose (GObject *object)
{
  EphyWebExtension *self = EPHY_WEB_EXTENSION (object);

  g_clear_pointer (&self->native_metadata, json_object_unref);
  G_OBJECT_CLASS (ephy_web_extension_parent_class)->dispose (object);
}

static void
ephy_web_extension_class_init (EphyWebExtensionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ephy_web_extension_dispose;
  g_signal_new ("native-state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ephy_web_extension_init (EphyWebExtension *self)
{
}

EphyWebExtension *
ephy_web_extension_new_native (JsonObject *metadata)
{
  EphyWebExtension *self;

  g_return_val_if_fail (metadata != NULL, NULL);
  self = g_object_new (EPHY_TYPE_WEB_EXTENSION, NULL);
  self->native_metadata = json_object_ref (metadata);
  return self;
}

GdkPixbuf *
ephy_web_extension_get_icon (EphyWebExtension *self, gint64 size)
{
  const char *path = ephy_json_object_get_string (self->native_metadata, "icon");

  return path && *path ? gdk_pixbuf_new_from_file_at_scale (path, size, size, TRUE, NULL) : NULL;
}

const char *
ephy_web_extension_get_name (EphyWebExtension *self)
{
  return ephy_json_object_get_string (self->native_metadata, "name");
}

const char *
ephy_web_extension_get_short_name (EphyWebExtension *self)
{
  return ephy_web_extension_get_name (self);
}

const char *
ephy_web_extension_get_version (EphyWebExtension *self)
{
  return ephy_json_object_get_string (self->native_metadata, "version");
}

const char *
ephy_web_extension_get_description (EphyWebExtension *self)
{
  return json_object_get_string_member_with_default (self->native_metadata, "description", "");
}

const char *
ephy_web_extension_get_homepage_url (EphyWebExtension *self)
{
  return ephy_json_object_get_string (self->native_metadata, "homepage");
}

const char *
ephy_web_extension_get_author (EphyWebExtension *self)
{
  return ephy_json_object_get_string (self->native_metadata, "author");
}

const char *
ephy_web_extension_get_browser_popup (EphyWebExtension *self)
{
  const char *popup = ephy_json_object_get_string (self->native_metadata, "popup");

  return popup && *popup ? popup : NULL;
}

gboolean
ephy_web_extension_has_browser_action (EphyWebExtension *self)
{
  return json_object_get_boolean_member_with_default (self->native_metadata, "has_action", FALSE) ||
         ephy_web_extension_get_browser_popup (self) != NULL;
}

const char *
ephy_web_extension_get_option_ui_page (EphyWebExtension *self)
{
  const char *options = ephy_json_object_get_string (self->native_metadata, "options");

  return options && *options ? options : NULL;
}

const char *
ephy_web_extension_get_guid (EphyWebExtension *self)
{
  return ephy_json_object_get_string (self->native_metadata, "id");
}

gboolean
ephy_web_extension_is_native (EphyWebExtension *self)
{
  return self->native_metadata != NULL;
}

gboolean
ephy_web_extension_native_is_enabled (EphyWebExtension *self)
{
  return self->native_metadata && json_object_get_boolean_member_with_default (self->native_metadata, "enabled", FALSE);
}

gboolean
ephy_web_extension_native_is_available (EphyWebExtension *self)
{
  return self->native_metadata && json_object_get_boolean_member_with_default (self->native_metadata, "available", FALSE);
}

gboolean
ephy_web_extension_native_is_pending (EphyWebExtension *self)
{
  return self->native_pending;
}

gboolean
ephy_web_extension_native_is_restart_required (EphyWebExtension *self)
{
  return self->native_metadata &&
         json_object_get_boolean_member_with_default (self->native_metadata, "restart_required", FALSE);
}

void
ephy_web_extension_native_update_metadata (EphyWebExtension *self,
                                           JsonObject       *metadata)
{
  g_return_if_fail (self->native_metadata != NULL);
  json_object_unref (self->native_metadata);
  self->native_metadata = json_object_ref (metadata);
  g_signal_emit_by_name (self, "native-state-changed");
}

void
ephy_web_extension_native_set_state (EphyWebExtension *self,
                                     gboolean enabled,
                                     gboolean available,
                                     gboolean pending)
{
  g_return_if_fail (self->native_metadata != NULL);
  if (ephy_web_extension_native_is_enabled (self) == enabled &&
      ephy_web_extension_native_is_available (self) == available &&
      self->native_pending == pending)
    return;
  json_object_set_boolean_member (self->native_metadata, "enabled", enabled);
  json_object_set_boolean_member (self->native_metadata, "available", available);
  self->native_pending = pending;
  g_signal_emit_by_name (self, "native-state-changed");
}

void
ephy_web_extension_native_set_restart_required (EphyWebExtension *self,
                                                 gboolean          restart_required)
{
  g_return_if_fail (self->native_metadata != NULL);
  if (ephy_web_extension_native_is_restart_required (self) == restart_required)
    return;
  json_object_set_boolean_member (self->native_metadata, "restart_required", restart_required);
  g_signal_emit_by_name (self, "native-state-changed");
}

gboolean
ephy_web_extension_native_is_builtin (EphyWebExtension *self)
{
  return self->native_metadata && json_object_get_boolean_member_with_default (self->native_metadata, "builtin", FALSE);
}

static gboolean
is_valid_key (const char *key)
{
  static const char * const allowed_keys[] = { "Comma", "Period", "Home", "End", "PageUp", "PageDown", "Space", "Insert", "Delete", "Up", "Down", "Left", "Right", NULL};

  if (strlen (key) == 1 && ((*key >= 'A' && *key <= 'Z') || (*key >= '0' && *key <= '9')))
    return TRUE;

  if (strlen (key) == 2 && key[0] == 'F' && (key[1] >= '0' && key[1] <= '9'))
    return TRUE;

  return g_strv_contains (allowed_keys, key);
}

/*
 * ephy_web_extension_parse_command_key:
 * @key: A command key
 *
 * This parses a key in the format specified on [MDN](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/commands).
 *
 * It returns a string in Gtk's accelerator format.
 *
 * Returns: (transfer full): An accelerator or %NULL if invalid
 */
char *
ephy_web_extension_parse_command_key (const char *suggested_key)
{
  g_autoptr (GString) accelerator = g_string_sized_new (strlen (suggested_key) + 5);
  g_auto (GStrv) keys = NULL;
  guint i;
  gboolean has_modifier = FALSE;
  gboolean has_key = FALSE;

  if (strcmp (suggested_key, "MediaNextTrack") == 0)
    return g_strdup ("XF86AudioNext");
  else if (strcmp (suggested_key, "MediaPlayPause") == 0)
    return g_strdup ("XF86AudioPlay");
  else if (strcmp (suggested_key, "MediaPrevTrack") == 0)
    return g_strdup ("XF86AudioPrev");
  else if (strcmp (suggested_key, "MediaStop") == 0)
    return g_strdup ("XF86AudioStop");

  keys = g_strsplit (suggested_key, "+", 3);
  for (i = 0; keys[i]; i++) {
    const char *key = keys[i];
    /* First two are potentially modifiers. We should check for duplicates but its probably harmless. */
    if (i == 0 || i == 1) {
      if (strcmp (key, "Ctrl") == 0 || strcmp (key, "Alt") == 0 || (i == 1 && strcmp (key, "Shift") == 0)) {
        g_string_append_printf (accelerator, "<%s>", key);
        has_modifier = TRUE;
        continue;
      } else if (strcmp (key, "Command") == 0 || strcmp (key, "MacCtrl") == 0) {
        g_string_append (accelerator, "<Ctrl>");
        has_modifier = TRUE;
        continue;
      } else if (i == 0 || !is_valid_key (key)) {
        g_debug ("Invalid modifier at index %u: %s", i, key);
        return NULL;
      }
    }

    /* Second two are potentially keys. */
    if (i == 1 || i == 2) {
      if (has_key) {
        g_debug ("Command key has two keys: %s", suggested_key);
        return NULL;
      }
      if (!is_valid_key (key)) {
        g_debug ("Command key has invalid_key: %s", key);
        return NULL;
      }
      if (strcmp (key, "Space") == 0)
        g_string_append (accelerator, "space");
      else if (strcmp (key, "Period") == 0)
        g_string_append (accelerator, "period");
      else if (strcmp (key, "Comma") == 0)
        g_string_append (accelerator, "comma");
      else if (strcmp (key, "PageUp") == 0)
        g_string_append (accelerator, "Page_Up");
      else if (strcmp (key, "PageDown") == 0)
        g_string_append (accelerator, "Page_Down");
      else
        g_string_append (accelerator, key);
      has_key = TRUE;
    }
  }

  if (!has_modifier && !has_key) {
    g_debug ("Command key requires a modifier and a key: %s", suggested_key);
    return NULL;
  }

  if (!gtk_accelerator_parse (accelerator->str, NULL, NULL)) {
    g_warning ("Transformed WebExtensions accelerator %s into %s, but this is not a valid GTK accelerator", suggested_key, accelerator->str);
    return NULL;
  }

  return g_steal_pointer (&accelerator->str);
}
