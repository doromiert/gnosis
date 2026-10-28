/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <wayland-server-core.h>

guint gnosis_wayland_source_attach(struct wl_display *display);
