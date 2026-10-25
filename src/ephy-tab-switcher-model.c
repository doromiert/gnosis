/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "ephy-tab-switcher-model.h"
#include "ephy-embed.h"
#include "ephy-engine-view.h"
#include "ephy-prefs.h"

struct _EphyTabSwitcherItem {
  GObject parent_instance;
  guint64 id;
  char *title;
  GdkPaintable *thumbnail;
  AdwTabPage *page;
};

G_DEFINE_FINAL_TYPE (EphyTabSwitcherItem, ephy_tab_switcher_item, G_TYPE_OBJECT)

enum { ITEM_PROP_0, ITEM_PROP_ID, ITEM_PROP_TITLE, ITEM_PROP_THUMBNAIL, ITEM_N_PROPS };
static GParamSpec *item_properties[ITEM_N_PROPS];

static void
item_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
  EphyTabSwitcherItem *self = EPHY_TAB_SWITCHER_ITEM (object);
  switch (property_id) {
    case ITEM_PROP_ID: g_value_set_uint64 (value, self->id); break;
    case ITEM_PROP_TITLE: g_value_set_string (value, self->title); break;
    case ITEM_PROP_THUMBNAIL: g_value_set_object (value, self->thumbnail); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
item_finalize (GObject *object)
{
  EphyTabSwitcherItem *self = EPHY_TAB_SWITCHER_ITEM (object);
  g_clear_object (&self->page);
  g_clear_object (&self->thumbnail);
  g_free (self->title);
  G_OBJECT_CLASS (ephy_tab_switcher_item_parent_class)->finalize (object);
}

static void
ephy_tab_switcher_item_class_init (EphyTabSwitcherItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = item_get_property;
  object_class->finalize = item_finalize;
  item_properties[ITEM_PROP_ID] = g_param_spec_uint64 ("id", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  item_properties[ITEM_PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  item_properties[ITEM_PROP_THUMBNAIL] = g_param_spec_object ("thumbnail", NULL, NULL, GDK_TYPE_PAINTABLE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, ITEM_N_PROPS, item_properties);
}

static void ephy_tab_switcher_item_init (EphyTabSwitcherItem *self) { (void)self; }
guint64 ephy_tab_switcher_item_get_id (EphyTabSwitcherItem *self) { return self->id; }
const char *ephy_tab_switcher_item_get_title (EphyTabSwitcherItem *self) { return self->title; }
GdkPaintable *ephy_tab_switcher_item_get_thumbnail (EphyTabSwitcherItem *self) { return self->thumbnail; }

struct _EphyTabSwitcherModel {
  GObject parent_instance;
  GWeakRef view;
  GSettings *settings;
  GListStore *items;
  GPtrArray *recent;
  GCancellable *capture_cancel;
  guint capture_source;
  guint capture_index;
  guint capture_start;
  guint selected_index;
  guint64 gesture;
  gboolean active;
};

G_DEFINE_FINAL_TYPE (EphyTabSwitcherModel, ephy_tab_switcher_model, G_TYPE_OBJECT)

enum { MODEL_PROP_0, MODEL_PROP_ACTIVE, MODEL_PROP_SELECTED_INDEX, MODEL_N_PROPS };
static GParamSpec *model_properties[MODEL_N_PROPS];
static guint closed_signal;
static guint64 next_page_id = 1;

static gboolean
contains_page (AdwTabView *view, AdwTabPage *page)
{
  for (int i = 0; i < adw_tab_view_get_n_pages (view); i++)
    if (adw_tab_view_get_nth_page (view, i) == page)
      return TRUE;
  return FALSE;
}

static void
remember_page (EphyTabSwitcherModel *self, AdwTabPage *page)
{
  if (!page)
    return;
  g_ptr_array_remove (self->recent, page);
  g_ptr_array_insert (self->recent, 0, page);
}

static void
selected_page_changed (AdwTabView *view, GParamSpec *pspec, EphyTabSwitcherModel *self)
{
  (void)pspec;
  /* Selection changes outside a gesture commit cancel the frozen candidate list. */
  ephy_tab_switcher_model_cancel (self);
  remember_page (self, adw_tab_view_get_selected_page (view));
}

static void
page_attached (AdwTabView *view, AdwTabPage *page, int position, EphyTabSwitcherModel *self)
{
  (void)view; (void)position;
  if (!g_ptr_array_find (self->recent, page, NULL))
    g_ptr_array_add (self->recent, page);
}

static void
page_detached (AdwTabView *view, AdwTabPage *page, int position, EphyTabSwitcherModel *self)
{
  (void)view; (void)position;
  ephy_tab_switcher_model_cancel (self);
  g_ptr_array_remove (self->recent, page);
}

static void
model_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
  EphyTabSwitcherModel *self = EPHY_TAB_SWITCHER_MODEL (object);
  switch (property_id) {
    case MODEL_PROP_ACTIVE: g_value_set_boolean (value, self->active); break;
    case MODEL_PROP_SELECTED_INDEX: g_value_set_uint (value, self->selected_index); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
model_dispose (GObject *object)
{
  EphyTabSwitcherModel *self = EPHY_TAB_SWITCHER_MODEL (object);
  ephy_tab_switcher_model_cancel (self);
  g_clear_object (&self->settings);
  g_clear_object (&self->items);
  g_clear_pointer (&self->recent, g_ptr_array_unref);
  G_OBJECT_CLASS (ephy_tab_switcher_model_parent_class)->dispose (object);
}

static void
model_finalize (GObject *object)
{
  g_weak_ref_clear (&EPHY_TAB_SWITCHER_MODEL (object)->view);
  G_OBJECT_CLASS (ephy_tab_switcher_model_parent_class)->finalize (object);
}

static void
ephy_tab_switcher_model_class_init (EphyTabSwitcherModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = model_get_property;
  object_class->dispose = model_dispose;
  object_class->finalize = model_finalize;
  model_properties[MODEL_PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  model_properties[MODEL_PROP_SELECTED_INDEX] = g_param_spec_uint ("selected-index", NULL, NULL, 0, G_MAXUINT, GTK_INVALID_LIST_POSITION, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, MODEL_N_PROPS, model_properties);
  closed_signal = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ephy_tab_switcher_model_init (EphyTabSwitcherModel *self)
{
  g_weak_ref_init (&self->view, NULL);
  self->items = g_list_store_new (EPHY_TYPE_TAB_SWITCHER_ITEM);
  self->recent = g_ptr_array_new ();
  self->selected_index = GTK_INVALID_LIST_POSITION;
}

EphyTabSwitcherModel *
ephy_tab_switcher_model_new (AdwTabView *view, GSettings *settings)
{
  EphyTabSwitcherModel *self;
  g_return_val_if_fail (ADW_IS_TAB_VIEW (view), NULL);
  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  self = g_object_new (EPHY_TYPE_TAB_SWITCHER_MODEL, NULL);
  g_weak_ref_set (&self->view, view);
  self->settings = g_object_ref (settings);
  for (int i = 0; i < adw_tab_view_get_n_pages (view); i++)
    g_ptr_array_add (self->recent, adw_tab_view_get_nth_page (view, i));
  remember_page (self, adw_tab_view_get_selected_page (view));
  g_signal_connect_object (view, "notify::selected-page", G_CALLBACK (selected_page_changed), self, 0);
  g_signal_connect_object (view, "page-attached", G_CALLBACK (page_attached), self, 0);
  g_signal_connect_object (view, "page-detached", G_CALLBACK (page_detached), self, 0);
  return self;
}

GListModel *ephy_tab_switcher_model_get_items (EphyTabSwitcherModel *self) { return G_LIST_MODEL (self->items); }
gboolean ephy_tab_switcher_model_get_active (EphyTabSwitcherModel *self) { return self->active; }
guint ephy_tab_switcher_model_get_selected_index (EphyTabSwitcherModel *self) { return self->selected_index; }

static void
finish (EphyTabSwitcherModel *self, gboolean commit)
{
  g_autoptr (AdwTabView) view = NULL;
  g_autoptr (EphyTabSwitcherItem) selected = NULL;
  if (!self->active)
    return;
  if (commit && self->selected_index < g_list_model_get_n_items (G_LIST_MODEL (self->items)))
    selected = g_list_model_get_item (G_LIST_MODEL (self->items), self->selected_index);
  self->active = FALSE;
  self->gesture++;
  g_clear_handle_id (&self->capture_source, g_source_remove);
  if (self->capture_cancel)
    g_cancellable_cancel (self->capture_cancel);
  g_clear_object (&self->capture_cancel);
  g_object_notify_by_pspec (G_OBJECT (self), model_properties[MODEL_PROP_ACTIVE]);
  view = g_weak_ref_get (&self->view);
  if (selected && view && contains_page (view, selected->page))
    adw_tab_view_set_selected_page (view, selected->page);
  self->selected_index = GTK_INVALID_LIST_POSITION;
  g_list_store_remove_all (self->items);
  g_object_notify_by_pspec (G_OBJECT (self), model_properties[MODEL_PROP_SELECTED_INDEX]);
  g_signal_emit (self, closed_signal, 0);
}

void ephy_tab_switcher_model_commit (EphyTabSwitcherModel *self) { finish (self, TRUE); }
void ephy_tab_switcher_model_cancel (EphyTabSwitcherModel *self) { finish (self, FALSE); }

void
ephy_tab_switcher_model_move (EphyTabSwitcherModel *self, int delta)
{
  guint count = g_list_model_get_n_items (G_LIST_MODEL (self->items));
  if (!self->active || count == 0)
    return;
  self->selected_index = ((gint64)self->selected_index + delta % (gint64)count + count) % count;
  g_object_notify_by_pspec (G_OBJECT (self), model_properties[MODEL_PROP_SELECTED_INDEX]);
}

void
ephy_tab_switcher_model_activate (EphyTabSwitcherModel *self, guint index)
{
  if (!self->active || index >= g_list_model_get_n_items (G_LIST_MODEL (self->items)))
    return;
  self->selected_index = index;
  ephy_tab_switcher_model_commit (self);
}

static void
append_page (EphyTabSwitcherModel *self, AdwTabPage *page)
{
  guint64 *id = g_object_get_data (G_OBJECT (page), "jupiter-switcher-page-id");
  EphyTabSwitcherItem *item = g_object_new (EPHY_TYPE_TAB_SWITCHER_ITEM, NULL);
  if (!id) {
    id = g_new (guint64, 1);
    *id = next_page_id++;
    g_object_set_data_full (G_OBJECT (page), "jupiter-switcher-page-id", id, g_free);
  }
  item->id = *id;
  item->title = g_strdup (adw_tab_page_get_title (page));
  item->page = g_object_ref (page);
  g_list_store_append (self->items, item);
  g_object_unref (item);
}

typedef struct {
  GWeakRef model;
  EphyTabSwitcherItem *item;
  guint64 gesture;
} SnapshotData;

static gboolean capture_next (gpointer user_data);

static void
schedule_capture (EphyTabSwitcherModel *self)
{
  if (self->active && !self->capture_source)
    self->capture_source = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, capture_next, g_object_ref (self), g_object_unref);
}

static void
snapshot_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SnapshotData *data = user_data;
  g_autoptr (EphyTabSwitcherModel) self = g_weak_ref_get (&data->model);
  g_autoptr (GdkTexture) texture = ephy_engine_web_view_get_snapshot_finish (EPHY_ENGINE_VIEW (source), result, NULL);
  if (self && self->active && self->gesture == data->gesture && texture) {
    /* Bound retained preview memory rather than keeping full-size page captures. */
    GdkTextureDownloader *downloader = gdk_texture_downloader_new (texture);
    gsize stride;
    g_autoptr (GBytes) bytes = NULL;
    int width, height;
    g_autoptr (GdkPixbuf) pixbuf = NULL;
    double scale;
    g_autoptr (GdkPixbuf) small = NULL;

    gdk_texture_downloader_set_format (downloader, GDK_MEMORY_R8G8B8A8);
    bytes = gdk_texture_downloader_download_bytes (downloader, &stride);
    gdk_texture_downloader_free (downloader);
    width = gdk_texture_get_width (texture);
    height = gdk_texture_get_height (texture);
    pixbuf = gdk_pixbuf_new_from_bytes (bytes, GDK_COLORSPACE_RGB, TRUE, 8, width, height, stride);
    scale = MIN (1.0, MIN (160.0 / width, 100.0 / height));
    small = gdk_pixbuf_scale_simple (pixbuf, MAX (1, (int)(width * scale)), MAX (1, (int)(height * scale)), GDK_INTERP_BILINEAR);
    if (small) {
      g_autoptr (GdkTexture) thumbnail = gdk_texture_new_for_pixbuf (small);
      g_set_object (&data->item->thumbnail, GDK_PAINTABLE (thumbnail));
      g_object_notify_by_pspec (G_OBJECT (data->item), item_properties[ITEM_PROP_THUMBNAIL]);
    }
  }
  if (self && self->active && self->gesture == data->gesture)
    schedule_capture (self);
  g_weak_ref_clear (&data->model);
  g_object_unref (data->item);
  g_free (data);
}

static gboolean
capture_next (gpointer user_data)
{
  EphyTabSwitcherModel *self = user_data;
  self->capture_source = 0;
  if (!self->active)
    return G_SOURCE_REMOVE;
  while (self->capture_index < g_list_model_get_n_items (G_LIST_MODEL (self->items))) {
    guint index = (self->capture_start + self->capture_index++) % g_list_model_get_n_items (G_LIST_MODEL (self->items));
    g_autoptr (EphyTabSwitcherItem) item = g_list_model_get_item (G_LIST_MODEL (self->items), index);
    GtkWidget *child = adw_tab_page_get_child (item->page);
    if (EPHY_IS_EMBED (child)) {
      SnapshotData *data = g_new0 (SnapshotData, 1);
      g_weak_ref_init (&data->model, self);
      data->item = g_object_ref (item);
      data->gesture = self->gesture;
      ephy_engine_web_view_get_snapshot (EPHY_ENGINE_VIEW (ephy_embed_get_web_view (EPHY_EMBED (child))),
                                         WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                         self->capture_cancel, snapshot_ready, data);
      break;
    }
  }
  return G_SOURCE_REMOVE;
}

gboolean
ephy_tab_switcher_model_begin (EphyTabSwitcherModel *self, int direction)
{
  g_autoptr (AdwTabView) view = g_weak_ref_get (&self->view);
  AdwTabPage *selected;
  guint count;
  if (self->active) {
    ephy_tab_switcher_model_move (self, direction);
    return TRUE;
  }
  if (!view || adw_tab_view_get_n_pages (view) < 2)
    return FALSE;
  selected = adw_tab_view_get_selected_page (view);
  if (g_settings_get_boolean (self->settings, EPHY_PREFS_TAB_SWITCHER_MRU)) {
    for (guint i = 0; i < self->recent->len; i++) {
      AdwTabPage *page = g_ptr_array_index (self->recent, i);
      if (contains_page (view, page))
        append_page (self, page);
    }
  } else {
    for (int i = 0; i < adw_tab_view_get_n_pages (view); i++)
      append_page (self, adw_tab_view_get_nth_page (view, i));
  }
  count = g_list_model_get_n_items (G_LIST_MODEL (self->items));
  if (count < 2) {
    g_list_store_remove_all (self->items);
    return FALSE;
  }
  self->selected_index = 0;
  for (guint i = 0; i < count; i++) {
    g_autoptr (EphyTabSwitcherItem) item = g_list_model_get_item (G_LIST_MODEL (self->items), i);
    if (item->page == selected) {
      self->selected_index = i;
      break;
    }
  }
  self->capture_index = 0;
  self->capture_cancel = g_cancellable_new ();
  self->gesture++;
  self->active = TRUE;
  ephy_tab_switcher_model_move (self, direction);
  self->capture_start = self->selected_index;
  g_object_notify_by_pspec (G_OBJECT (self), model_properties[MODEL_PROP_ACTIVE]);
  schedule_capture (self);
  return TRUE;
}
