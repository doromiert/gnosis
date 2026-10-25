/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"

#include "ephy-client-certificate-dialog.h"

#include <glib/gi18n.h>
#include <string.h>

typedef struct {
  AdwAlertDialog *dialog;
  guint index;
} CertificateChoice;

static char *
format_recipient (const char *host,
                  gint        port)
{
  gsize host_length = strlen (host);
  gboolean is_bracketed = host_length > 1 && host[0] == '[' && host[host_length - 1] == ']';

  if (strchr (host, ':') && !is_bracketed)
    return g_strdup_printf ("[%s]:%d", host, port);

  return g_strdup_printf ("%s:%d", host, port);
}

static void
certificate_choice_free (gpointer  data,
                         GClosure *closure)
{
  (void)closure;
  g_free (data);
}

static void
certificate_toggled_cb (GtkCheckButton   *button,
                        CertificateChoice *choice)
{
  if (!gtk_check_button_get_active (button))
    return;

  g_object_set_data (G_OBJECT (choice->dialog),
                     "ephy-client-certificate-index",
                     GUINT_TO_POINTER (choice->index + 1));
  adw_alert_dialog_set_response_enabled (choice->dialog, "select", TRUE);
}

static GtkWidget *
create_certificate_row (EphyEngineClientCertificate *request,
                        AdwAlertDialog              *dialog,
                        guint                        index,
                        GtkCheckButton             *group)
{
  const char *subject = ephy_engine_client_certificate_get_subject (request, index);
  const char *issuer = ephy_engine_client_certificate_get_issuer (request, index);
  GTlsCertificate *certificate = ephy_engine_client_certificate_get_certificate (request, index);
  CertificateChoice *choice = g_new0 (CertificateChoice, 1);
  g_autofree char *issuer_label = NULL;
  GtkWidget *row;
  GtkWidget *button;

  row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 subject && *subject ? subject : _("Unnamed Certificate"));
  adw_action_row_set_title_lines (ADW_ACTION_ROW (row), 2);

  if (issuer && *issuer)
    issuer_label = g_strdup_printf (_("Issued by %s"), issuer);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                               issuer_label ? issuer_label : _("Issuer Unknown"));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);

  button = gtk_check_button_new ();
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_check_button_set_group (GTK_CHECK_BUTTON (button), group);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);

  choice->dialog = dialog;
  choice->index = index;
  g_signal_connect_data (button, "toggled",
                         G_CALLBACK (certificate_toggled_cb), choice,
                         certificate_choice_free, 0);

  gtk_widget_set_sensitive (row, certificate != NULL);
  return row;
}

AdwAlertDialog *
ephy_client_certificate_dialog_new (EphyEngineClientCertificate *request)
{
  g_autofree char *recipient = NULL;
  g_autofree char *body = NULL;
  AdwAlertDialog *dialog;
  GtkWidget *listbox = NULL;
  GtkCheckButton *group = NULL;
  guint n_certificates;
  guint n_selectable = 0;

  g_return_val_if_fail (EPHY_IS_ENGINE_CLIENT_CERTIFICATE (request), NULL);
  g_return_val_if_fail (ephy_engine_client_certificate_get_host (request) != NULL, NULL);

  recipient = format_recipient (ephy_engine_client_certificate_get_host (request),
                                ephy_engine_client_certificate_get_port (request));
  n_certificates = ephy_engine_client_certificate_get_n_certificates (request);

  if (n_certificates == 0) {
    if (ephy_engine_client_certificate_is_proxy (request))
      body = g_strdup_printf (_("No matching certificates are available for the proxy %s."), recipient);
    else
      body = g_strdup_printf (_("No matching certificates are available for the server %s."), recipient);
  } else if (ephy_engine_client_certificate_is_proxy (request)) {
    body = g_strdup_printf (_("The proxy %s is requesting a certificate for authentication."), recipient);
  } else {
    body = g_strdup_printf (_("The server %s is requesting a certificate for authentication."), recipient);
  }

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Select Certificate"), body));
  adw_alert_dialog_set_body_use_markup (dialog, FALSE);
  gtk_widget_set_size_request (GTK_WIDGET (dialog), 360, -1);

  if (n_certificates == 0) {
    adw_alert_dialog_add_response (dialog, "cancel", _("_Close"));
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    return dialog;
  }

  listbox = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (listbox), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (listbox, "content");

  for (guint i = 0; i < n_certificates; i++) {
    GtkWidget *row = create_certificate_row (request, dialog, i, group);
    GtkWidget *button = adw_action_row_get_activatable_widget (ADW_ACTION_ROW (row));

    if (ephy_engine_client_certificate_get_certificate (request, i)) {
      n_selectable++;
      if (!group)
        group = GTK_CHECK_BUTTON (button);
    }
    gtk_list_box_append (GTK_LIST_BOX (listbox), row);
  }

  adw_alert_dialog_set_extra_child (dialog, listbox);
  adw_alert_dialog_add_response (dialog, "cancel", n_selectable ? _("_Cancel") : _("_Close"));
  if (n_selectable) {
    adw_alert_dialog_add_response (dialog, "select", _("_Select"));
    adw_alert_dialog_set_response_appearance (dialog, "select", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_enabled (dialog, "select", FALSE);
  } else {
    g_autofree char *unavailable_body = NULL;

    if (ephy_engine_client_certificate_is_proxy (request))
      unavailable_body = g_strdup_printf (_("No usable certificates are available for the proxy %s."), recipient);
    else
      unavailable_body = g_strdup_printf (_("No usable certificates are available for the server %s."), recipient);
    adw_alert_dialog_set_body (dialog, unavailable_body);
  }
  adw_alert_dialog_set_default_response (dialog, n_selectable ? "select" : "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  return dialog;
}

gboolean
ephy_client_certificate_dialog_get_selected (AdwAlertDialog *dialog,
                                             guint          *index)
{
  guint stored;

  g_return_val_if_fail (ADW_IS_ALERT_DIALOG (dialog), FALSE);
  g_return_val_if_fail (index != NULL, FALSE);

  stored = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog),
                                                "ephy-client-certificate-index"));
  if (stored == 0)
    return FALSE;

  *index = stored - 1;
  return TRUE;
}
