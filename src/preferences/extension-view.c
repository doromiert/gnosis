/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2022 Jamie Murphy
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

#include "extension-view.h"

struct _EphyExtensionView {
  AdwNavigationPage parent_instance;

  GtkWidget *box;

  GtkWidget *window_title;

  GtkWidget *author_row;
  GtkWidget *author_label;
  GtkWidget *version_row;
  GtkWidget *version_label;
  GtkWidget *homepage_row;

  AdwSwitchRow *enabled_row;

  EphyWebExtension *web_extension;
  GCancellable *cancellable;
  gboolean updating_enabled;
};

G_DEFINE_FINAL_TYPE (EphyExtensionView, ephy_extension_view, ADW_TYPE_NAVIGATION_PAGE)

typedef enum {
  PROP_WEB_EXTENSION = 1,
} EphyExtensionViewProps;

static GParamSpec *properties[PROP_WEB_EXTENSION + 1];

static void
update_enabled_state (EphyExtensionView *self)
{
  gboolean available = ephy_web_extension_native_is_available (self->web_extension);
  gboolean pending = ephy_web_extension_native_is_pending (self->web_extension);

  self->updating_enabled = TRUE;
  adw_switch_row_set_active (self->enabled_row, ephy_web_extension_native_is_enabled (self->web_extension));
  self->updating_enabled = FALSE;

  gtk_widget_set_sensitive (GTK_WIDGET (self->enabled_row), available && !pending);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->enabled_row), available ? NULL : _("Unavailable"));
}

static void
show_native_toggle_error (EphyExtensionView *self,
                          const GError      *error)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  AdwDialog *dialog;

  if (!root)
    return;

  dialog = adw_alert_dialog_new (_("Could Not Update Extension"), error->message);
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (dialog), FALSE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_dialog_present (dialog, GTK_WIDGET (root));
}

static void
native_toggle_finished_cb (EphyWebExtensionManager *manager,
                           GAsyncResult            *result,
                           gpointer                 user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (EphyExtensionView) self = g_weak_ref_get (user_data);

  ephy_web_extension_manager_set_native_enabled_finish (manager, result, &error);

  if (self) {
    update_enabled_state (self);
    if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      show_native_toggle_error (self, error);
  }

  g_weak_ref_clear (user_data);
  g_free (user_data);
}

static void
on_homepage_activated (AdwActionRow *row,
                       gpointer      user_data)
{
  EphyExtensionView *self = EPHY_EXTENSION_VIEW (user_data);
  g_autoptr (GtkUriLauncher) launcher = NULL;

  launcher = gtk_uri_launcher_new (ephy_web_extension_get_homepage_url (self->web_extension));
  gtk_uri_launcher_launch (launcher,
                           GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                           NULL, NULL, NULL);
}

static void
on_toggle_extension_enabled (EphyExtensionView *view)
{
  EphyWebExtensionManager *manager = ephy_web_extension_manager_get_default ();
  GWeakRef *weak_view;

  if (view->updating_enabled)
    return;

  if (!ephy_web_extension_native_is_available (view->web_extension) ||
      ephy_web_extension_native_is_pending (view->web_extension)) {
    update_enabled_state (view);
    return;
  }

  weak_view = g_new0 (GWeakRef, 1);
  g_weak_ref_init (weak_view, view);
  ephy_web_extension_manager_set_native_enabled_async (manager,
                                                       view->web_extension,
                                                       adw_switch_row_get_active (view->enabled_row),
                                                       view->cancellable,
                                                       (GAsyncReadyCallback)native_toggle_finished_cb,
                                                       weak_view);
  update_enabled_state (view);
}

static void
update (EphyExtensionView *self)
{
  const char *author = ephy_web_extension_get_author (self->web_extension);
  const char *homepage = ephy_web_extension_get_homepage_url (self->web_extension);

  /* Window Title */
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self),
                                 ephy_web_extension_get_name (self->web_extension));

  adw_window_title_set_title (ADW_WINDOW_TITLE (self->window_title),
                              ephy_web_extension_get_name (self->web_extension));
  adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->window_title),
                                 ephy_web_extension_get_description (self->web_extension));

  /* Information Rows */
  gtk_label_set_label (GTK_LABEL (self->version_label), ephy_web_extension_get_version (self->web_extension));
  if (author && *author) {
    gtk_widget_set_visible (self->author_row, TRUE);
    gtk_label_set_label (GTK_LABEL (self->author_label), author);
  }
  if (homepage && *homepage)
    gtk_widget_set_visible (self->homepage_row, TRUE);

  update_enabled_state (self);
}

static void
ephy_extension_view_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  EphyExtensionView *self = EPHY_EXTENSION_VIEW (object);

  switch ((EphyExtensionViewProps)prop_id) {
    case PROP_WEB_EXTENSION:
      g_set_object (&self->web_extension, g_value_get_object (value));
      g_signal_connect_object (self->web_extension, "native-state-changed", G_CALLBACK (update_enabled_state), self, G_CONNECT_SWAPPED);
      update (self);
      break;
  }
}

static void
ephy_extension_view_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  EphyExtensionView *self = EPHY_EXTENSION_VIEW (object);

  switch ((EphyExtensionViewProps)prop_id) {
    case PROP_WEB_EXTENSION:
      g_value_set_object (value, self->web_extension);
      break;
  }
}

static void
ephy_extension_view_dispose (GObject *object)
{
  EphyExtensionView *self = EPHY_EXTENSION_VIEW (object);

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }
  g_clear_object (&self->web_extension);

  G_OBJECT_CLASS (ephy_extension_view_parent_class)->dispose (object);
}

static void
ephy_extension_view_class_init (EphyExtensionViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = ephy_extension_view_get_property;
  object_class->set_property = ephy_extension_view_set_property;
  object_class->dispose = ephy_extension_view_dispose;

  properties[PROP_WEB_EXTENSION] =
    g_param_spec_object ("web-extension",
                         NULL, NULL,
                         EPHY_TYPE_WEB_EXTENSION,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/epiphany/gtk/extension-view.ui");

  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, window_title);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, author_row);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, author_label);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, version_row);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, version_label);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, homepage_row);
  gtk_widget_class_bind_template_child (widget_class, EphyExtensionView, enabled_row);

  gtk_widget_class_bind_template_callback (widget_class, on_homepage_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_toggle_extension_enabled);
}

static void
ephy_extension_view_init (EphyExtensionView *extension_view)
{
  gtk_widget_init_template (GTK_WIDGET (extension_view));
  extension_view->cancellable = g_cancellable_new ();
}

EphyExtensionView *
ephy_extension_view_new (EphyWebExtension *extension)
{
  return g_object_new (EPHY_TYPE_EXTENSION_VIEW,
                       "web-extension", extension,
                       NULL);
}
