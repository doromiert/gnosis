/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2015 Gustavo Noronha Silva <gns@gnome.org>
 *  Copyright © 2016 Igalia S.L.
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

#include <glib-object.h>
#include "ephy-engine-session.h"

G_BEGIN_DECLS

#define EPHY_TYPE_PERMISSIONS_MANAGER (ephy_permissions_manager_get_type ())

G_DECLARE_FINAL_TYPE (EphyPermissionsManager, ephy_permissions_manager, EPHY, PERMISSIONS_MANAGER, GObject)

typedef enum {
  EPHY_PERMISSION_UNDECIDED = -1,
  EPHY_PERMISSION_DENY = 0,
  EPHY_PERMISSION_PERMIT = 1
} EphyPermission;

typedef enum {
  EPHY_PERMISSION_TYPE_SHOW_NOTIFICATIONS,
  EPHY_PERMISSION_TYPE_SAVE_PASSWORD,
  EPHY_PERMISSION_TYPE_ACCESS_LOCATION,
  EPHY_PERMISSION_TYPE_ACCESS_MICROPHONE,
  EPHY_PERMISSION_TYPE_ACCESS_WEBCAM,
  EPHY_PERMISSION_TYPE_SHOW_ADS,
  EPHY_PERMISSION_TYPE_AUTOPLAY_POLICY,
  EPHY_PERMISSION_TYPE_ACCESS_WEBCAM_AND_MICROPHONE,
  EPHY_PERMISSION_TYPE_WEBSITE_DATA_ACCESS,
  EPHY_PERMISSION_TYPE_CLIPBOARD,
  EPHY_PERMISSION_TYPE_ACCESS_DISPLAY,
  EPHY_PERMISSION_TYPE_OTHER
} EphyPermissionType;

EphyPermissionsManager *ephy_permissions_manager_new            (EphyEngineSession *session);
void                    ephy_permissions_manager_prepare_session (EphyPermissionsManager *manager);

EphyPermission          ephy_permissions_manager_get_permission (EphyPermissionsManager *manager,
                                                                 EphyPermissionType      type,
                                                                 const char             *origin);
void                    ephy_permissions_manager_set_permission (EphyPermissionsManager *manager,
                                                                 EphyPermissionType      type,
                                                                 const char             *origin,
                                                                 EphyPermission          permission);



gboolean                ephy_permission_is_stored_by_permissions_manager (EphyPermissionType type);

G_END_DECLS
