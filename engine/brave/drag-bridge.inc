/* SPDX-License-Identifier: GPL-3.0-or-later
 * Desktop GTK drag-and-drop <-> the embedded browser's real Wayland seat.
 * MIME data stays lazy; Chromium retains hit testing, file grants and DOM events.
 */
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <stdlib.h>
#include <string.h>

struct DesktopDrag {
  JupiterCompositor *server;
  JupiterCompositorView *host;
  GdkDrag *outer;
  GdkDrop *self_target;
  struct wlr_drag *inner;
  struct wlr_data_source *source;
  struct wl_listener destroyed, source_destroyed;
  gboolean dropped, self_drop, closing;
};
struct ImportedDrag {
  struct wlr_data_source source;
  JupiterCompositor *server;
  JupiterCompositorView *host;
  GdkDrop *outer;
  GCancellable *cancel;
  gboolean dropped, finished;
};
#include "drag-preview.inc"
static GdkDragAction desktop_action(uint32_t action) {
  return action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? GDK_ACTION_COPY :
    action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE ? GDK_ACTION_MOVE :
    action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK ? GDK_ACTION_ASK : 0;
}
static uint32_t inner_action(GdkDragAction action) {
  return action == GDK_ACTION_COPY || action == GDK_ACTION_LINK ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY :
    action == GDK_ACTION_MOVE ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE :
    action == GDK_ACTION_ASK ? WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK : 0;
}
static void retire_listener(struct wl_listener *listener) {
  wl_list_remove(&listener->link); wl_list_init(&listener->link);
}
static void end_desktop_drag(DesktopDrag *drag, gboolean success) {
  if (drag->closing) return;
  drag->closing = TRUE;
  GCancellable *preview_cancel = g_object_get_data(G_OBJECT(drag->outer), "gnosis-drag-preview-cancel");
  if (preview_cancel) g_cancellable_cancel(preview_cancel);
  drag->server->export_drag = NULL;
  g_signal_handlers_disconnect_by_data(drag->outer, drag);
  retire_listener(&drag->destroyed); retire_listener(&drag->source_destroyed);
  if (success) {
    if (drag->source && !drag->self_drop) {
      if (!drag->dropped) wlr_data_source_dnd_drop(drag->source);
      wlr_data_source_dnd_finish(drag->source);
    }
    if (drag->inner) {
      drag->inner->dropped = TRUE;
      drag->inner->pointer_grab.interface->cancel(&drag->inner->pointer_grab);
    }
  } else if (drag->source) wlr_data_source_destroy(drag->source);
  if (drag->self_target) {
    if (drag->self_drop && drag->dropped) gdk_drop_finish(drag->self_target, success ? GDK_ACTION_COPY : 0);
    g_object_unref(drag->self_target);
  }
  jupiter_compositor_view_release_buttons(drag->host);
  wl_display_flush_clients(drag->server->display);
  gdk_drag_drop_done(drag->outer, success);
  g_object_unref(drag->outer);
  g_free(drag);
}
static void exported_inner_destroyed(struct wl_listener *listener, void *data) {
  (void)data; DesktopDrag *drag = wl_container_of(listener, drag, destroyed);
  retire_listener(listener); drag->inner = NULL;
}
static void exported_source_destroyed(struct wl_listener *listener, void *data) {
  (void)data; DesktopDrag *drag = wl_container_of(listener, drag, source_destroyed);
  retire_listener(listener); drag->source = NULL;
  end_desktop_drag(drag, drag->self_drop && drag->dropped);
}
static void exported_action(GdkDrag *outer, GParamSpec *pspec, DesktopDrag *drag) {
  (void)pspec;
  if (!drag->source || drag->self_target) return;
  uint32_t action = inner_action(gdk_drag_get_selected_action(outer));
  wlr_data_source_dnd_action(drag->source, action);
  char **mime;
  wl_array_for_each(mime, &drag->source->mime_types) {
    if (action && *mime) { wlr_data_source_accept(drag->source, 0, *mime); break; }
  }
  wl_display_flush_clients(drag->server->display);
}
static void exported_drop(GdkDrag *outer, DesktopDrag *drag) {
  (void)outer;
  if (!drag->source || drag->dropped) return;
  drag->dropped = TRUE;
  if (drag->inner) drag->inner->dropped = TRUE;
  if (!drag->self_drop) wlr_data_source_dnd_drop(drag->source);
  wl_display_flush_clients(drag->server->display);
}
static void exported_finished(GdkDrag *outer, DesktopDrag *drag) {
  (void)outer; end_desktop_drag(drag, TRUE);
}
static void exported_cancelled(GdkDrag *outer, GdkDragCancelReason reason, DesktopDrag *drag) {
  (void)outer; (void)reason; end_desktop_drag(drag, FALSE);
}
static void reject_requested_drag(struct wlr_drag *drag) {
  if (drag->source) wlr_data_source_destroy(drag->source);
  else drag->pointer_grab.interface->cancel(&drag->pointer_grab);
}
static void request_drag(struct wl_listener *listener, void *data) {
  JupiterCompositor *server = wl_container_of(listener, server, request_drag);
  struct wlr_seat_request_start_drag_event *event = data;
  JupiterCompositorView *host = NULL;
  struct wlr_surface *root = wlr_surface_get_root_surface(event->origin);
  for (guint i = 0; i < server->hosts->len; i++) {
    JupiterCompositorView *candidate = g_ptr_array_index(server->hosts, i);
    if (candidate->active && candidate->active->toplevel->base->surface == root && gtk_widget_get_mapped(GTK_WIDGET(candidate))) host = candidate;
  }
  if (!host || !host->pointer_device || !event->drag->source || server->export_drag || server->import_drag ||
      !wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
    reject_requested_drag(event->drag); return;
  }
  GtkNative *native = gtk_widget_get_native(GTK_WIDGET(host));
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  if (!surface) { reject_requested_drag(event->drag); return; }
  DesktopDrag *drag = g_new0(DesktopDrag, 1);
  drag->server = server; drag->host = host; drag->inner = event->drag; drag->source = event->drag->source;
  wl_list_init(&drag->destroyed.link); wl_list_init(&drag->source_destroyed.link);
  server->export_drag = drag; // GTK cancels its press gesture when acquiring the desktop drag.
  wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
  listen(&event->drag->events.destroy, &drag->destroyed, exported_inner_destroyed);
  listen(&drag->source->events.destroy, &drag->source_destroyed, exported_source_destroyed);
  GdkContentProvider *provider = jupiter_wayland_source_provider_new(server->display, drag->source);
  uint32_t actions = drag->source->actions;
  GdkDragAction allowed = (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? GDK_ACTION_COPY : 0) |
    (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE ? GDK_ACTION_MOVE : 0);
  drag->outer = gdk_drag_begin(surface, host->pointer_device, provider, allowed ? allowed : GDK_ACTION_COPY, 0, 0);
  if (!drag->outer) {
    retire_listener(&drag->destroyed); retire_listener(&drag->source_destroyed);
    server->export_drag = NULL; wlr_data_source_destroy(drag->source); g_object_unref(provider); g_free(drag); return;
  }
  g_signal_connect(drag->outer, "notify::selected-action", G_CALLBACK(exported_action), drag);
  g_signal_connect(drag->outer, "drop-performed", G_CALLBACK(exported_drop), drag);
  g_signal_connect(drag->outer, "dnd-finished", G_CALLBACK(exported_finished), drag);
  g_signal_connect(drag->outer, "cancel", G_CALLBACK(exported_cancelled), drag);
  setup_drag_preview(drag, provider);
  g_object_unref(provider);
  g_message("Native webpage drag connected to desktop");
}

