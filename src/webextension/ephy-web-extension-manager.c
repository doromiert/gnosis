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

#include "config.h"
#include "ephy-web-extension-manager.h"

#include <adwaita.h>
#include "ephy-browser-action.h"
#include "ephy-embed-prefs.h"
#include "ephy-embed-shell.h"
#include "ephy-engine-extensions.h"
#include "ephy-shell.h"
#include "ephy-web-view.h"

/* Extension execution, permissions, messaging, resources and background pages
 * belong to Chromium. This object is only the native catalog/UI model. */
struct _EphyWebExtensionManager {
  GObject parent_instance;
  GCancellable *cancellable;
  GPtrArray *web_extensions;
  GHashTable *browser_action_map;
  GListStore *browser_actions;
  gboolean native_query_pending;
  gboolean native_query_again;
};

G_DEFINE_FINAL_TYPE (EphyWebExtensionManager, ephy_web_extension_manager, G_TYPE_OBJECT)

enum { CHANGED, SHOW_BROWSER_ACTION, LAST_SIGNAL };
static guint signals[LAST_SIGNAL];
static void remove_browser_action (EphyWebExtensionManager *self, EphyWebExtension *extension);
static EphyWebExtension *find_native_extension (EphyWebExtensionManager *self, const char *id);
static void native_extensions_changed (EphyEngineSession *session, EphyWebExtensionManager *self);

static void
reconcile_native_extensions (EphyWebExtensionManager *self,
                              JsonArray               *snapshot)
{
  /* Add real store installs and refresh updated metadata before reconciling
   * action/state objects. Brave, rather than the old staged catalog, owns this
   * inventory. Keep object identity so open detail views remain valid. */
  for (guint i = 0; i < json_array_get_length (snapshot); i++) {
    JsonObject *item = json_array_get_object_element (snapshot, i);
    const char *id = json_object_get_string_member_with_default (item, "id", "");
    EphyWebExtension *extension = find_native_extension (self, id);

    if (extension)
      ephy_web_extension_native_update_metadata (extension, item);
    else if (*id)
      g_ptr_array_add (self->web_extensions, ephy_web_extension_new_native (item));
  }
  for (guint i = self->web_extensions->len; i > 0; i--) {
    EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i - 1);
    gboolean present = FALSE;

    if (!ephy_web_extension_is_native (extension))
      continue;
    for (guint j = 0; j < json_array_get_length (snapshot); j++)
      present |= !g_strcmp0 (ephy_web_extension_get_guid (extension),
                            json_object_get_string_member_with_default (json_array_get_object_element (snapshot, j), "id", ""));
    if (!present) {
      ephy_web_extension_native_set_state (extension, FALSE, FALSE, FALSE);
      remove_browser_action (self, extension);
      g_ptr_array_remove_index (self->web_extensions, i - 1);
    }
  }
  for (guint i = 0; i < self->web_extensions->len; i++) {
    EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);
    gboolean available = FALSE;
    gboolean enabled = FALSE;

    if (!ephy_web_extension_is_native (extension))
      continue;
    for (guint j = 0; j < json_array_get_length (snapshot); j++) {
      JsonObject *item = json_array_get_object_element (snapshot, j);

      if (g_strcmp0 (ephy_web_extension_get_guid (extension),
                     json_object_get_string_member_with_default (item, "id", "")))
        continue;
      available = json_object_get_boolean_member_with_default (item, "available", FALSE);
      enabled = available && json_object_get_boolean_member_with_default (item, "enabled", FALSE);
      break;
    }
    ephy_web_extension_native_set_state (extension, enabled, available,
                                         ephy_web_extension_native_is_pending (extension));
    if (available)
      ephy_web_extension_native_set_restart_required (extension, FALSE);
    if (enabled && (ephy_web_extension_has_browser_action (extension) ||
                    ephy_web_extension_get_option_ui_page (extension))) {
      if (!g_hash_table_contains (self->browser_action_map, extension)) {
        EphyBrowserAction *action = ephy_browser_action_new (extension);

        g_list_store_append (self->browser_actions, action);
        g_hash_table_insert (self->browser_action_map, extension, action);
      }
    } else {
      /* Removing the shared menu action dismisses its popup in every action bar. */
      remove_browser_action (self, extension);
    }
  }
  g_signal_emit (self, signals[CHANGED], 0);
}

