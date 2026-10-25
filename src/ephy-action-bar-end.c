/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2018 Purism SPC
 *  Copyright © 2018 Adrien Plazas <kekun.plazas@laposte.net>
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

#include "ephy-action-bar-end.h"

#include "ephy-bookmark-properties.h"
#include "ephy-bookmarks-dialog.h"
#include "ephy-browser-action-row.h"
#include "ephy-browser-action.h"
#include "ephy-desktop-utils.h"
#include "ephy-downloads-paintable.h"
#include "ephy-downloads-popover.h"
#include "ephy-embed.h"
#include "ephy-embed-shell.h"
#include "ephy-embed-container.h"
#include "ephy-shell.h"
#include "ephy-web-extension-manager.h"
#include "ephy-window.h"

#define NEEDS_ATTENTION_ANIMATION_TIMEOUT 2000 /*ms */

struct _EphyActionBarEnd {
  GtkBox parent_instance;

  GtkWidget *bookmarks_button;
  GtkWidget *downloads_revealer;
  GtkWidget *downloads_button;
  GtkWidget *downloads_popover;
  GtkWidget *downloads_icon;
  GtkWidget *overview_button;
  GtkWidget *browser_actions_button;
  GtkWidget *browser_actions_popover;
  GtkWidget *browser_actions_scrolled_window;
  GtkWidget *browser_actions_listbox;
  GtkWidget *page_actions_listbox;
  GtkWidget *browser_actions_stack;
  GtkWidget *browser_actions_popup_view_box;
  GtkWidget *browser_actions_popup_view_label;
  GtkWidget *browser_action_popup_web_view;

  GListModel *browser_actions_model;

  GdkPaintable *downloads_paintable;

  guint downloads_button_attention_timeout_id;
};

G_DEFINE_FINAL_TYPE (EphyActionBarEnd, ephy_action_bar_end, GTK_TYPE_BOX)

static void set_browser_actions (EphyActionBarEnd *action_bar_end,
                                 GListStore       *browser_actions);

static void update_extensions_button_visibility (EphyActionBarEnd *action_bar_end);

static void
add_attention_timeout_cb (EphyActionBarEnd *self)
{
  gtk_widget_remove_css_class (self->downloads_icon, "accent");
  self->downloads_button_attention_timeout_id = 0;
}

static void
add_attention (EphyActionBarEnd *self)
{
  g_clear_handle_id (&self->downloads_button_attention_timeout_id, g_source_remove);

  gtk_widget_add_css_class (self->downloads_icon, "accent");
  self->downloads_button_attention_timeout_id = g_timeout_add_once (NEEDS_ATTENTION_ANIMATION_TIMEOUT,
                                                                    (GSourceOnceFunc)add_attention_timeout_cb,
                                                                    self);
}

static void
download_added_cb (EphyDownloadsManager *manager,
                   EphyDownload         *download,
                   EphyActionBarEnd     *action_bar_end)
{
  if (!action_bar_end->downloads_popover) {
    action_bar_end->downloads_popover = ephy_downloads_popover_new ();
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (action_bar_end->downloads_button),
                                 action_bar_end->downloads_popover);
  }

  add_attention (action_bar_end);
  gtk_revealer_set_reveal_child (GTK_REVEALER (action_bar_end->downloads_revealer), TRUE);
}

static void
download_completed_cb (EphyDownloadsManager *manager,
                       EphyDownload         *download,
                       EphyActionBarEnd     *action_bar_end)
{
  ephy_downloads_paintable_animate_done (EPHY_DOWNLOADS_PAINTABLE (action_bar_end->downloads_paintable));
}

static void
download_removed_cb (EphyDownloadsManager *manager,
                     EphyDownload         *download,
                     EphyActionBarEnd     *action_bar_end)
{
  if (!ephy_downloads_manager_get_downloads (manager))
    gtk_revealer_set_reveal_child (GTK_REVEALER (action_bar_end->downloads_revealer), FALSE);
}