typedef struct { GOutputStream *output; GCancellable *cancel; char *mime; } DropRead;
static void drop_read_free(DropRead *read) {
  g_output_stream_close(read->output, NULL, NULL);
  g_object_unref(read->output); g_object_unref(read->cancel); g_free(read->mime); g_free(read);
}
static void drop_written(GObject *output, GAsyncResult *result, gpointer data) {
  g_autoptr(GError) error = NULL;
  g_output_stream_splice_finish(G_OUTPUT_STREAM(output), result, &error);
  if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) g_warning("Drag transfer failed: %s", error->message);
  drop_read_free(data);
}
static void drop_read(GObject *outer, GAsyncResult *result, gpointer data) {
  DropRead *read = data;
  g_autoptr(GError) error = NULL;
  GInputStream *input = gdk_drop_read_finish(GDK_DROP(outer), result, NULL, &error);
  if (!input || g_cancellable_is_cancelled(read->cancel)) {
    g_clear_object(&input); drop_read_free(read); return;
  }
  g_output_stream_splice_async(read->output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
    G_PRIORITY_DEFAULT, read->cancel, drop_written, read);
  g_object_unref(input);
}
static void imported_send(struct wlr_data_source *source, const char *mime, int32_t fd) {
  ImportedDrag *drag = wl_container_of(source, drag, source);
  DropRead *read = g_new0(DropRead, 1);
  read->output = g_unix_output_stream_new(fd, TRUE); read->cancel = g_object_ref(drag->cancel); read->mime = g_strdup(mime);
  const char *types[] = {read->mime, NULL};
  gdk_drop_read_async(drag->outer, types, G_PRIORITY_DEFAULT, drag->cancel, drop_read, read);
}
static void imported_accept(struct wlr_data_source *source, uint32_t serial, const char *mime) {
  (void)serial; ImportedDrag *drag = wl_container_of(source, drag, source);
  gdk_drop_status(drag->outer, mime ? GDK_ACTION_COPY : 0, mime ? GDK_ACTION_COPY : 0);
}
static void imported_action(struct wlr_data_source *source, enum wl_data_device_manager_dnd_action action) {
  ImportedDrag *drag = wl_container_of(source, drag, source);
  gdk_drop_status(drag->outer, desktop_action(action), desktop_action(action));
}
static void imported_drop(struct wlr_data_source *source) {
  ImportedDrag *drag = wl_container_of(source, drag, source); drag->dropped = TRUE;
}
static void imported_finish(struct wlr_data_source *source) {
  ImportedDrag *drag = wl_container_of(source, drag, source);
  drag->finished = TRUE; gdk_drop_finish(drag->outer, desktop_action(source->current_dnd_action));
}
static void imported_destroy(struct wlr_data_source *source) {
  ImportedDrag *drag = wl_container_of(source, drag, source);
  if (drag->server->import_drag == drag) drag->server->import_drag = NULL;
  if (drag->dropped && !drag->finished) gdk_drop_finish(drag->outer, 0);
  g_cancellable_cancel(drag->cancel); g_object_unref(drag->cancel); g_object_unref(drag->outer); g_free(drag);
}
static const struct wlr_data_source_impl imported_impl = {
  .send = imported_send, .accept = imported_accept, .destroy = imported_destroy,
  .dnd_drop = imported_drop, .dnd_finish = imported_finish, .dnd_action = imported_action
};
static void drag_pointer(JupiterCompositorView *host, double x, double y) {
  struct wlr_xdg_surface *xdg = host->active->toplevel->base;
  int width = gtk_widget_get_width(GTK_WIDGET(host)), height = gtk_widget_get_height(GTK_WIDGET(host));
  if (width && xdg->geometry.width > 0) x *= (double)xdg->geometry.width / width;
  if (height && xdg->geometry.height > 0) y *= (double)xdg->geometry.height / height;
  double sx, sy;
  struct wlr_surface *surface = wlr_xdg_surface_surface_at(xdg, x + xdg->geometry.x, y + xdg->geometry.y, &sx, &sy);
  if (!surface) return;
  wlr_seat_pointer_notify_enter(host->server->seat, surface, sx, sy);
  wlr_seat_pointer_notify_motion(host->server->seat, (uint32_t)(g_get_monotonic_time() / 1000), sx, sy);
  wlr_seat_pointer_notify_frame(host->server->seat);
  wl_display_flush_clients(host->server->display);
}
static GdkDragAction drop_motion(GtkDropTargetAsync *target, GdkDrop *outer, double x, double y, JupiterCompositorView *host) {
  (void)target; JupiterCompositor *server = host->server;
  if (!server || !host->active || !gtk_widget_get_mapped(GTK_WIDGET(host))) return 0;
  if (server->export_drag && gdk_drop_get_drag(outer) == server->export_drag->outer) {
    g_set_object(&server->export_drag->self_target, outer);
    drag_pointer(host, x, y);
    return server->export_drag->source ? desktop_action(server->export_drag->source->current_dnd_action) : 0;
  }
  if (server->export_drag) return 0;
  if (!server->import_drag) {
    if (!(gdk_drop_get_actions(outer) & GDK_ACTION_COPY)) return 0;
    struct wlr_seat_client *client = wlr_seat_client_for_wl_client(server->seat,
      wl_resource_get_client(host->active->toplevel->base->surface->resource));
    if (!client) return 0;
    ImportedDrag *drag = g_new0(ImportedDrag, 1);
    drag->server = server; drag->host = host; drag->outer = g_object_ref(outer); drag->cancel = g_cancellable_new();
    wlr_data_source_init(&drag->source, &imported_impl);
    drag->source.actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    GdkContentFormats *formats = gdk_content_formats_union_serialize_mime_types(gdk_content_formats_ref(gdk_drop_get_formats(outer)));
    gsize count; const char *const *mimes = gdk_content_formats_get_mime_types(formats, &count);
    for (gsize i = 0; i < count; i++) {
      char **mime = wl_array_add(&drag->source.mime_types, sizeof(char*));
      if (mime) *mime = strdup(mimes[i]);
    }
    gdk_content_formats_unref(formats);
    struct wlr_drag *inner = wlr_drag_create(client, &drag->source, NULL);
    if (!inner) { wlr_data_source_destroy(&drag->source); return 0; }
    server->import_drag = drag;
    // The desktop owns the real button press. No synthetic click is sent to the
    // webpage; this button identifies the eventual drop to wlroots' drag grab.
    server->seat->pointer_state.grab_button = BTN_LEFT;
    wlr_seat_start_pointer_drag(server->seat, inner, 0);
  }
  if (server->import_drag->outer != outer) return 0;
  server->import_drag->host = host;
  drag_pointer(host, x, y);
  return desktop_action(server->import_drag->source.current_dnd_action);
}
static gboolean drop_accept(GtkDropTargetAsync *target, GdkDrop *outer, JupiterCompositorView *host) {
  (void)target;
  return host->active && gtk_widget_get_mapped(GTK_WIDGET(host)) &&
    gdk_drop_get_actions(outer) & (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
}
static gboolean perform_drop(GtkDropTargetAsync *target, GdkDrop *outer, double x, double y, JupiterCompositorView *host) {
  if (!drop_motion(target, outer, x, y, host)) return FALSE;
  JupiterCompositor *server = host->server;
  if (server->export_drag && server->export_drag->self_target == outer) {
    server->export_drag->self_drop = TRUE; server->export_drag->dropped = TRUE;
  } else if (!server->import_drag || !server->import_drag->source.accepted) return FALSE;
  uint32_t time = (uint32_t)(g_get_monotonic_time() / 1000);
  if (server->import_drag && server->seat->drag) {
    // No nested wl_pointer press preceded an external desktop drag. Deliver
    // its release to the drag grab directly, rather than inventing a click.
    struct wlr_seat_pointer_grab *grab = &server->seat->drag->pointer_grab;
    grab->interface->button(grab, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
  } else wlr_seat_pointer_notify_button(server->seat, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
  wlr_seat_pointer_notify_frame(server->seat);
  host->pressed_buttons = 0;
  wl_display_flush_clients(server->display);
  return TRUE;
}
static void drop_leave(GtkDropTargetAsync *target, GdkDrop *outer, JupiterCompositorView *host) {
  (void)target; JupiterCompositor *server = host->server;
  if (!server) return;
  if (server->import_drag && server->import_drag->outer == outer && !server->import_drag->dropped)
    wlr_data_source_destroy(&server->import_drag->source);
  if (server->export_drag && server->export_drag->self_target == outer && !server->export_drag->dropped) {
    g_clear_object(&server->export_drag->self_target);
    wlr_seat_pointer_notify_clear_focus(server->seat);
  }
}
static void cancel_view_drag(JupiterCompositorView *host) {
  if (!host->server) return;
  if (host->server->export_drag && host->server->export_drag->host == host) end_desktop_drag(host->server->export_drag, FALSE);
  if (host->server->import_drag && host->server->import_drag->host == host) wlr_data_source_destroy(&host->server->import_drag->source);
}
static void setup_drop_target(JupiterCompositorView *host) {
  GtkDropTargetAsync *target = gtk_drop_target_async_new(NULL, GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
  g_signal_connect(target, "accept", G_CALLBACK(drop_accept), host);
  g_signal_connect(target, "drag-enter", G_CALLBACK(drop_motion), host);
  g_signal_connect(target, "drag-motion", G_CALLBACK(drop_motion), host);
  g_signal_connect(target, "drag-leave", G_CALLBACK(drop_leave), host);
  g_signal_connect(target, "drop", G_CALLBACK(perform_drop), host);
  gtk_widget_add_controller(GTK_WIDGET(host), GTK_EVENT_CONTROLLER(target));
}
