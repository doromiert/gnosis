/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2015 Gustavo Noronha Silva <gns@gnome.org>
 *  Copyright © 2016-2017 Igalia S.L.
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

#include "config.h"

#include "ephy-permissions-manager.h"

#include <stdlib.h>
#include <string.h>

#define G_SETTINGS_ENABLE_BACKEND 1
#include <gio/gsettingsbackend.h>
#include "ephy-engine-origin.h"

#include "ephy-file-helpers.h"
#include "ephy-string.h"

struct _EphyPermissionsManager {
  GObject parent_instance;

  GHashTable *origins_mapping;
  GHashTable *settings_mapping;

  EphyEngineSession *session;
  gboolean prepared;

  GSettingsBackend *backend;
};

G_DEFINE_FINAL_TYPE (EphyPermissionsManager, ephy_permissions_manager, G_TYPE_OBJECT)

#define PERMISSIONS_FILENAME "permissions.ini"

static void
ephy_permissions_manager_init (EphyPermissionsManager *manager)
{
  manager->origins_mapping = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  manager->settings_mapping = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

}

static void
ephy_permissions_manager_dispose (GObject *object)
{
  EphyPermissionsManager *manager = EPHY_PERMISSIONS_MANAGER (object);

  g_clear_pointer (&manager->origins_mapping, g_hash_table_destroy);
  g_clear_pointer (&manager->settings_mapping, g_hash_table_destroy);

  g_clear_object (&manager->session);

  g_clear_object (&manager->backend);

  G_OBJECT_CLASS (ephy_permissions_manager_parent_class)->dispose (object);
}

static void
ephy_permissions_manager_class_init (EphyPermissionsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ephy_permissions_manager_dispose;
}

static GSettings *
ephy_permissions_manager_get_settings_for_origin (EphyPermissionsManager *manager,
                                                  const char             *origin)
{
  char *origin_path;
  char *trimmed_protocol;
  GSettings *settings;
  EphyEngineOrigin *security_origin;
  char *pos;

  g_assert (origin);

  settings = g_hash_table_lookup (manager->origins_mapping, origin);
  if (settings)
    return settings;

  /* Cannot contain consecutive slashes in GSettings path... */
  security_origin = ephy_engine_origin_new_for_uri (origin);
  if (!security_origin)
    return NULL;
  trimmed_protocol = g_strdup (ephy_engine_origin_get_protocol (security_origin));
  pos = strchr (trimmed_protocol, '/');
  if (pos)
    *pos = '\0';

  origin_path = g_strdup_printf ("/org/gnome/epiphany/permissions/%s/%s/%u/",
                                 trimmed_protocol,
                                 ephy_engine_origin_get_host (security_origin),
                                 ephy_engine_origin_get_port (security_origin));

  settings = g_settings_new_with_backend_and_path ("io.github.doromiert.Jupiter.permissions", manager->backend, origin_path);
  g_free (trimmed_protocol);
  g_free (origin_path);
  ephy_engine_origin_unref (security_origin);

  /* Note that settings is owned only by the first hash table! */
  g_hash_table_insert (manager->origins_mapping, g_strdup (origin), settings);
  g_hash_table_insert (manager->settings_mapping, settings, g_strdup (origin));

  return settings;
}

static gboolean
native_permission_kind (EphyPermissionType type, EphyEnginePermissionKind *kind)
{
  switch (type) {
    case EPHY_PERMISSION_TYPE_SHOW_NOTIFICATIONS: *kind = EPHY_ENGINE_PERMISSION_NOTIFICATIONS; return TRUE;
    case EPHY_PERMISSION_TYPE_ACCESS_LOCATION: *kind = EPHY_ENGINE_PERMISSION_LOCATION; return TRUE;
    case EPHY_PERMISSION_TYPE_ACCESS_MICROPHONE: *kind = EPHY_ENGINE_PERMISSION_MICROPHONE; return TRUE;
    case EPHY_PERMISSION_TYPE_ACCESS_WEBCAM: *kind = EPHY_ENGINE_PERMISSION_CAMERA; return TRUE;
    default: return FALSE;
  }
}

EphyPermissionsManager *
ephy_permissions_manager_new (EphyEngineSession *session)
{
  EphyPermissionsManager *manager = g_object_new (EPHY_TYPE_PERMISSIONS_MANAGER, NULL);
  g_autofree char *filename = NULL;

  if (session)
    manager->session = g_object_ref (session);
  if (session && ephy_engine_session_is_ephemeral (session)) {
    manager->backend = g_memory_settings_backend_new ();
  } else {
    filename = g_build_filename (ephy_profile_dir (), PERMISSIONS_FILENAME, NULL);
    manager->backend = g_keyfile_settings_backend_new (filename, "/", NULL);
  }
  return manager;
}

static const char *
permission_type_to_string (EphyPermissionType type)
{
  g_assert (ephy_permission_is_stored_by_permissions_manager (type));

  switch (type) {
    case EPHY_PERMISSION_TYPE_SHOW_NOTIFICATIONS:
      return "notifications-permission";
    case EPHY_PERMISSION_TYPE_SAVE_PASSWORD:
      return "save-password-permission";
    case EPHY_PERMISSION_TYPE_ACCESS_LOCATION:
      return "geolocation-permission";
    case EPHY_PERMISSION_TYPE_ACCESS_MICROPHONE:
      return "audio-device-permission";
    case EPHY_PERMISSION_TYPE_ACCESS_WEBCAM:
      return "video-device-permission";
    case EPHY_PERMISSION_TYPE_SHOW_ADS:
      return "advertisement-permission";
    case EPHY_PERMISSION_TYPE_AUTOPLAY_POLICY:
      return "autoplay-permission";
    case EPHY_PERMISSION_TYPE_ACCESS_DISPLAY:
      return "display-device-permission";
    default:
      g_assert_not_reached ();
  }
}