static EphyWebExtension *
find_native_extension (EphyWebExtensionManager *self,
                       const char              *id)
{
  for (guint i = 0; i < self->web_extensions->len; i++) {
    EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);

    if (ephy_web_extension_is_native (extension) &&
        !g_strcmp0 (ephy_web_extension_get_guid (extension), id))
      return extension;
  }

  return NULL;
}

void
ephy_web_extension_manager_refresh_native_installed (EphyWebExtensionManager *self)
{
  g_return_if_fail (EPHY_IS_WEB_EXTENSION_MANAGER (self));
  native_extensions_changed (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()), self);
}

static void
native_inventory_ready (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (EphyWebExtensionManager) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonArray) snapshot = ephy_engine_extensions_query_finish (EPHY_ENGINE_SESSION (source), result, &error);

  self->native_query_pending = FALSE;
  if (snapshot)
    reconcile_native_extensions (self, snapshot);
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Could not query native extensions: %s", error->message);
  for (guint i = 0; i < self->web_extensions->len; i++) {
    EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);

    if (ephy_web_extension_is_native (extension))
      ephy_web_extension_native_set_state (extension,
                                           ephy_web_extension_native_is_enabled (extension),
                                           ephy_web_extension_native_is_available (extension), FALSE);
  }
  if (self->native_query_again) {
    self->native_query_again = FALSE;
    native_extensions_changed (EPHY_ENGINE_SESSION (source), self);
  }
}

static void
native_extensions_changed (EphyEngineSession      *session,
                            EphyWebExtensionManager *self)
{
  if (g_cancellable_is_cancelled (self->cancellable))
    return;
  if (self->native_query_pending) {
    self->native_query_again = TRUE;
    return;
  }
  self->native_query_pending = TRUE;
  ephy_engine_extensions_query_async (session, self->cancellable,
                                      native_inventory_ready, g_object_ref (self));
}

typedef struct {
  EphyWebExtension *extension;
  GError *error;
} NativeToggle;

static void
native_toggle_free (NativeToggle *toggle)
{
  g_object_unref (toggle->extension);
  g_clear_error (&toggle->error);
  g_free (toggle);
}

static void
native_toggle_reconciled (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  EphyWebExtensionManager *self = g_task_get_source_object (task);
  NativeToggle *toggle = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonArray) snapshot = ephy_engine_extensions_query_finish (EPHY_ENGINE_SESSION (source), result, &error);

  if (snapshot)
    reconcile_native_extensions (self, snapshot);
  else {
    /* A failed query is not proof that every extension was uninstalled. */
    for (guint i = 0; i < self->web_extensions->len; i++) {
      EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);

      if (ephy_web_extension_is_native (extension)) {
        ephy_web_extension_native_set_state (extension, FALSE, FALSE, FALSE);
        remove_browser_action (self, extension);
      }
    }
  }
  ephy_web_extension_native_set_state (toggle->extension,
                                       ephy_web_extension_native_is_enabled (toggle->extension),
                                       ephy_web_extension_native_is_available (toggle->extension), FALSE);
  g_task_return_error (task, g_steal_pointer (&toggle->error));
}

static void
native_toggle_ready (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  GTask *task = user_data;
  EphyWebExtensionManager *self = g_task_get_source_object (task);
  NativeToggle *toggle = g_task_get_task_data (task);
  g_autoptr (JsonArray) snapshot = ephy_engine_extensions_set_enabled_finish (EPHY_ENGINE_SESSION (source), result, &toggle->error);

  if (!snapshot) {
    /* Cancellation can arrive after Chromium mutates; reconcile actual state. */
    ephy_engine_extensions_query_async (EPHY_ENGINE_SESSION (source), self->cancellable,
                                        native_toggle_reconciled, task);
    return;
  }
  reconcile_native_extensions (self, snapshot);
  ephy_web_extension_native_set_state (toggle->extension,
                                       ephy_web_extension_native_is_enabled (toggle->extension),
                                       ephy_web_extension_native_is_available (toggle->extension), FALSE);
  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

void
ephy_web_extension_manager_set_native_enabled_async (EphyWebExtensionManager *self,
                                                     EphyWebExtension        *extension,
                                                     gboolean                 enabled,
                                                     GCancellable            *cancellable,
                                                     GAsyncReadyCallback      callback,
                                                     gpointer                 user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  NativeToggle *toggle;
  EphyEngineSession *session;

  g_task_set_source_tag (task, ephy_web_extension_manager_set_native_enabled_async);
  if (!g_ptr_array_find (self->web_extensions, extension, NULL) ||
      !ephy_web_extension_is_native (extension) ||
      !ephy_web_extension_native_is_available (extension)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "This extension is unavailable in the current browser profile");
    g_object_unref (task);
    return;
  }
  if (ephy_web_extension_native_is_pending (extension)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                             "An extension change is already in progress");
    g_object_unref (task);
    return;
  }
  toggle = g_new0 (NativeToggle, 1);
  toggle->extension = g_object_ref (extension);
  g_task_set_task_data (task, toggle, (GDestroyNotify)native_toggle_free);
  ephy_web_extension_native_set_state (extension, ephy_web_extension_native_is_enabled (extension), TRUE, TRUE);
  session = ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ());
  ephy_engine_extensions_set_enabled_async (session, ephy_web_extension_get_guid (extension), enabled,
                                           cancellable, native_toggle_ready, task);
}

