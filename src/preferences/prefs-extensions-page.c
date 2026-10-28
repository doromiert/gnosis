/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2019-2020 Jan-Michael Brummer <jan.brummer@tabos.org>
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

#include "prefs-extensions-page.h"

#include "ephy-embed-shell.h"
#include "ephy-link.h"
#include "ephy-window.h"
#include "ephy-engine-extensions.h"
#include "ephy-pixbuf-utils.h"
#include "ephy-web-extension-manager.h"
#include "ephy-web-extension.h"

enum {
  EXTENSION_ROW_ACTIVATED,

  LAST_SIGNAL
};

struct _PrefsExtensionsPage {
  AdwPreferencesPage parent_instance;
  EphyWebExtensionManager *web_extension_manager;

  GtkWidget *stack;
  GtkWidget *listbox;
  GtkWidget *install_group;
  GtkWidget *native_install_row;
  GCancellable *cancellable;
};

static guint signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE (PrefsExtensionsPage, prefs_extensions_page, ADW_TYPE_PREFERENCES_PAGE)

static void
open_extension_store (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
  GtkWidget *window = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (user_data)));
  const char *store = g_variant_get_string (parameter, NULL);
  const char *uri = NULL;

  if (!EPHY_IS_WINDOW (window))
    return;
  if (g_str_equal (store, "chrome"))
    uri = "https://chromewebstore.google.com/";
  else if (g_str_equal (store, "edge"))
    uri = "https://microsoftedge.microsoft.com/addons/";
  if (!uri)
    return;

  ephy_link_open (EPHY_LINK (window), uri, NULL, EPHY_LINK_NEW_TAB | EPHY_LINK_JUMP_TO);
  if (ADW_IS_APPLICATION_WINDOW (window)) {
    AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));

    if (dialog)
      adw_dialog_close (dialog);
  }
}

static void
on_extension_row_activated (GtkWidget           *row,
                            PrefsExtensionsPage *page)
{
  EphyWebExtension *web_extension = g_object_get_data (G_OBJECT (row), "web_extension");

  g_signal_emit (page, signals[EXTENSION_ROW_ACTIVATED], 0, web_extension);
}

