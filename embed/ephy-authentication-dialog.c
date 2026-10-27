/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2026 The Gnosis Developers
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

#include "ephy-authentication-dialog.h"

#include <glib/gi18n.h>
#include <string.h>

static char *
format_recipient (const char *host,
                  guint16     port)
{
  gsize host_length = strlen (host);
  gboolean is_bracketed = host_length > 1 && host[0] == '[' && host[host_length - 1] == ']';

  if (strchr (host, ':') && !is_bracketed)
    return g_strdup_printf ("[%s]:%u", host, port);

  return g_strdup_printf ("%s:%u", host, port);
}

AdwAlertDialog *
ephy_authentication_dialog_new (const char           *host,
                                guint16               port,
                                const char           *realm,
                                gboolean              is_proxy,
                                AdwEntryRow         **username_row,
                                AdwPasswordEntryRow **password_row)
{
  g_autofree char *recipient = NULL;
  g_autofree char *body = NULL;
  AdwAlertDialog *dialog;
  AdwPreferencesGroup *group;
  GtkWidget *username;
  GtkWidget *password;

  g_return_val_if_fail (host && *host, NULL);
  g_return_val_if_fail (username_row, NULL);
  g_return_val_if_fail (password_row, NULL);

  recipient = format_recipient (host, port);

  if (is_proxy) {
    if (realm && *realm)
      body = g_strdup_printf (_("The proxy %s is requesting a username and password for “%s”."), recipient, realm);
    else
      body = g_strdup_printf (_("The proxy %s is requesting a username and password."), recipient);
  } else {
    if (realm && *realm)
      body = g_strdup_printf (_("The server %s is requesting a username and password for “%s”."), recipient, realm);
    else
      body = g_strdup_printf (_("The server %s is requesting a username and password."), recipient);
  }

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Authentication Required"), NULL));
  adw_alert_dialog_set_body (dialog, body);
  adw_alert_dialog_set_body_use_markup (dialog, FALSE);
  adw_alert_dialog_add_responses (dialog,
                                  "cancel", _("_Cancel"),
                                  "login", _("_Log In"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (dialog, "login", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "login");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  username = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (username), _("Username"));
  gtk_text_set_activates_default (GTK_TEXT (gtk_editable_get_delegate (GTK_EDITABLE (username))), TRUE);
  adw_preferences_group_add (group, username);

  password = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (password), _("Password"));
  gtk_text_set_activates_default (GTK_TEXT (gtk_editable_get_delegate (GTK_EDITABLE (password))), TRUE);
  adw_preferences_group_add (group, password);

  adw_alert_dialog_set_extra_child (dialog, GTK_WIDGET (group));
  adw_dialog_set_focus (ADW_DIALOG (dialog), username);

  *username_row = ADW_ENTRY_ROW (g_object_ref (username));
  *password_row = ADW_PASSWORD_ENTRY_ROW (g_object_ref (password));

  return dialog;
}
