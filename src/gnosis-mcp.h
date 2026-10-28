/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS
void gnosis_mcp_start (GApplication *application);
void gnosis_mcp_stop (void);
void gnosis_mcp_window_closed (GtkWindow *window);
void gnosis_mcp_inherit_window (GtkWindow *window, GtkWindow *opener);
GtkWidget *gnosis_mcp_new_tab (GtkWindow *window, const char *url);
G_END_DECLS
