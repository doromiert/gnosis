/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <adwaita.h>

#include "ephy-tab-switcher-model.h"

G_BEGIN_DECLS

#define EPHY_TYPE_TAB_SWITCHER (ephy_tab_switcher_get_type ())

G_DECLARE_FINAL_TYPE (EphyTabSwitcher, ephy_tab_switcher, EPHY, TAB_SWITCHER, AdwBin)

GtkWidget *ephy_tab_switcher_new             (EphyTabSwitcherModel *model);
guint      ephy_tab_switcher_get_columns     (GtkWidget            *switcher);
gboolean   ephy_tab_switcher_contains_widget (GtkWidget            *switcher,
                                              GtkWidget            *widget);

G_END_DECLS