static void
downloads_estimated_progress_cb (EphyDownloadsManager *manager,
                                 EphyActionBarEnd     *action_bar_end)
{
  gdouble fraction = ephy_downloads_manager_get_estimated_progress (manager);

  g_object_set (action_bar_end->downloads_paintable, "progress", fraction, NULL);
}

static void
show_downloads_cb (EphyDownloadsManager *manager,
                   EphyActionBarEnd     *action_bar_end)
{
  if (gtk_widget_get_mapped (GTK_WIDGET (action_bar_end)))
    gtk_menu_button_popup (GTK_MENU_BUTTON (action_bar_end->downloads_button));
}

static void
remove_popup_webview (EphyActionBarEnd *action_bar_end)
{
  if (action_bar_end->browser_action_popup_web_view) {
    gtk_box_remove (GTK_BOX (action_bar_end->browser_actions_popup_view_box),
                    action_bar_end->browser_action_popup_web_view);
    action_bar_end->browser_action_popup_web_view = NULL;
  }
}

static gboolean
is_private_mode (void)
{
  EphyEmbedShellMode mode = ephy_embed_shell_get_mode (ephy_embed_shell_get_default ());

  return mode == EPHY_EMBED_SHELL_MODE_PRIVATE ||
         mode == EPHY_EMBED_SHELL_MODE_INCOGNITO;
}

static EphyWebView *
get_popup_owner (EphyActionBarEnd *action_bar_end)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (action_bar_end));
  EphyEmbed *embed;

  if (!EPHY_IS_WINDOW (root) || is_private_mode ())
    return NULL;

  embed = ephy_window_get_active_embed (EPHY_WINDOW (root));
  return embed ? ephy_embed_get_web_view (embed) : NULL;
}

static void
show_browser_action_popup (EphyActionBarEnd  *action_bar_end,
                           EphyBrowserAction *action)
{
  EphyWebExtensionManager *manager = ephy_web_extension_manager_get_default ();
  EphyWebExtension *web_extension = ephy_browser_action_get_web_extension (action);
  EphyWebView *owner = get_popup_owner (action_bar_end);
  GtkWidget *popup_view;

  if (!owner)
    return;

  popup_view = ephy_web_extension_manager_create_browser_popup (manager, web_extension, owner);
  if (!popup_view)
    return;

  gtk_box_append (GTK_BOX (action_bar_end->browser_actions_popup_view_box), popup_view);
  action_bar_end->browser_action_popup_web_view = popup_view;

  gtk_label_set_text (GTK_LABEL (action_bar_end->browser_actions_popup_view_label),
                      ephy_browser_action_get_title (action));

  gtk_stack_set_visible_child (GTK_STACK (action_bar_end->browser_actions_stack),
                               action_bar_end->browser_actions_popup_view_box);
}

static void
browser_actions_popup_view_back_clicked_cb (GtkButton        *button,
                                            EphyActionBarEnd *action_bar_end)
{
  gtk_stack_set_visible_child (GTK_STACK (action_bar_end->browser_actions_stack),
                               action_bar_end->browser_actions_scrolled_window);
  remove_popup_webview (action_bar_end);
}

static void
show_browser_action_cb (EphyWebExtensionManager *manager,
                        EphyBrowserAction       *action,
                        EphyActionBarEnd        *action_bar_end)
{
  GtkWindow *parent_window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (action_bar_end)));
  GtkWindow *active_window = gtk_application_get_active_window (GTK_APPLICATION (g_application_get_default ()));

  /* There may be multiple action bars that exist. We only want to show the popup in the visible bar the active window. */
  if (parent_window != active_window)
    return;
  if (!gtk_widget_is_visible (action_bar_end->browser_actions_button))
    return;

  remove_popup_webview (action_bar_end);
  gtk_menu_button_popdown (GTK_MENU_BUTTON (action_bar_end->browser_actions_button));
  show_browser_action_popup (action_bar_end, action);
}