gboolean
ephy_web_extension_manager_set_native_enabled_finish (EphyWebExtensionManager *self,
                                                      GAsyncResult            *result,
                                                      GError                 **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, ephy_web_extension_manager_set_native_enabled_async), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
ephy_web_extension_manager_constructed (GObject *object)
{
  EphyWebExtensionManager *self = EPHY_WEB_EXTENSION_MANAGER (object);
  g_autoptr (JsonArray) native_extensions = NULL;
  EphyEngineSession *session;

  G_OBJECT_CLASS (ephy_web_extension_manager_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  self->browser_action_map = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
  self->browser_actions = g_list_store_new (EPHY_TYPE_BROWSER_ACTION);
  self->web_extensions = g_ptr_array_new_full (0, g_object_unref);

  native_extensions = ephy_engine_extensions_dup_installed ();
  for (guint i = 0; i < json_array_get_length (native_extensions); i++) {
    EphyWebExtension *extension = ephy_web_extension_new_native (json_array_get_object_element (native_extensions, i));

    g_ptr_array_add (self->web_extensions, extension);
    ephy_web_extension_native_set_state (extension, FALSE, FALSE, TRUE);
  }

  session = ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ());
  g_signal_connect_object (session, "extensions-changed", G_CALLBACK (native_extensions_changed), self, 0);
  native_extensions_changed (session, self);
}

