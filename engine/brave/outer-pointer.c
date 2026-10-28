/* SPDX-License-Identifier: GPL-3.0-or-later
 * A private event queue on GTK's outer Wayland connection. The compositor
 * widget owns the host pointer constraint; GTK retains ordinary window/input
 * ownership. No global pointer warps or synthetic button holds.
 */
#include "outer-pointer.h"
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <errno.h>
#include <string.h>
#include "pointer-constraints-client.h"
#include "relative-pointer-client.h"

struct GnosisOuterPointer {
  GtkWidget *widget;
  GdkCursor *saved_cursor;
  struct wl_display *display;
  struct wl_event_queue *queue;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct zwp_pointer_constraints_v1 *constraints;
  struct zwp_relative_pointer_manager_v1 *relative_manager;
  struct zwp_relative_pointer_v1 *relative;
  struct zwp_locked_pointer_v1 *lock;
  GSource *source;
  guint32 enter_serial;
  gboolean locked, cursor_hidden, releasing;
  GnosisPointerState state;
  GnosisPointerMotion motion;
  gpointer data;
};
typedef struct { GSource source; GPollFD poll; GnosisOuterPointer *owner; } PointerSource;
static gboolean source_prepare(GSource *source, gint *timeout) {
  PointerSource *s = (PointerSource *)source;
  *timeout = -1;
  // GTK reads the shared socket. Detect already-queued events without reading
  // or dispatching GTK's queue ourselves; otherwise watch its FD for wakeups.
  if (wl_display_prepare_read_queue(s->owner->display, s->owner->queue) == 0) {
    wl_display_cancel_read(s->owner->display); return FALSE;
  }
  return errno == EAGAIN;
}
static gboolean source_check(GSource *source) { return ((PointerSource *)source)->poll.revents != 0; }
static gboolean source_dispatch(GSource *source, GSourceFunc callback, gpointer user) {
  (void)callback; (void)user;
  PointerSource *s = (PointerSource *)source;
  if (wl_display_dispatch_queue_pending(s->owner->display, s->owner->queue) < 0) return G_SOURCE_REMOVE;
  return G_SOURCE_CONTINUE;
}
static GSourceFuncs source_funcs = {.prepare = source_prepare, .check = source_check, .dispatch = source_dispatch};
static void enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
  (void)pointer; (void)surface; (void)x; (void)y;
  ((GnosisOuterPointer *)data)->enter_serial = serial;
}
static void leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {
  (void)pointer; (void)serial; (void)surface;
  GnosisOuterPointer *self = data; self->enter_serial = 0;
}
static void absolute_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {(void)d;(void)p;(void)t;(void)x;(void)y;}
static void button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t v) {(void)d;(void)p;(void)s;(void)t;(void)b;(void)v;}
static void axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {(void)d;(void)p;(void)t;(void)a;(void)v;}
static void frame(void *d, struct wl_pointer *p) {(void)d;(void)p;}
static void axis_source(void *d, struct wl_pointer *p, uint32_t s) {(void)d;(void)p;(void)s;}
static void axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {(void)d;(void)p;(void)t;(void)a;}
static void axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {(void)d;(void)p;(void)a;(void)v;}
static const struct wl_pointer_listener pointer_listener = {
  .enter = enter, .leave = leave, .motion = absolute_motion, .button = button,
  .axis = axis, .frame = frame, .axis_source = axis_source,
  .axis_stop = axis_stop, .axis_discrete = axis_discrete,
};
static void capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  GnosisOuterPointer *self = data;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer) {
    self->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(self->pointer, &pointer_listener, self);
  }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {(void)d;(void)s;(void)n;}
static const struct wl_seat_listener seat_listener = {.capabilities = capabilities, .name = seat_name};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  GnosisOuterPointer *self = data;
  if (!strcmp(interface, "wl_seat") && !self->seat) {
    self->seat = wl_registry_bind(registry, name, &wl_seat_interface, MIN(version, 7));
    wl_seat_add_listener(self->seat, &seat_listener, self);
  } else if (!strcmp(interface, "zwp_pointer_constraints_v1"))
    self->constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
  else if (!strcmp(interface, "zwp_relative_pointer_manager_v1"))
    self->relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {(void)d;(void)r;(void)n;}