static void
show_native_install_result (PrefsExtensionsPage *self,
                            const char           *title,
                            const char           *body)
{
  AdwDialog *dialog;

  dialog = adw_alert_dialog_new (title, body);
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (dialog), FALSE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static gboolean
native_install_error_is_cancelled (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
         g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
         g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
}

typedef struct {
  GWeakRef page;
  EphyWebExtensionManager *manager;
} NativeInstallData;

static NativeInstallData *
native_install_data_new (PrefsExtensionsPage *page)
{
  NativeInstallData *data = g_new0 (NativeInstallData, 1);

  g_weak_ref_init (&data->page, page);
  data->manager = g_object_ref (page->web_extension_manager);
  return data;
}

static void
native_install_data_free (NativeInstallData *data)
{
  g_weak_ref_clear (&data->page);
  g_clear_object (&data->manager);
  g_free (data);
}

static void
native_install_finished_cb (EphyEngineSession *session,
                            GAsyncResult      *result,
                            gpointer           user_data)
{
  g_autoptr (JsonObject) metadata = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (PrefsExtensionsPage) self = NULL;
  NativeInstallData *data = user_data;

  metadata = ephy_engine_extensions_install_unpacked_finish (session, result, &error);
  self = g_weak_ref_get (&data->page);

  if (self)
    gtk_widget_set_sensitive (self->native_install_row, TRUE);

  if (metadata)
    ephy_web_extension_manager_refresh_native_installed (data->manager);

  if (self && error && !native_install_error_is_cancelled (error)) {
    show_native_install_result (self, _("Could Not Install Extension"), error->message);
  } else if (self && metadata) {
    const char *name = json_object_get_string_member_with_default (metadata, "name", _("Extension"));
    g_autofree char *body = g_strdup_printf (_("%s will be installed after you restart Gnosis."), name);

    show_native_install_result (self, _("Restart Required"), body);
  }

  native_install_data_free (data);
}

static void
native_install_folder_selected_cb (GtkFileDialog *dialog,
                                   GAsyncResult  *result,
                                   gpointer       user_data)
{
  g_autoptr (GFile) directory = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (PrefsExtensionsPage) self = NULL;
  NativeInstallData *data = user_data;

  directory = gtk_file_dialog_select_folder_finish (dialog, result, &error);
  self = g_weak_ref_get (&data->page);

  if (!directory) {
    if (self)
      gtk_widget_set_sensitive (self->native_install_row, TRUE);
    if (self && error && !native_install_error_is_cancelled (error))
      show_native_install_result (self, _("Could Not Select Extension Folder"), error->message);
    native_install_data_free (data);
    return;
  }

  if (!self) {
    native_install_data_free (data);
    return;
  }

  ephy_engine_extensions_install_unpacked_async (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                                 directory,
                                                 self->cancellable,
                                                 (GAsyncReadyCallback)native_install_finished_cb,
                                                 data);
}

static void
on_install_unpacked_clicked (GtkButton *button,
                             gpointer   user_data)
{
  PrefsExtensionsPage *self = EPHY_PREFS_EXTENSIONS_PAGE (user_data);
  g_autoptr (GtkFileDialog) dialog = NULL;
  NativeInstallData *data;

  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Select an Unpacked Extension"));

  gtk_widget_set_sensitive (self->native_install_row, FALSE);
  data = native_install_data_new (self);
  gtk_file_dialog_select_folder (dialog,
                                 GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                                 self->cancellable,
                                 (GAsyncReadyCallback)native_install_folder_selected_cb,
                                 data);
}

static gboolean toggle_state_set_cb (GtkSwitch *widget,
                                     gboolean   state,
                                     gpointer   user_data);

static void
update_native_toggle (EphyWebExtension *web_extension,
                      GtkSwitch       *toggle)
{
  g_signal_handlers_block_by_func (toggle, toggle_state_set_cb, web_extension);
  gtk_switch_set_active (toggle, ephy_web_extension_native_is_enabled (web_extension));
  g_signal_handlers_unblock_by_func (toggle, toggle_state_set_cb, web_extension);

  gtk_widget_set_sensitive (GTK_WIDGET (toggle),
                            ephy_web_extension_native_is_available (web_extension) &&
                            !ephy_web_extension_native_is_pending (web_extension));
  gtk_widget_set_tooltip_text (GTK_WIDGET (toggle),
                               ephy_web_extension_native_is_available (web_extension)
                                 ? NULL
                                 : ephy_web_extension_native_is_restart_required (web_extension)
                                   ? _("Restart Required")
                                   : _("Unavailable"));
}

static void
show_native_toggle_error (GtkWidget    *parent,
                          const GError *error)
{
  AdwDialog *dialog;

  dialog = adw_alert_dialog_new (_("Could Not Update Extension"), error->message);
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (dialog), FALSE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_dialog_present (dialog, parent);
}

static void
native_toggle_finished_cb (EphyWebExtensionManager *manager,
                           GAsyncResult            *result,
                           gpointer                 user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (PrefsExtensionsPage) page = NULL;
  GWeakRef *weak_page = user_data;

  ephy_web_extension_manager_set_native_enabled_finish (manager, result, &error);
  page = g_weak_ref_get (weak_page);

  if (page && error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (page));

    if (root)
      show_native_toggle_error (GTK_WIDGET (root), error);
  }

  g_weak_ref_clear (weak_page);
  g_free (weak_page);
}

static gboolean
toggle_state_set_cb (GtkSwitch *widget,
                     gboolean   state,
                     gpointer   user_data)
{
  EphyWebExtensionManager *manager = ephy_web_extension_manager_get_default ();
  EphyWebExtension *web_extension = EPHY_WEB_EXTENSION (user_data);
  PrefsExtensionsPage *page = EPHY_PREFS_EXTENSIONS_PAGE (gtk_widget_get_ancestor (GTK_WIDGET (widget), EPHY_TYPE_PREFS_EXTENSIONS_PAGE));
  GWeakRef *weak_page;

  if (!page || !ephy_web_extension_native_is_available (web_extension) ||
      ephy_web_extension_native_is_pending (web_extension)) {
    update_native_toggle (web_extension, widget);
    return TRUE;
  }

  weak_page = g_new0 (GWeakRef, 1);
  g_weak_ref_init (weak_page, page);
  ephy_web_extension_manager_set_native_enabled_async (manager,
                                                       web_extension,
                                                       state,
                                                       page->cancellable,
                                                       (GAsyncReadyCallback)native_toggle_finished_cb,
                                                       weak_page);
  update_native_toggle (web_extension, widget);
  return TRUE;
}