/* Some permissions do not get remembered by the permissions manager. Generally,
 * Epiphany prompts every time such permission is requested. But
 * ACCESS_WEBCAM_AND_MICROPHONE is special: only separate ACCESS_MICROPHONE and
 * ACCESS_WEBCAM permissions get stored.
 */
gboolean
ephy_permission_is_stored_by_permissions_manager (EphyPermissionType type)
{
  switch (type) {
    case EPHY_PERMISSION_TYPE_ACCESS_WEBCAM_AND_MICROPHONE:
    /* fallthrough */
    case EPHY_PERMISSION_TYPE_WEBSITE_DATA_ACCESS:
    /* fallthrough */
    case EPHY_PERMISSION_TYPE_CLIPBOARD:
    case EPHY_PERMISSION_TYPE_OTHER:
      return FALSE;
    default:
      return TRUE;
  }
}

EphyPermission
ephy_permissions_manager_get_permission (EphyPermissionsManager *manager,
                                         EphyPermissionType      type,
                                         const char             *origin)
{
  EphyEnginePermissionKind kind;
  GSettings *settings;

  g_assert (ephy_permission_is_stored_by_permissions_manager (type));
  if (manager->session && native_permission_kind (type, &kind)) {
    ephy_permissions_manager_prepare_session (manager);
    return (EphyPermission)ephy_engine_session_get_permission (manager->session, kind, origin, NULL);
  }
  settings = ephy_permissions_manager_get_settings_for_origin (manager, origin);
  return settings ? g_settings_get_enum (settings, permission_type_to_string (type)) : EPHY_PERMISSION_UNDECIDED;
}

void
ephy_permissions_manager_set_permission (EphyPermissionsManager *manager,
                                         EphyPermissionType      type,
                                         const char             *origin,
                                         EphyPermission          permission)
{
  EphyEnginePermissionKind kind;
  GSettings *settings;

  g_assert (ephy_permission_is_stored_by_permissions_manager (type));
  if (manager->session && native_permission_kind (type, &kind)) {
    ephy_permissions_manager_prepare_session (manager);
    ephy_engine_session_set_permission (manager->session, kind, origin, NULL, (EphyEnginePermissionState)permission);
    return;
  }
  settings = ephy_permissions_manager_get_settings_for_origin (manager, origin);
  if (settings)
    g_settings_set_enum (settings, permission_type_to_string (type), permission);
}

static EphyEngineOrigin *
group_name_to_security_origin (const char *group)
{
  char **tokens;
  EphyEngineOrigin *origin = NULL;

  /* Should be of form org/gnome/epiphany/permissions/http/example.com/0 */
  tokens = g_strsplit (group, "/", -1);
  if (g_strv_length (tokens) == 7 && tokens[4] && tokens[5] && tokens[6]) {
    guint64 port;
    if (g_ascii_string_to_unsigned (tokens[6], 10, 0, G_MAXUINT16, &port, NULL))
      origin = ephy_engine_origin_new (tokens[4], tokens[5], port);
  }

  g_strfreev (tokens);

  return origin;
}

void
ephy_permissions_manager_prepare_session (EphyPermissionsManager *manager)
{
  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *filename = NULL;
  g_auto (GStrv) groups = NULL;
  g_autoptr (GArray) records = g_array_new (FALSE, FALSE, sizeof (EphyEnginePermissionRecord));
  g_autoptr (GPtrArray) origins = g_ptr_array_new_with_free_func (g_free);
  const EphyPermissionType types[] = { EPHY_PERMISSION_TYPE_SHOW_NOTIFICATIONS, EPHY_PERMISSION_TYPE_ACCESS_LOCATION,
                                      EPHY_PERMISSION_TYPE_ACCESS_MICROPHONE, EPHY_PERMISSION_TYPE_ACCESS_WEBCAM };

  if (manager->prepared || !manager->session)
    return;
  if (ephy_engine_session_is_ephemeral (manager->session)) {
    manager->prepared = TRUE;
    return;
  }
  filename = g_build_filename (ephy_profile_dir (), PERMISSIONS_FILENAME, NULL);
  if (!g_key_file_load_from_file (file, filename, G_KEY_FILE_NONE, &error) &&
      !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_warning ("Could not import saved permissions: %s", error->message);
    return;
  }
  groups = g_key_file_get_groups (file, NULL);
  for (guint i = 0; groups && groups[i]; i++) {
    g_autoptr (EphyEngineOrigin) origin = group_name_to_security_origin (groups[i]);
    char *uri;

    if (!origin)
      continue;
    uri = ephy_engine_origin_to_string (origin);
    g_ptr_array_add (origins, uri);
    for (guint j = 0; j < G_N_ELEMENTS (types); j++) {
      EphyEnginePermissionRecord record = { .origin = uri };
      g_autofree char *value = g_key_file_get_string (file, groups[i], permission_type_to_string (types[j]), NULL);

      if (g_strcmp0 (value, "'allow'") == 0)
        record.state = EPHY_ENGINE_PERMISSION_STATE_ALLOW;
      else if (g_strcmp0 (value, "'deny'") == 0)
        record.state = EPHY_ENGINE_PERMISSION_STATE_DENY;
      else if (g_strcmp0 (value, "'undecided'") == 0)
        record.state = EPHY_ENGINE_PERMISSION_STATE_ASK;
      else
        continue;
      native_permission_kind (types[j], &record.kind);
      g_array_append_val (records, record);
    }
  }
  ephy_engine_session_import_permissions (manager->session, (EphyEnginePermissionRecord *)records->data, records->len);
  manager->prepared = TRUE;
}