static void
browser_actions_row_activated_cb (GtkListBox           *listbox,
                                  EphyBrowserActionRow *row,
                                  EphyActionBarEnd     *action_bar_end)
{
  EphyBrowserAction *action = ephy_browser_action_row_get_browser_action (row);
  EphyWebExtension *web_extension = ephy_browser_action_get_web_extension (action);

  if (ephy_web_extension_is_native (web_extension) &&
      !ephy_web_extension_get_browser_popup (web_extension))
    return;

  /* If it was handled we are done, otherwise we have to show a popup. */
  if (ephy_browser_action_activate (action)) {
    gtk_menu_button_popdown (GTK_MENU_BUTTON (action_bar_end->browser_actions_button));
    return;
  }

  show_browser_action_popup (action_bar_end, action);
}

static void
browser_action_options_activated_cb (EphyBrowserActionRow *row,
                                     EphyActionBarEnd     *action_bar_end)
{
  EphyBrowserAction *action = ephy_browser_action_row_get_browser_action (row);
  EphyWebExtension *web_extension = ephy_browser_action_get_web_extension (action);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (action_bar_end));

  if (!EPHY_IS_WINDOW (root) || is_private_mode () ||
      !ephy_web_extension_is_native (web_extension) ||
      !ephy_web_extension_get_option_ui_page (web_extension) ||
      !*ephy_web_extension_get_option_ui_page (web_extension))
    return;

  gtk_menu_button_popdown (GTK_MENU_BUTTON (action_bar_end->browser_actions_button));
  ephy_web_extension_manager_open_options (ephy_web_extension_manager_get_default (),
                                           web_extension,
                                           EPHY_WINDOW (root));
}

static void
page_actions_row_activated_cb (GtkListBox       *listbox,
                               GtkListBoxRow    *row,
                               EphyActionBarEnd *action_bar_end)
{
  GtkWidget *action = g_object_get_data (G_OBJECT (row), "ephy-page-action");

  if (action && gtk_widget_get_visible (GTK_WIDGET (row)) && gtk_widget_get_sensitive (action))
    gtk_widget_activate (action);
}

static void
browser_action_popover_visible_changed_cb (GtkWidget        *popover,
                                           GParamSpec       *pspec,
                                           EphyActionBarEnd *action_bar_end)
{
  if (!gtk_widget_get_visible (popover)) {
    GtkStack *stack = GTK_STACK (action_bar_end->browser_actions_stack);

    /* Reset to default state and destroy any open webview. */
    gtk_stack_set_visible_child (stack, action_bar_end->browser_actions_scrolled_window);
    remove_popup_webview (action_bar_end);
  }
}

static void
on_bookmarks_button (GtkButton *button,
                     gpointer   user_data)
{
  GtkWindow *parent_window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button)));
  ephy_window_toggle_bookmarks (EPHY_WINDOW (parent_window));
}

static void
ephy_action_bar_end_dispose (GObject *object)
{
  EphyActionBarEnd *self = EPHY_ACTION_BAR_END (object);

  g_clear_handle_id (&self->downloads_button_attention_timeout_id, g_source_remove);
  g_clear_object (&self->browser_actions_model);

  G_OBJECT_CLASS (ephy_action_bar_end_parent_class)->dispose (object);
}

static void
ephy_action_bar_end_class_init (EphyActionBarEndClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ephy_action_bar_end_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/epiphany/gtk/action-bar-end.ui");

  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        bookmarks_button);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        downloads_revealer);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        downloads_button);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        downloads_icon);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        overview_button);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_button);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_popover);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_scrolled_window);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_listbox);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        page_actions_listbox);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_stack);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_popup_view_box);
  gtk_widget_class_bind_template_child (widget_class,
                                        EphyActionBarEnd,
                                        browser_actions_popup_view_label);

  gtk_widget_class_bind_template_callback (widget_class,
                                           browser_actions_popup_view_back_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class,
                                           browser_actions_row_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class,
                                           page_actions_row_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class,
                                           on_bookmarks_button);
}

