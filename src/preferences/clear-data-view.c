/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2013 Red Hat, Inc.
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

#include "clear-data-view.h"

#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "ephy-embed-shell.h"
#include "ephy-history-service.h"
#include "ephy-settings.h"

struct _ClearDataView {
  EphyDataView parent_instance;

  GtkWidget *treeview;
  GtkTreeModel *treestore;
  GtkTreeModelFilter *treemodelfilter;
  GtkTreeViewColumn *active_column;
  GtkTreeViewColumn *name_column;

  GCancellable *cancellable;
  gboolean clearing;
};

enum {
  TYPE_COLUMN,
  ACTIVE_COLUMN,
  NAME_COLUMN,
  DATA_COLUMN,
  SENSITIVE_COLUMN,
  COOKIE_DOMAIN_COLUMN
};

G_DEFINE_FINAL_TYPE (ClearDataView, clear_data_view, EPHY_TYPE_DATA_VIEW)

#define PERSISTENT_DATA_TYPES WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE | \
        WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | \
        WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | \
        WEBKIT_WEBSITE_DATA_HSTS_CACHE | \
        WEBKIT_WEBSITE_DATA_ITP | \
        WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT | \
        WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS | \
        WEBKIT_WEBSITE_DATA_DOM_CACHE

/* Non-persistent data types are not managed by this dialog:
 *
 * WEBKIT_WEBSITE_DATA_MEMORY_CACHE
 * WEBKIT_WEBSITE_DATA_SESSION_STORAGE
 */

typedef struct {
  guint id;
  WebKitWebsiteDataTypes type;
  const char *name;
} DataEntry;

static const DataEntry data_entries[] = {
  { 0x001, WEBKIT_WEBSITE_DATA_COOKIES, N_("Cookies") },
  { 0x002, WEBKIT_WEBSITE_DATA_DISK_CACHE, N_("HTTP cache (all websites)") },
  { 0x004, WEBKIT_WEBSITE_DATA_LOCAL_STORAGE, N_("Local storage data") },
  { 0x008, WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE, N_("Offline web application cache") },
  { 0x010, WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES, N_("IndexedDB databases") },
  { 0x020, WEBKIT_WEBSITE_DATA_HSTS_CACHE, N_("HSTS policies cache") },
  { 0x040, WEBKIT_WEBSITE_DATA_ITP, N_("Intelligent Tracking Prevention data") },
  { 0x080, WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT, N_("MediaDevices device IDs") },
  { 0x100, WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS, N_("ServiceWorker registrations") },
  { 0x200, WEBKIT_WEBSITE_DATA_DOM_CACHE, N_("CacheStorage data") }
};

static WebKitWebsiteDataManager *
get_website_data_manager (void)
{
  return ephy_embed_shell_get_website_data_manager (ephy_embed_shell_get_default ());
}

static gboolean
uses_brave (void)
{
  return ephy_engine_session_uses_brave (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()));
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static gboolean any_item_checked (ClearDataView *self);

static gboolean
find_data_type (ClearDataView          *view,
                WebKitWebsiteDataTypes type,
                GtkTreeIter           *iter)
{
  if (!gtk_tree_model_get_iter_first (view->treestore, iter))
    return FALSE;
  do {
    guint row_type;

    gtk_tree_model_get (view->treestore, iter, TYPE_COLUMN, &row_type, -1);
    if (row_type == type)
      return TRUE;
  } while (gtk_tree_model_iter_next (view->treestore, iter));
  return FALSE;
}

