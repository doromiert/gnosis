/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS
typedef struct JupiterOuterPointer JupiterOuterPointer;
typedef void (*JupiterPointerState)(gpointer data, gboolean locked);
typedef void (*JupiterPointerMotion)(gpointer data, guint64 time_usec, double dx, double dy, double raw_dx, double raw_dy);
JupiterOuterPointer *jupiter_outer_pointer_new(GtkWidget *widget, JupiterPointerState state, JupiterPointerMotion motion, gpointer data, GError **error);
gboolean jupiter_outer_pointer_lock(JupiterOuterPointer *self, GError **error);
void jupiter_outer_pointer_release(JupiterOuterPointer *self);
void jupiter_outer_pointer_free(JupiterOuterPointer *self);
G_END_DECLS