static void
ephy_web_extension_manager_dispose (GObject *object)
{
  EphyWebExtensionManager *self = EPHY_WEB_EXTENSION_MANAGER (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_list_store_remove_all (self->browser_actions);

  g_clear_object (&self->browser_actions);
  g_clear_pointer (&self->browser_action_map, g_hash_table_destroy);
  g_clear_pointer (&self->web_extensions, g_ptr_array_unref);

  G_OBJECT_CLASS (ephy_web_extension_manager_parent_class)->dispose (object);
}

static void
ephy_web_extension_manager_class_init (EphyWebExtensionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ephy_web_extension_manager_constructed;
  object_class->dispose = ephy_web_extension_manager_dispose;

  signals[CHANGED] =
    g_signal_new ("changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SHOW_BROWSER_ACTION] =
    g_signal_new ("show-browser-action",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1, EPHY_TYPE_BROWSER_ACTION);
}

static void
ephy_web_extension_manager_init (EphyWebExtensionManager *self)
{
  (void)self;
}

EphyWebExtensionManager *
ephy_web_extension_manager_get_default (void)
{
  static EphyWebExtensionManager *manager = NULL;

  if (!manager)
    manager = g_object_new (EPHY_TYPE_WEB_EXTENSION_MANAGER, NULL);

  return manager;
}

GPtrArray *
ephy_web_extension_manager_get_web_extensions (EphyWebExtensionManager *self)
{
  gboolean unavailable = FALSE;
  gboolean pending = self->native_query_pending;

  for (guint i = 0; i < self->web_extensions->len; i++) {
    EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);

    if (!ephy_web_extension_is_native (extension))
      continue;
    unavailable |= !ephy_web_extension_native_is_available (extension) &&
                   !ephy_web_extension_native_is_restart_required (extension);
    pending |= ephy_web_extension_native_is_pending (extension);
  }
  /* Opening the manager retries a transient startup failure without restarting. */
  if (unavailable && !pending) {
    self->native_query_pending = TRUE;
    for (guint i = 0; i < self->web_extensions->len; i++) {
      EphyWebExtension *extension = g_ptr_array_index (self->web_extensions, i);

      if (ephy_web_extension_is_native (extension))
        ephy_web_extension_native_set_state (extension,
                                             ephy_web_extension_native_is_enabled (extension),
                                             ephy_web_extension_native_is_available (extension), TRUE);
    }
    ephy_engine_extensions_query_async (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                        self->cancellable, native_inventory_ready, g_object_ref (self));
  }
  return self->web_extensions;
}

GtkWidget *
ephy_web_extension_manager_create_browser_popup (EphyWebExtensionManager *self,
                                                 EphyWebExtension        *extension,
                                                 EphyWebView             *owner)
{
  GtkWidget *view;
  g_autofree char *uri = NULL;
  const char *popup = ephy_web_extension_get_browser_popup (extension);

  if (!popup || !owner ||
      !ephy_web_extension_native_is_enabled (extension) ||
      !ephy_web_extension_native_is_available (extension) ||
      ephy_web_extension_native_is_pending (extension))
    return NULL;

  view = g_object_new (EPHY_TYPE_ENGINE_VIEW,
                        "network-session", ephy_engine_session_get_default (),
                        "settings", ephy_embed_prefs_get_settings (), NULL);
  if (!ephy_engine_web_view_set_extension_popup (EPHY_ENGINE_VIEW (view), EPHY_ENGINE_VIEW (owner))) {
    g_object_ref_sink (view);
    g_object_unref (view);
    return NULL;
  }
  uri = g_strdup_printf ("chrome-extension://%s/%s", ephy_web_extension_get_guid (extension), popup);
  /* Upstream uBlock accepts an explicit owning tab, including Web Store builds. */
  if (!strcmp (popup, "popup-fenix.html") &&
      !g_strcmp0 (ephy_web_extension_get_name (extension), "uBlock Origin")) {
    gint tab_id = ephy_engine_web_view_get_extension_tab_id (EPHY_ENGINE_VIEW (owner));

    g_free (uri);
    uri = g_strdup_printf ("chrome-extension://%s/%s?tabId=%d",
                            ephy_web_extension_get_guid (extension), popup, tab_id);
  }
  gtk_widget_set_size_request (view, 320, 80);
  gtk_widget_set_hexpand (view, TRUE);
  gtk_widget_set_vexpand (view, FALSE);
  /* Keep the native readiness/error surface visible before Chromium starts. */
  gtk_widget_set_visible (view, TRUE);
  ephy_engine_web_view_load_uri (EPHY_ENGINE_VIEW (view), uri);
  return view;
}

void
ephy_web_extension_manager_open_options (EphyWebExtensionManager *self,
                                        EphyWebExtension        *extension,
                                        EphyWindow              *window)
{
  const char *options = ephy_web_extension_get_option_ui_page (extension);
  g_autoptr (EphyEngineSession) session = NULL;
  GtkWidget *page;
  EphyEmbed *embed;
  g_autofree char *uri = NULL;

  if (!options || !EPHY_IS_WINDOW (window))
    return;
  if (ephy_web_extension_is_native (extension) &&
      (!ephy_web_extension_native_is_enabled (extension) ||
       !ephy_web_extension_native_is_available (extension) ||
       ephy_web_extension_native_is_pending (extension)))
    return;
  page = ephy_tab_view_get_selected_page (ephy_window_get_tab_view (window));
  if (!page)
    return;
  g_object_get (ephy_embed_get_web_view (EPHY_EMBED (page)), "network-session", &session, NULL);
  if (!session || ephy_engine_session_is_ephemeral (session))
    return;

  uri = g_strdup_printf ("chrome-extension://%s/%s",
                         ephy_web_extension_get_guid (extension), options);
  embed = ephy_shell_new_tab (ephy_shell_get_default (), window, NULL, EPHY_NEW_TAB_JUMP);
  ephy_web_view_load_url (ephy_embed_get_web_view (embed), uri);
}

static void
remove_browser_action (EphyWebExtensionManager *self,
                       EphyWebExtension        *web_extension)
{
  EphyBrowserAction *action;
  guint position;

  action = g_hash_table_lookup (self->browser_action_map, web_extension);
  if (!action)
    return;

  g_assert (g_list_store_find (self->browser_actions, action, &position));
  g_list_store_remove (self->browser_actions, position);

  g_hash_table_remove (self->browser_action_map, web_extension);
}

gboolean
ephy_web_extension_manager_is_active (EphyWebExtensionManager *self,
                                      EphyWebExtension        *extension)
{
  return ephy_web_extension_native_is_enabled (extension);
}

GListStore *
ephy_web_extension_manager_get_browser_actions (EphyWebExtensionManager *self)
{
  return self->browser_actions;
}