static void
ephy_action_bar_end_init (EphyActionBarEnd *action_bar_end)
{
  GObject *object = G_OBJECT (action_bar_end);
  EphyDownloadsManager *downloads_manager;
  EphyWebExtensionManager *extension_manager;
  EphyEmbedShell *embed_shell;

  gtk_widget_init_template (GTK_WIDGET (action_bar_end));

  /* Downloads */
  embed_shell = ephy_embed_shell_get_default ();
  downloads_manager = ephy_embed_shell_get_downloads_manager (embed_shell);

  gtk_revealer_set_reveal_child (GTK_REVEALER (action_bar_end->downloads_revealer),
                                 !!ephy_downloads_manager_get_downloads (downloads_manager));

  if (ephy_downloads_manager_get_downloads (downloads_manager)) {
    action_bar_end->downloads_popover = ephy_downloads_popover_new ();
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (action_bar_end->downloads_button), action_bar_end->downloads_popover);
  }

  action_bar_end->downloads_paintable = ephy_downloads_paintable_new (action_bar_end->downloads_icon);
  gtk_image_set_from_paintable (GTK_IMAGE (action_bar_end->downloads_icon),
                                action_bar_end->downloads_paintable);

  if (is_desktop_pantheon ()) {
    gtk_button_set_icon_name (GTK_BUTTON (action_bar_end->bookmarks_button),
                              "user-bookmarks");
    gtk_button_set_icon_name (GTK_BUTTON (action_bar_end->overview_button),
                              "view-grid");
  }

  gtk_widget_set_visible (action_bar_end->overview_button,
                          ephy_embed_shell_get_mode (embed_shell) != EPHY_EMBED_SHELL_MODE_APPLICATION);

  g_signal_connect_object (downloads_manager, "download-added",
                           G_CALLBACK (download_added_cb),
                           object, G_CONNECT_DEFAULT);
  g_signal_connect_object (downloads_manager, "download-completed",
                           G_CALLBACK (download_completed_cb),
                           object, G_CONNECT_DEFAULT);
  g_signal_connect_object (downloads_manager, "download-removed",
                           G_CALLBACK (download_removed_cb),
                           object, G_CONNECT_DEFAULT);
  g_signal_connect_object (downloads_manager, "estimated-progress-changed",
                           G_CALLBACK (downloads_estimated_progress_cb),
                           object, G_CONNECT_DEFAULT);
  g_signal_connect_object (downloads_manager, "show-downloads",
                           G_CALLBACK (show_downloads_cb),
                           object, G_CONNECT_DEFAULT);

  extension_manager = ephy_web_extension_manager_get_default ();
  g_signal_connect_object (extension_manager, "show-browser-action",
                           G_CALLBACK (show_browser_action_cb),
                           object, G_CONNECT_DEFAULT);

  set_browser_actions (action_bar_end, ephy_web_extension_manager_get_browser_actions (extension_manager));

  g_signal_connect (action_bar_end->browser_actions_popover, "notify::visible",
                    G_CALLBACK (browser_action_popover_visible_changed_cb),
                    action_bar_end);
}

EphyActionBarEnd *
ephy_action_bar_end_new (void)
{
  return g_object_new (EPHY_TYPE_ACTION_BAR_END,
                       NULL);
}

void
ephy_action_bar_end_set_show_bookmarks_button (EphyActionBarEnd *action_bar_end,
                                               gboolean          show)
{
  gtk_widget_set_visible (action_bar_end->bookmarks_button, show);
}

void
ephy_action_bar_end_show_downloads (EphyActionBarEnd *action_bar_end)
{
  if (gtk_widget_get_visible (action_bar_end->downloads_button))
    gtk_menu_button_popup (GTK_MENU_BUTTON (action_bar_end->downloads_button));
}

GtkWidget *
ephy_action_bar_end_get_downloads_revealer (EphyActionBarEnd *action_bar_end)
{
  return action_bar_end->downloads_revealer;
}

GtkWidget *
ephy_action_bar_end_get_downloads_button (EphyActionBarEnd *action_bar_end)
{
  return action_bar_end->downloads_button;
}

