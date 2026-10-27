/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wayland-source.h"

typedef struct {
  GSource source;
  GPollFD poll;
  struct wl_display *display;
} WaylandSource;

static gboolean prepare(GSource *source, gint *timeout) {
  WaylandSource *self = (WaylandSource *)source;
  /* xdg configure is an idle: the epoll fd alone never wakes for GTK resizes. */
  wl_event_loop_dispatch_idle(wl_display_get_event_loop(self->display));
  wl_display_flush_clients(self->display);
  *timeout = -1;
  return FALSE;
}

static gboolean check(GSource *source) {
  return ((WaylandSource *)source)->poll.revents != 0;
}

static gboolean dispatch(GSource *source, GSourceFunc callback, gpointer data) {
  (void)callback; (void)data;
  WaylandSource *self = (WaylandSource *)source;
  if (self->poll.revents & (G_IO_ERR | G_IO_HUP)) return G_SOURCE_REMOVE;
  wl_event_loop_dispatch(wl_display_get_event_loop(self->display), 0);
  wl_display_flush_clients(self->display);
  return G_SOURCE_CONTINUE;
}

guint gnosis_wayland_source_attach(struct wl_display *display) {
  static GSourceFuncs funcs = {.prepare = prepare, .check = check, .dispatch = dispatch};
  GSource *source = g_source_new(&funcs, sizeof(WaylandSource));
  WaylandSource *self = (WaylandSource *)source;
  self->display = display;
  self->poll.fd = wl_event_loop_get_fd(wl_display_get_event_loop(display));
  self->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  g_source_add_poll(source, &self->poll);
  g_source_set_name(source, "Gnosis nested Wayland server");
  guint id = g_source_attach(source, NULL);
  g_source_unref(source);
  return id;
}
