/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define EPHY_TYPE_TAB_SWITCHER_ITEM (ephy_tab_switcher_item_get_type ())
G_DECLARE_FINAL_TYPE (EphyTabSwitcherItem, ephy_tab_switcher_item, EPHY, TAB_SWITCHER_ITEM, GObject)

guint64       ephy_tab_switcher_item_get_id        (EphyTabSwitcherItem *self);
const char   *ephy_tab_switcher_item_get_title     (EphyTabSwitcherItem *self);
/* Borrowed, nullable. notify::thumbnail fires when a native snapshot arrives. */
GdkPaintable *ephy_tab_switcher_item_get_thumbnail (EphyTabSwitcherItem *self);

#define EPHY_TYPE_TAB_SWITCHER_MODEL (ephy_tab_switcher_model_get_type ())
G_DECLARE_FINAL_TYPE (EphyTabSwitcherModel, ephy_tab_switcher_model, EPHY, TAB_SWITCHER_MODEL, GObject)

/* Per-window model. Settings uses the main tab-switcher-mru boolean key. */
EphyTabSwitcherModel *ephy_tab_switcher_model_new                (AdwTabView           *view,
                                                               GSettings            *settings);
/* Borrowed GListModel of EphyTabSwitcherItem. Identity/title/order are frozen
 * for one gesture; thumbnails may complete asynchronously. */
GListModel           *ephy_tab_switcher_model_get_items          (EphyTabSwitcherModel *self);
gboolean              ephy_tab_switcher_model_get_active         (EphyTabSwitcherModel *self);
guint                 ephy_tab_switcher_model_get_selected_index (EphyTabSwitcherModel *self);
/* Properties: active, selected-index (read only); signal: closed(). */
gboolean              ephy_tab_switcher_model_begin              (EphyTabSwitcherModel *self,
                                                               int                   direction);
void                  ephy_tab_switcher_model_move               (EphyTabSwitcherModel *self,
                                                               int                   delta);
void                  ephy_tab_switcher_model_commit             (EphyTabSwitcherModel *self);
void                  ephy_tab_switcher_model_cancel             (EphyTabSwitcherModel *self);
void                  ephy_tab_switcher_model_activate           (EphyTabSwitcherModel *self,
                                                               guint                 index);

G_END_DECLS
