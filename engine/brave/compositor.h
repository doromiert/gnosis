/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define JUPITER_TYPE_COMPOSITOR (jupiter_compositor_get_type())
G_DECLARE_FINAL_TYPE(JupiterCompositor, jupiter_compositor, JUPITER, COMPOSITOR, GObject)
JupiterCompositor *jupiter_compositor_new(GError **error);
const char *jupiter_compositor_get_socket(JupiterCompositor *self);
#define JUPITER_TYPE_COMPOSITOR_VIEW (jupiter_compositor_view_get_type())
G_DECLARE_FINAL_TYPE(JupiterCompositorView, jupiter_compositor_view, JUPITER, COMPOSITOR_VIEW, GtkWidget)
GtkWidget *jupiter_compositor_view_new(GError **error);
GtkWidget *jupiter_compositor_view_new_for_server(JupiterCompositor *server, const char *token);
gboolean jupiter_compositor_view_has_surface(JupiterCompositorView *self);
const char *jupiter_compositor_view_get_socket(JupiterCompositorView *self);
gboolean jupiter_compositor_view_edit(JupiterCompositorView *self, const char *command);
gboolean jupiter_compositor_view_context_position(JupiterCompositorView *self, double *x, double *y);
void jupiter_compositor_view_release_buttons(JupiterCompositorView *self);
void jupiter_compositor_drag_set_image(GdkDrag *drag, GdkTexture *image);
G_END_DECLS