static gboolean
has_visible_page_actions (EphyActionBarEnd *action_bar_end)
{
  for (GtkWidget *row = gtk_widget_get_first_child (action_bar_end->page_actions_listbox);
       row;
       row = gtk_widget_get_next_sibling (row)) {
    if (gtk_widget_get_visible (row))
      return TRUE;
  }

  return FALSE;
}

static gboolean
has_visible_browser_actions (EphyActionBarEnd *action_bar_end)
{
  for (GtkWidget *row = gtk_widget_get_first_child (action_bar_end->browser_actions_listbox);
       row;
       row = gtk_widget_get_next_sibling (row)) {
    if (gtk_widget_get_visible (row))
      return TRUE;
  }

  return FALSE;
}

static void
update_extensions_button_visibility (EphyActionBarEnd *action_bar_end)
{
  gboolean has_browser_actions = has_visible_browser_actions (action_bar_end);
  gboolean has_page_actions = has_visible_page_actions (action_bar_end);

  gtk_widget_set_visible (action_bar_end->page_actions_listbox, has_page_actions);
  gtk_widget_set_visible (action_bar_end->browser_actions_button,
                          has_browser_actions || has_page_actions);
}

static void
page_action_row_visibility_changed_cb (GtkWidget        *row,
                                       GParamSpec       *pspec,
                                       EphyActionBarEnd *action_bar_end)
{
  update_extensions_button_visibility (action_bar_end);
}

static void
detach_page_action_row (EphyActionBarEnd *action_bar_end,
                        GtkWidget        *row)
{
  GtkWidget *action = g_object_get_data (G_OBJECT (row), "ephy-page-action");
  GBinding *visible_binding = g_object_get_data (G_OBJECT (row), "ephy-page-action-visible-binding");
  GBinding *tooltip_binding = g_object_get_data (G_OBJECT (row), "ephy-page-action-tooltip-binding");

  g_signal_handlers_disconnect_by_func (row,
                                        G_CALLBACK (page_action_row_visibility_changed_cb),
                                        action_bar_end);
  if (visible_binding)
    g_binding_unbind (visible_binding);
  if (tooltip_binding)
    g_binding_unbind (tooltip_binding);
  g_object_set_data (G_OBJECT (row), "ephy-page-action-visible-binding", NULL);
  g_object_set_data (G_OBJECT (row), "ephy-page-action-tooltip-binding", NULL);
  g_object_set_data (G_OBJECT (row), "ephy-page-action", NULL);

  if (action && GTK_IS_BOX (gtk_widget_get_parent (action)))
    gtk_box_remove (GTK_BOX (gtk_widget_get_parent (action)), action);
  gtk_list_box_remove (GTK_LIST_BOX (action_bar_end->page_actions_listbox), row);
}

void
ephy_action_bar_end_clear_page_actions (EphyActionBarEnd *action_bar_end)
{
  GtkWidget *row;

  g_return_if_fail (EPHY_IS_ACTION_BAR_END (action_bar_end));

  while ((row = gtk_widget_get_first_child (action_bar_end->page_actions_listbox)))
    detach_page_action_row (action_bar_end, row);

  update_extensions_button_visibility (action_bar_end);
}

