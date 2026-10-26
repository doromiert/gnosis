/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gdk/gdk.h>
struct wl_display;
struct wlr_seat;
struct wlr_data_source;
GdkContentProvider *jupiter_wayland_source_provider_new(struct wl_display*, struct wlr_data_source*);
typedef struct JupiterClipboardBridge JupiterClipboardBridge;
typedef gboolean (*JupiterClipboardAllowed)(gpointer data);
JupiterClipboardBridge *jupiter_clipboard_bridge_new(struct wl_display*, struct wlr_seat*, GdkClipboard*, JupiterClipboardAllowed, gpointer);
void jupiter_clipboard_bridge_free(JupiterClipboardBridge*);
