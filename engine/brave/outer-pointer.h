/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS
typedef struct GnosisOuterPointer GnosisOuterPointer;
typedef void (*GnosisPointerState)(gpointer data, gboolean locked);
typedef void (*GnosisPointerMotion)(gpointer data, guint64 time_usec, double dx, double dy, double raw_dx, double raw_dy);
GnosisOuterPointer *gnosis_outer_pointer_new(GtkWidget *widget, GnosisPointerState state, GnosisPointerMotion motion, gpointer data, GError **error);
gboolean gnosis_outer_pointer_lock(GnosisOuterPointer *self, GError **error);
void gnosis_outer_pointer_release(GnosisOuterPointer *self);
void gnosis_outer_pointer_free(GnosisOuterPointer *self);
G_END_DECLS