void
ephy_action_bar_end_add_page_action (EphyActionBarEnd *action_bar_end,
                                     GtkWidget        *action,
                                     const char       *extension_name)
{
  GtkWidget *box;
  GtkWidget *label;
  GtkWidget *row;
  GBinding *binding;

  g_return_if_fail (EPHY_IS_ACTION_BAR_END (action_bar_end));
  g_return_if_fail (GTK_IS_WIDGET (action));
  g_return_if_fail (gtk_widget_get_parent (action) == NULL);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  label = gtk_label_new (extension_name);
  row = gtk_list_box_row_new ();

  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_focus_on_click (row, FALSE);

  gtk_widget_remove_css_class (action, "entry-icon");
  gtk_widget_remove_css_class (action, "end");
  gtk_widget_add_css_class (action, "flat");

  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_FILL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);

  gtk_box_append (GTK_BOX (box), action);
  gtk_box_append (GTK_BOX (box), label);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "ephy-page-action", action);

  binding = g_object_bind_property (action, "visible",
                                    row, "visible",
                                    G_BINDING_SYNC_CREATE);
  g_object_set_data_full (G_OBJECT (row), "ephy-page-action-visible-binding",
                          g_object_ref (binding), g_object_unref);
  binding = g_object_bind_property (action, "tooltip-text",
                                    row, "tooltip-text",
                                    G_BINDING_SYNC_CREATE);
  g_object_set_data_full (G_OBJECT (row), "ephy-page-action-tooltip-binding",
                          g_object_ref (binding), g_object_unref);
  g_signal_connect_object (row, "notify::visible",
                           G_CALLBACK (page_action_row_visibility_changed_cb),
                           action_bar_end, G_CONNECT_DEFAULT);

  gtk_list_box_append (GTK_LIST_BOX (action_bar_end->page_actions_listbox), row);
  update_extensions_button_visibility (action_bar_end);
}

void
ephy_action_bar_end_remove_page_action (EphyActionBarEnd *action_bar_end,
                                        GtkWidget        *action)
{
  GtkWidget *row;

  g_return_if_fail (EPHY_IS_ACTION_BAR_END (action_bar_end));
  g_return_if_fail (GTK_IS_WIDGET (action));

  for (row = gtk_widget_get_first_child (action_bar_end->page_actions_listbox);
       row;
       row = gtk_widget_get_next_sibling (row)) {
    if (g_object_get_data (G_OBJECT (row), "ephy-page-action") == action) {
      detach_page_action_row (action_bar_end, row);
      update_extensions_button_visibility (action_bar_end);
      return;
    }
  }
}

GtkWidget *
create_browser_action_item_widget (EphyBrowserAction *action,
                                   gpointer           user_data)
{
  EphyActionBarEnd *action_bar_end = EPHY_ACTION_BAR_END (user_data);
  EphyWebExtension *web_extension = ephy_browser_action_get_web_extension (action);
  GtkWidget *row = ephy_browser_action_row_new (action);

  if (is_private_mode () && ephy_web_extension_is_native (web_extension))
    gtk_widget_set_visible (row, FALSE);

  g_signal_connect (row, "options-activated",
                    G_CALLBACK (browser_action_options_activated_cb),
                    action_bar_end);

  return row;
}

static void
browser_actions_items_changed_cb (GListModel       *list,
                                  guint             position,
                                  guint             removed,
                                  guint             added,
                                  EphyActionBarEnd *action_bar_end)
{
  update_extensions_button_visibility (action_bar_end);

  /* This handles an edge-case where if an extension is disabled while its popover is open the webview should be destroyed.
   * However in normal usage this shouldn't happen and with the GTK4 port the extension dialog is also modal.
   * So we just always manually close it instead of trying to track which extension popup is open. */
  if (removed)
    gtk_menu_button_popdown (GTK_MENU_BUTTON (action_bar_end->browser_actions_button));
}

static void
set_browser_actions (EphyActionBarEnd *action_bar_end,
                     GListStore       *browser_actions)
{
  g_set_object (&action_bar_end->browser_actions_model, G_LIST_MODEL (browser_actions));

  gtk_list_box_bind_model (GTK_LIST_BOX (action_bar_end->browser_actions_listbox),
                           G_LIST_MODEL (browser_actions),
                           (GtkListBoxCreateWidgetFunc)create_browser_action_item_widget,
                           action_bar_end, NULL);

  g_signal_connect_object (browser_actions, "items-changed", G_CALLBACK (browser_actions_items_changed_cb),
                           action_bar_end, G_CONNECT_DEFAULT);

  browser_actions_items_changed_cb (G_LIST_MODEL (browser_actions), 0, 0, 0, action_bar_end);
}
