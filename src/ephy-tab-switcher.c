/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"

#include "ephy-tab-switcher.h"

struct _EphyTabSwitcher {
  AdwBin parent_instance;

  GtkWidget *surface;
  GtkFlowBox *grid;
  EphyTabSwitcherModel *model;
};

G_DEFINE_FINAL_TYPE (EphyTabSwitcher, ephy_tab_switcher, ADW_TYPE_BIN)

static void
thumbnail_changed_cb (EphyTabSwitcherItem *item,
                      GParamSpec          *pspec,
                      GtkPicture          *picture)
{
  gtk_picture_set_paintable (picture,
                             ephy_tab_switcher_item_get_thumbnail (item));
}

static GtkWidget *
create_item_cb (gpointer item,
                gpointer user_data)
{
  EphyTabSwitcherItem *switcher_item = EPHY_TAB_SWITCHER_ITEM (item);
  const char *title = ephy_tab_switcher_item_get_title (switcher_item);
  GtkWidget *tile;
  GtkWidget *picture;
  GtkWidget *label;

  tile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (tile, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (tile, title);

  picture = gtk_picture_new_for_paintable (ephy_tab_switcher_item_get_thumbnail (switcher_item));
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_widget_set_size_request (picture, 160, 100);
  gtk_widget_set_overflow (picture, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (picture, "card");
  gtk_widget_add_css_class (picture, "thumbnail");
  gtk_box_append (GTK_BOX (tile), picture);

  label = gtk_label_new (title);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 20);
  gtk_widget_set_size_request (label, 160, -1);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_box_append (GTK_BOX (tile), label);

  g_signal_connect_object (switcher_item,
                           "notify::thumbnail",
                           G_CALLBACK (thumbnail_changed_cb),
                           picture,
                           0);

  return tile;
}

static void
update_selected_index (EphyTabSwitcher *self)
{
  GtkFlowBoxChild *child;
  guint index;

  if (!self->model || !ephy_tab_switcher_model_get_active (self->model))
    return;

  index = ephy_tab_switcher_model_get_selected_index (self->model);
  child = gtk_flow_box_get_child_at_index (self->grid, index);
  if (!child)
    return;

  gtk_flow_box_select_child (self->grid, child);
  gtk_widget_grab_focus (GTK_WIDGET (child));
}

static void
selected_index_changed_cb (EphyTabSwitcherModel *model,
                           GParamSpec           *pspec,
                           EphyTabSwitcher      *self)
{
  update_selected_index (self);
}

static void
active_changed_cb (EphyTabSwitcherModel *model,
                   GParamSpec           *pspec,
                   EphyTabSwitcher      *self)
{
  gboolean active = ephy_tab_switcher_model_get_active (model);

  gtk_widget_set_visible (GTK_WIDGET (self), active);

  if (active)
    update_selected_index (self);
}

static void
child_activated_cb (GtkFlowBox      *grid,
                    GtkFlowBoxChild *child,
                    EphyTabSwitcher *self)
{
  int index;

  if (!self->model)
    return;

  index = gtk_flow_box_child_get_index (child);
  if (index >= 0)
    ephy_tab_switcher_model_activate (self->model, index);
}

static void
ephy_tab_switcher_dispose (GObject *object)
{
  EphyTabSwitcher *self = EPHY_TAB_SWITCHER (object);

  if (self->grid)
    gtk_flow_box_bind_model (self->grid, NULL, NULL, NULL, NULL);
  g_clear_object (&self->model);

  gtk_widget_dispose_template (GTK_WIDGET (self), EPHY_TYPE_TAB_SWITCHER);

  G_OBJECT_CLASS (ephy_tab_switcher_parent_class)->dispose (object);
}

static void
ephy_tab_switcher_class_init (EphyTabSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ephy_tab_switcher_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/epiphany/gtk/tab-switcher.ui");
  gtk_widget_class_bind_template_child (widget_class, EphyTabSwitcher, surface);
  gtk_widget_class_bind_template_child (widget_class, EphyTabSwitcher, grid);
}

static void
ephy_tab_switcher_init (EphyTabSwitcher *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->grid,
                    "child-activated",
                    G_CALLBACK (child_activated_cb),
                    self);
}

GtkWidget *
ephy_tab_switcher_new (EphyTabSwitcherModel *model)
{
  EphyTabSwitcher *self;

  g_return_val_if_fail (EPHY_IS_TAB_SWITCHER_MODEL (model), NULL);

  self = g_object_new (EPHY_TYPE_TAB_SWITCHER, NULL);
  self->model = g_object_ref (model);

  gtk_flow_box_bind_model (self->grid,
                           ephy_tab_switcher_model_get_items (model),
                           create_item_cb,
                           NULL,
                           NULL);

  g_signal_connect_object (model,
                           "notify::active",
                           G_CALLBACK (active_changed_cb),
                           self,
                           0);
  g_signal_connect_object (model,
                           "notify::selected-index",
                           G_CALLBACK (selected_index_changed_cb),
                           self,
                           0);

  active_changed_cb (model, NULL, self);

  return GTK_WIDGET (self);
}

guint
ephy_tab_switcher_get_columns (GtkWidget *switcher)
{
  EphyTabSwitcher *self;
  int width;

  g_return_val_if_fail (EPHY_IS_TAB_SWITCHER (switcher), 1);

  self = EPHY_TAB_SWITCHER (switcher);
  width = gtk_widget_get_width (GTK_WIDGET (self->grid));

  if (width <= 0)
    return 3;

  return CLAMP ((width + 8) / (160 + 8), 1, 3);
}

gboolean
ephy_tab_switcher_contains_widget (GtkWidget *switcher,
                                   GtkWidget *widget)
{
  EphyTabSwitcher *self;

  g_return_val_if_fail (EPHY_IS_TAB_SWITCHER (switcher), FALSE);

  if (!widget)
    return FALSE;

  self = EPHY_TAB_SWITCHER (switcher);

  return widget == self->surface || gtk_widget_is_ancestor (widget, self->surface);
}