static const struct wl_registry_listener registry_listener = {.global = global, .global_remove = global_remove};
static void relative_motion(void *data, struct zwp_relative_pointer_v1 *pointer, uint32_t hi, uint32_t lo,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t raw_dx, wl_fixed_t raw_dy) {
  (void)pointer; GnosisOuterPointer *self = data;
  if (self->locked && self->motion)
    self->motion(self->data, ((guint64)hi << 32) | lo, wl_fixed_to_double(dx), wl_fixed_to_double(dy),
                 wl_fixed_to_double(raw_dx), wl_fixed_to_double(raw_dy));
}
static const struct zwp_relative_pointer_v1_listener relative_listener = {.relative_motion = relative_motion};
static void restore_cursor(GnosisOuterPointer *self) {
  if (!self->cursor_hidden) return;
  self->cursor_hidden = FALSE;
  gtk_widget_set_cursor(self->widget, self->saved_cursor);
  g_clear_object(&self->saved_cursor);
}
static void locked(void *data, struct zwp_locked_pointer_v1 *lock) {
  (void)lock; GnosisOuterPointer *self = data;
  self->locked = TRUE;
  self->saved_cursor = gtk_widget_get_cursor(self->widget);
  if (self->saved_cursor) g_object_ref(self->saved_cursor);
  self->cursor_hidden = TRUE; gtk_widget_set_cursor_from_name(self->widget, "none");
  if (self->enter_serial) wl_pointer_set_cursor(self->pointer, self->enter_serial, NULL, 0, 0);
  if (self->state) self->state(self->data, TRUE);
}
static void unlocked(void *data, struct zwp_locked_pointer_v1 *lock) {
  (void)lock; GnosisOuterPointer *self = data;
  self->locked = FALSE; restore_cursor(self);
  if (!self->releasing && self->state) self->state(self->data, FALSE);
}
static const struct zwp_locked_pointer_v1_listener lock_listener = {.locked = locked, .unlocked = unlocked};
GnosisOuterPointer *gnosis_outer_pointer_new(GtkWidget *widget, GnosisPointerState state, GnosisPointerMotion motion, gpointer data, GError **error) {
  GdkDisplay *display = gtk_widget_get_display(widget);
  if (!GDK_IS_WAYLAND_DISPLAY(display)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Relative capture requires the GTK Wayland backend"); return NULL;
  }
  GnosisOuterPointer *self = g_new0(GnosisOuterPointer, 1);
  self->widget = widget; self->state = state; self->motion = motion; self->data = data;
  self->display = gdk_wayland_display_get_wl_display(display);
  self->queue = wl_display_create_queue(self->display);
  self->registry = wl_display_get_registry(self->display);
  wl_proxy_set_queue((struct wl_proxy *)self->registry, self->queue);
  wl_registry_add_listener(self->registry, &registry_listener, self);
  if (wl_display_roundtrip_queue(self->display, self->queue) < 0 ||
      wl_display_roundtrip_queue(self->display, self->queue) < 0 ||
      !self->pointer || !self->constraints || !self->relative_manager) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "The desktop does not provide Wayland relative-pointer capture");
    gnosis_outer_pointer_free(self); return NULL;
  }
  self->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(self->relative_manager, self->pointer);
  zwp_relative_pointer_v1_add_listener(self->relative, &relative_listener, self);
  PointerSource *source = (PointerSource *)g_source_new(&source_funcs, sizeof(PointerSource));
  source->owner = self; source->poll.fd = wl_display_get_fd(self->display); source->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  g_source_add_poll((GSource *)source, &source->poll); self->source = (GSource *)source;
  g_source_set_name(self->source, "Gnosis outer relative pointer"); g_source_attach(self->source, NULL);
  return self;
}
gboolean gnosis_outer_pointer_lock(GnosisOuterPointer *self, GError **error) {
  if (self->lock) return TRUE;
  GtkNative *native = gtk_widget_get_native(self->widget);
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  if (!surface || !GDK_IS_WAYLAND_SURFACE(surface)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The GTK view has no Wayland surface"); return FALSE;
  }
  graphene_point_t zero = {0, 0}, point;
  if (!gtk_widget_compute_point(self->widget, GTK_WIDGET(native), &zero, &point)) return FALSE;
  double tx, ty; gtk_native_get_surface_transform(native, &tx, &ty);
  struct wl_compositor *compositor = gdk_wayland_display_get_wl_compositor(gtk_widget_get_display(self->widget));
  struct wl_region *region = wl_compositor_create_region(compositor);
  wl_region_add(region, point.x + tx, point.y + ty, gtk_widget_get_width(self->widget), gtk_widget_get_height(self->widget));
  self->lock = zwp_pointer_constraints_v1_lock_pointer(self->constraints, gdk_wayland_surface_get_wl_surface(surface),
    self->pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
  wl_region_destroy(region); zwp_locked_pointer_v1_add_listener(self->lock, &lock_listener, self);
  wl_display_flush(self->display); return TRUE;
}
void gnosis_outer_pointer_release(GnosisOuterPointer *self) {
  if (!self) return;
  self->releasing = TRUE;
  if (self->lock) { zwp_locked_pointer_v1_destroy(self->lock); self->lock = NULL; }
  self->locked = FALSE; restore_cursor(self); self->releasing = FALSE;
  wl_display_flush(self->display);
}
void gnosis_outer_pointer_free(GnosisOuterPointer *self) {
  if (!self) return;
  gnosis_outer_pointer_release(self);
  if (self->source) { g_source_destroy(self->source); g_source_unref(self->source); }
  if (self->relative) zwp_relative_pointer_v1_destroy(self->relative);
  if (self->relative_manager) zwp_relative_pointer_manager_v1_destroy(self->relative_manager);
  if (self->constraints) zwp_pointer_constraints_v1_destroy(self->constraints);
  if (self->pointer) wl_pointer_release(self->pointer);
  if (self->seat) wl_seat_release(self->seat);
  if (self->registry) wl_registry_destroy(self->registry);
  if (self->queue) wl_event_queue_destroy(self->queue);
  g_free(self);
}
