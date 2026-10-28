/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gdk/gdk.h>
struct wl_display;
struct wlr_seat;
struct wlr_data_source;
GdkContentProvider *gnosis_wayland_source_provider_new(struct wl_display*, struct wlr_data_source*);
typedef struct GnosisClipboardBridge GnosisClipboardBridge;
typedef gboolean (*GnosisClipboardAllowed)(gpointer data);
GnosisClipboardBridge *gnosis_clipboard_bridge_new(struct wl_display*, struct wlr_seat*, GdkClipboard*, GnosisClipboardAllowed, gpointer);
void gnosis_clipboard_bridge_free(GnosisClipboardBridge*);