static GtkWidget *
create_row (PrefsExtensionsPage *self,
            EphyWebExtension    *web_extension)
{
  GtkWidget *row;
  GtkWidget *image;
  GtkWidget *toggle;
  GtkWidget *arrow;
  g_autoptr (GdkPixbuf) icon = NULL;
  EphyWebExtensionManager *manager = ephy_web_extension_manager_get_default ();

  row = adw_action_row_new ();
  g_object_set_data (G_OBJECT (row), "web_extension", web_extension);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), true);
  g_signal_connect (row, "activated", G_CALLBACK (on_extension_row_activated), self);

  /* Tooltip */
  gtk_widget_set_tooltip_text (GTK_WIDGET (row), ephy_web_extension_get_name (web_extension));

  /* Images */
  icon = ephy_web_extension_get_icon (web_extension, 32);
  if (icon) {
    g_autoptr (GdkTexture) texture = ephy_texture_new_for_pixbuf (icon);
    image = gtk_image_new_from_paintable (GDK_PAINTABLE (texture));
  } else {
    image = gtk_image_new_from_icon_name ("application-x-addon-symbolic");
  }
  gtk_image_set_pixel_size (GTK_IMAGE (image), 32);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), image);

  /* Titles */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), ephy_web_extension_get_name (web_extension));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                               ephy_web_extension_is_native (web_extension) &&
                               ephy_web_extension_native_is_restart_required (web_extension)
                                 ? _("Restart Required")
                                 : ephy_web_extension_get_description (web_extension));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);

  /* Toggle */
  toggle = gtk_switch_new ();
  if (ephy_web_extension_is_native (web_extension)) {
    update_native_toggle (web_extension, GTK_SWITCH (toggle));
    g_signal_connect_object (web_extension, "native-state-changed", G_CALLBACK (update_native_toggle), toggle, 0);
    g_signal_connect (toggle, "state-set", G_CALLBACK (toggle_state_set_cb), web_extension);
  } else {
    gtk_switch_set_active (GTK_SWITCH (toggle), ephy_web_extension_manager_is_active (manager, web_extension));
    g_signal_connect (toggle, "state-set", G_CALLBACK (toggle_state_set_cb), web_extension);
  }
  gtk_widget_set_valign (toggle, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), toggle);

  /* Arrow */
  arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_margin_start (arrow, 6);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), arrow);

  return GTK_WIDGET (row);
}

static void
refresh_listbox (PrefsExtensionsPage *self)
{
  GPtrArray *extensions = ephy_web_extension_manager_get_web_extensions (self->web_extension_manager);
  gboolean empty = TRUE;

  gtk_list_box_remove_all (GTK_LIST_BOX (self->listbox));

  for (guint i = 0; i < extensions->len; i++) {
    EphyWebExtension *web_extension = g_ptr_array_index (extensions, i);
    GtkWidget *row;


    row = create_row (self, web_extension);
    gtk_list_box_insert (GTK_LIST_BOX (self->listbox), row, -1);
    empty = FALSE;
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), empty ? "empty" : "list");
}

static void
on_web_extension_manager_changed (EphyWebExtensionManager *manager,
                                  gpointer                 user_data)
{
  PrefsExtensionsPage *self = EPHY_PREFS_EXTENSIONS_PAGE (user_data);

  refresh_listbox (self);
}

static void
prefs_extensions_page_dispose (GObject *object)
{
  PrefsExtensionsPage *self = EPHY_PREFS_EXTENSIONS_PAGE (object);

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  g_clear_weak_pointer (&self->web_extension_manager);

  G_OBJECT_CLASS (prefs_extensions_page_parent_class)->dispose (object);
}

static void
prefs_extensions_page_class_init (PrefsExtensionsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = prefs_extensions_page_dispose;

  signals[EXTENSION_ROW_ACTIVATED] =
    g_signal_new ("extension-row-activated",
                  EPHY_TYPE_PREFS_EXTENSIONS_PAGE,
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  EPHY_TYPE_WEB_EXTENSION);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/epiphany/gtk/prefs-extensions-page.ui");

  gtk_widget_class_bind_template_child (widget_class, PrefsExtensionsPage, stack);
  gtk_widget_class_bind_template_child (widget_class, PrefsExtensionsPage, listbox);
  gtk_widget_class_bind_template_child (widget_class, PrefsExtensionsPage, install_group);
  gtk_widget_class_bind_template_child (widget_class, PrefsExtensionsPage, native_install_row);

  gtk_widget_class_bind_template_callback (widget_class, on_install_unpacked_clicked);
}

static void
prefs_extensions_page_init (PrefsExtensionsPage *self)
{
  EphyWebExtensionManager *manager;

  gtk_widget_init_template (GTK_WIDGET (self));
  {
    g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
    const GActionEntry actions[] = { { "open", open_extension_store, "s" } };

    g_action_map_add_action_entries (G_ACTION_MAP (group), actions, G_N_ELEMENTS (actions), self);
    gtk_widget_insert_action_group (GTK_WIDGET (self), "extension-store", G_ACTION_GROUP (group));
  }


  manager = ephy_web_extension_manager_get_default ();
  g_set_weak_pointer (&self->web_extension_manager, manager);
  g_signal_connect_object (self->web_extension_manager, "changed", G_CALLBACK (on_web_extension_manager_changed), self, G_CONNECT_DEFAULT);

  self->cancellable = g_cancellable_new ();

  refresh_listbox (self);
}