static void
populate_website_data (ClearDataView *clear_data_view,
                       GList         *data_list)
{
  GtkTreeStore *treestore;
  int active_items;

  ephy_data_view_set_is_loading (EPHY_DATA_VIEW (clear_data_view), clear_data_view->clearing);

  ephy_data_view_set_has_data (EPHY_DATA_VIEW (clear_data_view), TRUE);
  active_items = g_settings_get_int (EPHY_SETTINGS_MAIN, EPHY_PREFS_ACTIVE_CLEAR_DATA_ITEMS);

  treestore = GTK_TREE_STORE (clear_data_view->treestore);
  for (guint i = 0; i < G_N_ELEMENTS (data_entries); i++) {
    GtkTreeIter parent_iter;
    gboolean is_cache = data_entries[i].type == WEBKIT_WEBSITE_DATA_DISK_CACHE;
    gboolean native = uses_brave ();
    gboolean empty = !is_cache && !native;
    const char *native_name = NULL;

    if (native) {
      switch (data_entries[i].type) {
        case WEBKIT_WEBSITE_DATA_LOCAL_STORAGE: native_name = _("Local storage (all websites)"); break;
        case WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES: native_name = _("IndexedDB databases (all websites)"); break;
        case WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS: native_name = _("Service workers (all websites)"); break;
        case WEBKIT_WEBSITE_DATA_DOM_CACHE: native_name = _("CacheStorage (all websites)"); break;
        default: break;
      }
    }
    if (native && !(data_entries[i].type & (WEBKIT_WEBSITE_DATA_COOKIES | WEBKIT_WEBSITE_DATA_DISK_CACHE |
        WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES |
        WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS | WEBKIT_WEBSITE_DATA_DOM_CACHE)))
      continue;

    if (data_entries[i].type == WEBKIT_WEBSITE_DATA_COOKIES)
      continue;
    if (find_data_type (clear_data_view, data_entries[i].type, &parent_iter)) {
      if (is_cache)
        continue;
      gtk_tree_store_remove (treestore, &parent_iter);
    }
    gtk_tree_store_insert_with_values (treestore, &parent_iter, NULL, -1,
                                       TYPE_COLUMN, data_entries[i].type,
                                       ACTIVE_COLUMN, active_items & data_entries[i].id,
                                       NAME_COLUMN, native_name ? native_name : _(data_entries[i].name),
                                       DATA_COLUMN, NULL,
                                       SENSITIVE_COLUMN, TRUE,
                                       -1);
    for (GList *l = data_list; !is_cache && l && l->data; l = g_list_next (l)) {
      WebKitWebsiteData *data = (WebKitWebsiteData *)l->data;

      if (!(webkit_website_data_get_types (data) & data_entries[i].type))
        continue;

      gtk_tree_store_insert_with_values (treestore, NULL, &parent_iter, -1,
                                         TYPE_COLUMN, data_entries[i].type,
                                         ACTIVE_COLUMN, active_items & data_entries[i].id,
                                         NAME_COLUMN, webkit_website_data_get_name (data),
                                         DATA_COLUMN, data,
                                         SENSITIVE_COLUMN, TRUE,
                                         -1);
      empty = FALSE;
    }

    if (empty)
      gtk_tree_store_remove (treestore, &parent_iter);
  }

  gtk_tree_model_filter_refilter (clear_data_view->treemodelfilter);
  if (ephy_data_view_get_search_text (EPHY_DATA_VIEW (clear_data_view)) &&
      *ephy_data_view_get_search_text (EPHY_DATA_VIEW (clear_data_view)))
    gtk_tree_view_expand_all (GTK_TREE_VIEW (clear_data_view->treeview));
  ephy_data_view_set_can_clear (EPHY_DATA_VIEW (clear_data_view), !clear_data_view->clearing && any_item_checked (clear_data_view));
}

static GWeakRef *
weak_view_new (ClearDataView *view)
{
  GWeakRef *weak = g_new0 (GWeakRef, 1);

  g_weak_ref_init (weak, view);
  return weak;
}

static ClearDataView *
weak_view_take (GWeakRef *weak)
{
  ClearDataView *view = g_weak_ref_get (weak);

  g_weak_ref_clear (weak);
  g_free (weak);
  return view;
}

static void
cookies_fetched_cb (EphyEngineSession *session,
                    GAsyncResult      *result,
                    GWeakRef          *weak)
{
  g_autoptr (ClearDataView) view = weak_view_take (weak);
  g_autoptr (GError) error = NULL;
  GList *cookies = ephy_engine_session_get_cookies_finish (session, result, &error);

  if (view && !g_cancellable_is_cancelled (view->cancellable)) {
    GtkTreeStore *store = GTK_TREE_STORE (view->treestore);
    GtkTreeIter parent;
    g_autoptr (GHashTable) domains = g_hash_table_new (g_str_hash, g_str_equal);
    gboolean active = g_settings_get_int (EPHY_SETTINGS_MAIN, EPHY_PREFS_ACTIVE_CLEAR_DATA_ITEMS) & 0x001;

    if (find_data_type (view, WEBKIT_WEBSITE_DATA_COOKIES, &parent))
      gtk_tree_store_remove (store, &parent);
    if (cookies)
      gtk_tree_store_insert_with_values (store, &parent, NULL, 0,
                                         TYPE_COLUMN, WEBKIT_WEBSITE_DATA_COOKIES,
                                         ACTIVE_COLUMN, active, NAME_COLUMN, _("Cookies"),
                                         SENSITIVE_COLUMN, TRUE, -1);
    for (GList *l = cookies; l; l = l->next) {
      const char *domain = soup_cookie_get_domain (l->data);

      if (!g_hash_table_add (domains, (gpointer)domain))
        continue;
      gtk_tree_store_insert_with_values (store, NULL, &parent, -1,
                                         TYPE_COLUMN, WEBKIT_WEBSITE_DATA_COOKIES,
                                         ACTIVE_COLUMN, active, NAME_COLUMN, domain,
                                         COOKIE_DOMAIN_COLUMN, domain,
                                         SENSITIVE_COLUMN, TRUE, -1);
    }
    gtk_tree_model_filter_refilter (view->treemodelfilter);
    if (ephy_data_view_get_search_text (EPHY_DATA_VIEW (view)) &&
        *ephy_data_view_get_search_text (EPHY_DATA_VIEW (view)))
      gtk_tree_view_expand_all (GTK_TREE_VIEW (view->treeview));
    ephy_data_view_set_can_clear (EPHY_DATA_VIEW (view), !view->clearing && any_item_checked (view));
    if (error)
      adw_toast_overlay_add_toast (ephy_data_view_get_toast_overlay (EPHY_DATA_VIEW (view)),
                                   adw_toast_new (_("Could not load cookies")));
  }
  g_list_free_full (cookies, (GDestroyNotify)soup_cookie_free);
}

static void
fetch_cookies (ClearDataView *view)
{
  ephy_engine_session_get_cookies (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                   NULL, view->cancellable, (GAsyncReadyCallback)cookies_fetched_cb, weak_view_new (view));
}

static void
website_data_fetched_cb (WebKitWebsiteDataManager *manager,
                         GAsyncResult             *result,
                         GWeakRef                 *weak)
{
  g_autoptr (ClearDataView) clear_data_view = weak_view_take (weak);
  GError *error = NULL;
  GList *data_list = webkit_website_data_manager_fetch_finish (manager, result, &error);

  if (!clear_data_view || g_cancellable_is_cancelled (clear_data_view->cancellable)) {
    g_clear_error (&error);
    g_list_free_full (data_list, (GDestroyNotify)webkit_website_data_unref);
    return;
  }
  populate_website_data (clear_data_view, data_list);
  g_list_free_full (data_list, (GDestroyNotify)webkit_website_data_unref);
  if (error) {
    adw_toast_overlay_add_toast (ephy_data_view_get_toast_overlay (EPHY_DATA_VIEW (clear_data_view)),
                                 adw_toast_new (_("Could not load website data")));
    g_error_free (error);
  }
}

typedef struct {
  GWeakRef view;
  guint pending;
  GError *error;
  gboolean reload_sites;
  gboolean reload_cookies;
} ClearOperation;

static void
clear_operation_done (ClearOperation *operation,
                       GError         *error)
{
  ClearDataView *view;

  if (error && !operation->error)
    operation->error = error;
  else
    g_clear_error (&error);
  if (--operation->pending)
    return;

  view = g_weak_ref_get (&operation->view);
  if (view) {
    view->clearing = FALSE;
    if (!g_error_matches (operation->error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      AdwToast *toast = adw_toast_new (operation->error ? _("Could not clear website data") : _("Data cleared"));
      adw_toast_overlay_add_toast (ephy_data_view_get_toast_overlay (EPHY_DATA_VIEW (view)), toast);
      ephy_data_view_set_is_loading (EPHY_DATA_VIEW (view), operation->reload_sites);
      ephy_data_view_set_can_clear (EPHY_DATA_VIEW (view), !operation->reload_sites && any_item_checked (view));
      if (operation->reload_sites && uses_brave ())
        populate_website_data (view, NULL);
      else if (operation->reload_sites)
        webkit_website_data_manager_fetch (get_website_data_manager (), PERSISTENT_DATA_TYPES,
                                           view->cancellable, (GAsyncReadyCallback)website_data_fetched_cb, weak_view_new (view));
      if (operation->reload_cookies)
        fetch_cookies (view);
    }
    g_object_unref (view);
  }
  g_weak_ref_clear (&operation->view);
  g_clear_error (&operation->error);
  g_free (operation);
}

static void
cookies_cleared_cb (EphyEngineSession *session,
                    GAsyncResult      *result,
                    ClearOperation    *operation)
{
  GError *error = NULL;

  ephy_engine_session_clear_cookies_finish (session, result, &error);
  clear_operation_done (operation, error);
}

static void
cache_cleared_cb (EphyEngineSession *session,
                  GAsyncResult      *result,
                  ClearOperation    *operation)
{
  GError *error = NULL;
  ephy_engine_session_clear_cache_finish (session, result, &error);
  clear_operation_done (operation, error);
}

static void
native_storage_cleared_cb (EphyEngineSession *session,
                          GAsyncResult *result,
                          ClearOperation *operation)
{
  GError *error = NULL;
  ephy_engine_session_clear_website_storage_finish (session, result, &error);
  clear_operation_done (operation, error);
}

static void
website_data_cleared_cb (WebKitWebsiteDataManager *manager,
                         GAsyncResult             *result,
                         ClearOperation           *operation)
{
  GError *error = NULL;
  webkit_website_data_manager_clear_finish (manager, result, &error);
  clear_operation_done (operation, error);
}

static void
website_data_removed_cb (WebKitWebsiteDataManager *manager,
                         GAsyncResult             *result,
                         ClearOperation           *operation)
{
  GError *error = NULL;
  webkit_website_data_manager_remove_finish (manager, result, &error);
  clear_operation_done (operation, error);
}

static gboolean
all_children_visible (GtkTreeModel       *model,
                      GtkTreeIter        *child_iter,
                      GtkTreeModelFilter *filter)
{
  GtkTreeIter filter_iter;

  gtk_tree_model_filter_convert_child_iter_to_iter (filter, &filter_iter, child_iter);
  return gtk_tree_model_iter_n_children (model, child_iter) == gtk_tree_model_iter_n_children (GTK_TREE_MODEL (filter), &filter_iter);
}

static void
on_clear_button_clicked (ClearDataView *clear_data_view)
{
  GtkTreeIter top_iter;
  WebKitWebsiteDataTypes types_to_clear = 0;
  GList *data_to_remove = NULL;
  GList *cookie_domains = NULL;
  WebKitWebsiteDataTypes types_to_remove = 0;
  ClearOperation *operation;

  if (!gtk_tree_model_get_iter_first (clear_data_view->treestore, &top_iter))
    return;

  do {
    guint type;
    gboolean active;
    GtkTreeIter child_iter;
    GtkTreeIter filter_iter;

    if (!gtk_tree_model_filter_convert_child_iter_to_iter (clear_data_view->treemodelfilter, &filter_iter, &top_iter))
      continue;

    gtk_tree_model_get (clear_data_view->treestore, &top_iter,
                        TYPE_COLUMN, &type,
                        ACTIVE_COLUMN, &active,
                        -1);
    if (active && all_children_visible (clear_data_view->treestore, &top_iter, clear_data_view->treemodelfilter)) {
      types_to_clear |= type;
    } else if (gtk_tree_model_iter_children (clear_data_view->treestore, &child_iter, &top_iter)) {
      gboolean empty = TRUE;

      do {
        WebKitWebsiteData *data = NULL;
        char *domain = NULL;
        GtkTreeIter filter_iter;

        if (gtk_tree_model_filter_convert_child_iter_to_iter (clear_data_view->treemodelfilter, &filter_iter, &child_iter)) {
          gtk_tree_model_get (clear_data_view->treestore, &child_iter,
                              ACTIVE_COLUMN, &active,
                              DATA_COLUMN, &data,
                              COOKIE_DOMAIN_COLUMN, &domain,
                              -1);

          if (active) {
            if (type == WEBKIT_WEBSITE_DATA_COOKIES)
              cookie_domains = g_list_prepend (cookie_domains, g_steal_pointer (&domain));
            else
              data_to_remove = g_list_prepend (data_to_remove, data);
            empty = FALSE;
          } else if (data)
            webkit_website_data_unref (data);
          g_free (domain);
        }
      } while (gtk_tree_model_iter_next (clear_data_view->treestore, &child_iter));

      if (!empty && type != WEBKIT_WEBSITE_DATA_COOKIES)
        types_to_remove |= type;
    }
  } while (gtk_tree_model_iter_next (clear_data_view->treestore, &top_iter));

  if (!types_to_clear && !types_to_remove && !cookie_domains)
    return;

  operation = g_new0 (ClearOperation, 1);
  g_weak_ref_init (&operation->view, clear_data_view);
  operation->pending = 1;
  operation->reload_sites = (types_to_clear & ~(WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_COOKIES)) || types_to_remove;
  operation->reload_cookies = (types_to_clear & WEBKIT_WEBSITE_DATA_COOKIES) || cookie_domains;
  clear_data_view->clearing = TRUE;
  ephy_data_view_set_can_clear (EPHY_DATA_VIEW (clear_data_view), FALSE);
  ephy_data_view_set_is_loading (EPHY_DATA_VIEW (clear_data_view), TRUE);

  if (types_to_clear & WEBKIT_WEBSITE_DATA_DISK_CACHE) {
    operation->pending++;
    ephy_engine_session_clear_cache (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                     clear_data_view->cancellable, (GAsyncReadyCallback)cache_cleared_cb, operation);
    types_to_clear &= ~WEBKIT_WEBSITE_DATA_DISK_CACHE;
  }
  if (types_to_clear & WEBKIT_WEBSITE_DATA_COOKIES) {
    operation->pending++;
    ephy_engine_session_clear_cookies (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                       NULL, clear_data_view->cancellable, (GAsyncReadyCallback)cookies_cleared_cb, operation);
    types_to_clear &= ~WEBKIT_WEBSITE_DATA_COOKIES;
  } else {
    for (GList *l = cookie_domains; l; l = l->next) {
      operation->pending++;
      ephy_engine_session_clear_cookies (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
                                         l->data, clear_data_view->cancellable, (GAsyncReadyCallback)cookies_cleared_cb, operation);
    }
  }
  g_list_free_full (cookie_domains, g_free);
  if (types_to_clear && uses_brave ()) {
    const char *types[5] = { NULL };
    guint count = 0;
    if (types_to_clear & WEBKIT_WEBSITE_DATA_LOCAL_STORAGE) types[count++] = "localStorage";
    if (types_to_clear & WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES) types[count++] = "indexedDB";
    if (types_to_clear & WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS) types[count++] = "serviceWorkers";
    if (types_to_clear & WEBKIT_WEBSITE_DATA_DOM_CACHE) types[count++] = "cacheStorage";
    operation->pending++;
    ephy_engine_session_clear_website_storage (ephy_embed_shell_get_network_session (ephy_embed_shell_get_default ()),
      types, clear_data_view->cancellable, (GAsyncReadyCallback)native_storage_cleared_cb, operation);
  } else if (types_to_clear) {
    operation->pending++;
    webkit_website_data_manager_clear (get_website_data_manager (),
                                       types_to_clear, 0,
                                       clear_data_view->cancellable, (GAsyncReadyCallback)website_data_cleared_cb, operation);
  }

  if (types_to_remove) {
    operation->pending++;
    webkit_website_data_manager_remove (get_website_data_manager (),
                                        types_to_remove, data_to_remove,
                                        clear_data_view->cancellable, (GAsyncReadyCallback)website_data_removed_cb, operation);
  }

  g_list_free_full (data_to_remove, (GDestroyNotify)webkit_website_data_unref);

  clear_operation_done (operation, NULL);
}

static gboolean
any_item_checked (ClearDataView *self)
{
  GtkTreeIter top_iter;

  if (!gtk_tree_model_get_iter_first (self->treestore, &top_iter))
    return FALSE;

  do {
    gboolean active;
    GtkTreeIter child_iter;
    GtkTreeIter filter_iter;

    if (!gtk_tree_model_filter_convert_child_iter_to_iter (self->treemodelfilter, &filter_iter, &top_iter))
      continue;

    gtk_tree_model_get (self->treestore, &top_iter,
                        ACTIVE_COLUMN, &active, -1);
    if (active && !gtk_tree_model_iter_has_child (self->treestore, &top_iter)) {
      return TRUE;
    } else if (gtk_tree_model_iter_children (self->treestore, &child_iter, &top_iter)) {
      do {
        GtkTreeIter filter_iter;

        if (gtk_tree_model_filter_convert_child_iter_to_iter (self->treemodelfilter, &filter_iter, &child_iter)) {
          gtk_tree_model_get (self->treestore, &child_iter,
                              ACTIVE_COLUMN, &active, -1);
          if (active)
            return TRUE;
        }
      } while (gtk_tree_model_iter_next (self->treestore, &child_iter));
    }
  } while (gtk_tree_model_iter_next (self->treestore, &top_iter));

  return FALSE;
}

static void
item_toggled_cb (GtkCellRendererToggle *renderer,
                 const char            *path_str,
                 ClearDataView         *clear_data_view)
{
  GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
  GtkTreeIter filter_iter, iter;
  GtkTreeIter parent_iter;
  gboolean active;

  gtk_tree_model_get_iter (GTK_TREE_MODEL (clear_data_view->treemodelfilter),
                           &filter_iter, path);
  gtk_tree_model_filter_convert_iter_to_child_iter (clear_data_view->treemodelfilter,
                                                    &iter, &filter_iter);
  gtk_tree_model_get (clear_data_view->treestore, &iter,
                      ACTIVE_COLUMN, &active,
                      -1);
  gtk_tree_store_set (GTK_TREE_STORE (clear_data_view->treestore), &iter,
                      ACTIVE_COLUMN, !active,
                      -1);

  if (!gtk_tree_model_iter_parent (clear_data_view->treestore, &parent_iter, &iter)) {
    GtkTreeIter child_iter;
    g_autofree char *name = NULL;
    int active_items;

    active_items = g_settings_get_int (EPHY_SETTINGS_MAIN, EPHY_PREFS_ACTIVE_CLEAR_DATA_ITEMS);

    gtk_tree_model_get (clear_data_view->treestore, &iter,
                        NAME_COLUMN, &name,
                        -1);
    for (guint i = 0; i < G_N_ELEMENTS (data_entries); i++) {
      if (g_strcmp0 (gettext (data_entries[i].name), name) == 0) {
        if (active)
          active_items &= ~data_entries[i].id;
        else
          active_items |= data_entries[i].id;

        break;
      }
    }

    g_settings_set_int (EPHY_SETTINGS_MAIN, EPHY_PREFS_ACTIVE_CLEAR_DATA_ITEMS, active_items);

    if (gtk_tree_model_iter_children (clear_data_view->treestore, &child_iter, &iter)) {
      do {
        gtk_tree_store_set (GTK_TREE_STORE (clear_data_view->treestore), &child_iter,
                            ACTIVE_COLUMN, !active,
                            -1);
      } while (gtk_tree_model_iter_next (clear_data_view->treestore, &child_iter));
    }
  } else {
    /* Update the parent */
    if (active) {
      /* When unchecking a child we know the parent should be unchecked too */
      gtk_tree_store_set (GTK_TREE_STORE (clear_data_view->treestore), &parent_iter,
                          ACTIVE_COLUMN, FALSE,
                          -1);
    } else {
      GtkTreeIter child_iter;
      gboolean all_active = TRUE;

      /* When checking a child, parent should be checked if all its children are */
      gtk_tree_model_iter_children (clear_data_view->treestore, &child_iter, &parent_iter);
      do {
        gtk_tree_model_get (clear_data_view->treestore, &child_iter,
                            ACTIVE_COLUMN, &all_active,
                            -1);
      } while (all_active && gtk_tree_model_iter_next (clear_data_view->treestore, &child_iter));

      if (all_active) {
        gtk_tree_store_set (GTK_TREE_STORE (clear_data_view->treestore), &parent_iter,
                            ACTIVE_COLUMN, TRUE,
                            -1);
      }
    }
  }

  gtk_tree_path_free (path);

  ephy_data_view_set_can_clear (EPHY_DATA_VIEW (clear_data_view), any_item_checked (clear_data_view));
}

static void
search_text_changed_cb (ClearDataView *clear_data_view)
{
  gtk_tree_model_filter_refilter (clear_data_view->treemodelfilter);
  if (ephy_data_view_get_search_text (EPHY_DATA_VIEW (clear_data_view)) &&
      *ephy_data_view_get_search_text (EPHY_DATA_VIEW (clear_data_view)))
    gtk_tree_view_expand_all (GTK_TREE_VIEW (clear_data_view->treeview));
  ephy_data_view_set_can_clear (EPHY_DATA_VIEW (clear_data_view),
                               !ephy_data_view_get_is_loading (EPHY_DATA_VIEW (clear_data_view)) && any_item_checked (clear_data_view));
}

static gboolean
row_visible_func (GtkTreeModel  *model,
                  GtkTreeIter   *iter,
                  ClearDataView *clear_data_view)
{
  const char *search_text;
  char *name;
  gboolean visible;
  GtkTreeIter parent_iter;

  /* Category visibility must not depend on children still being inserted. */
  if (!gtk_tree_model_iter_parent (model, &parent_iter, iter)) {
    guint type;

    gtk_tree_model_get (model, iter, TYPE_COLUMN, &type, -1);
    if (type != WEBKIT_WEBSITE_DATA_DISK_CACHE)
      return TRUE;
  }

  search_text = ephy_data_view_get_search_text (EPHY_DATA_VIEW (clear_data_view));
  if (!search_text || search_text[0] == '\0')
    return TRUE;

  gtk_tree_model_get (model, iter,
                      NAME_COLUMN, &name,
                      -1);

  visible = name && strstr (name, search_text);
  g_free (name);

  return visible;
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void
clear_data_view_dispose (GObject *object)
{
  ClearDataView *clear_data_view = (ClearDataView *)object;

  if (clear_data_view->cancellable) {
    g_cancellable_cancel (clear_data_view->cancellable);
    g_clear_object (&clear_data_view->cancellable);
  }

  G_OBJECT_CLASS (clear_data_view_parent_class)->dispose (object);
}

static void
clear_data_view_class_init (ClearDataViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = clear_data_view_dispose;

  g_type_ensure (WEBKIT_TYPE_WEBSITE_DATA);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/epiphany/gtk/clear-data-view.ui");

  gtk_widget_class_bind_template_child (widget_class, ClearDataView, treeview);
  gtk_widget_class_bind_template_child (widget_class, ClearDataView, active_column);
  gtk_widget_class_bind_template_child (widget_class, ClearDataView, name_column);

  gtk_widget_class_bind_template_callback (widget_class, item_toggled_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_clear_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, search_text_changed_cb);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
clear_data_view_init (ClearDataView *clear_data_view)
{
  GtkCellRenderer *cell_renderer;

  gtk_widget_init_template (GTK_WIDGET (clear_data_view));

  cell_renderer = gtk_cell_renderer_toggle_new ();
  g_signal_connect (cell_renderer, "toggled", G_CALLBACK (item_toggled_cb), clear_data_view);
  gtk_tree_view_column_pack_start (clear_data_view->active_column, cell_renderer, TRUE);
  gtk_tree_view_column_set_attributes (clear_data_view->active_column, cell_renderer, "active", 1, "sensitive", 4, NULL);

  cell_renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (clear_data_view->name_column, cell_renderer, TRUE);
  gtk_tree_view_column_set_attributes (clear_data_view->name_column, cell_renderer, "text", 2, "sensitive", 4, NULL);

  clear_data_view->treestore = GTK_TREE_MODEL (gtk_tree_store_new (6, G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_STRING, WEBKIT_TYPE_WEBSITE_DATA, G_TYPE_BOOLEAN, G_TYPE_STRING));
  clear_data_view->treemodelfilter = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (clear_data_view->treestore, NULL));
  gtk_tree_view_set_model (GTK_TREE_VIEW (clear_data_view->treeview), GTK_TREE_MODEL (clear_data_view->treemodelfilter));
  /* Search is handled by the page's GtkSearchBar. */
  gtk_tree_view_set_enable_search (GTK_TREE_VIEW (clear_data_view->treeview), FALSE);

  gtk_tree_model_filter_set_visible_func (clear_data_view->treemodelfilter,
                                          (GtkTreeModelFilterVisibleFunc)row_visible_func,
                                          clear_data_view,
                                          NULL);

  populate_website_data (clear_data_view, NULL);

  clear_data_view->cancellable = g_cancellable_new ();
  fetch_cookies (clear_data_view);

  if (!uses_brave ())
    webkit_website_data_manager_fetch (get_website_data_manager (),
                                     PERSISTENT_DATA_TYPES,
                                     clear_data_view->cancellable,
                                     (GAsyncReadyCallback)website_data_fetched_cb,
                                     weak_view_new (clear_data_view));
}
G_GNUC_END_IGNORE_DEPRECATIONS
