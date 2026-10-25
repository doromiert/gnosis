/* SPDX-License-Identifier: GPL-3.0-or-later
 * Stream MIME offers between the private Wayland seat and GTK's desktop
 * clipboard. Content remains lazy and asynchronous in both directions.
 */
#define WLR_USE_UNSTABLE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "clipboard-bridge.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GdkContentProvider parent;
  struct wlr_data_source *source;
  struct wl_display *display;
  struct wl_listener destroyed;
  GdkContentFormats *formats;
} InnerProvider;
typedef struct { GdkContentProviderClass parent; } InnerProviderClass;
GType inner_provider_get_type(void);
G_DEFINE_TYPE(InnerProvider, inner_provider, GDK_TYPE_CONTENT_PROVIDER)

struct JupiterClipboardBridge {
  struct wl_display *display;
  struct wlr_seat *seat;
  GdkClipboard *clipboard;
  GdkContentProvider *exported;
  struct wl_listener selection;
  gulong changed;
  JupiterClipboardAllowed allowed;
  gpointer user;
};
static void inner_source_destroyed(struct wl_listener *listener, void *data) {
  (void)data; InnerProvider *self = wl_container_of(listener, self, destroyed);
  wl_list_remove(&self->destroyed.link); wl_list_init(&self->destroyed.link);
  self->source = NULL;
  g_clear_pointer(&self->formats, gdk_content_formats_unref);
  self->formats = gdk_content_formats_new(NULL, 0);
  gdk_content_provider_content_changed(GDK_CONTENT_PROVIDER(self));
}
static GdkContentFormats *inner_formats(GdkContentProvider *provider) {
  return gdk_content_formats_ref(((InnerProvider*)provider)->formats);
}
static void inner_written(GObject *object, GAsyncResult *result, gpointer data) {
  GTask *task = data; GError *error = NULL;
  if (g_output_stream_splice_finish(G_OUTPUT_STREAM(object), result, &error) < 0) g_task_return_error(task, error);
  else g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}
static void inner_write(GdkContentProvider *provider, const char *mime, GOutputStream *stream,
                        int priority, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  InnerProvider *self = (InnerProvider*)provider;
  GTask *task = g_task_new(provider, cancel, callback, user);
  if (!self->source || !gdk_content_formats_contain_mime_type(self->formats, mime)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Clipboard offer is no longer available");
    g_object_unref(task); return;
  }
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot open clipboard transfer pipe");
    g_object_unref(task); return;
  }
  GInputStream *input = g_unix_input_stream_new(pipe_fds[0], TRUE);
  wlr_data_source_send(self->source, mime, pipe_fds[1]); // source consumes the write FD
  wl_display_flush_clients(self->display);
  g_output_stream_splice_async(stream, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, priority, cancel, inner_written, task);
  g_object_unref(input);
}
static gboolean inner_write_finish(GdkContentProvider *self, GAsyncResult *result, GError **error) {
  (void)self; return g_task_propagate_boolean(G_TASK(result), error);
}
static void inner_dispose(GObject *object) {
  InnerProvider *self = (InnerProvider*)object;
  wl_list_remove(&self->destroyed.link); wl_list_init(&self->destroyed.link);
  self->source = NULL; g_clear_pointer(&self->formats, gdk_content_formats_unref);
  G_OBJECT_CLASS(inner_provider_parent_class)->dispose(object);
}
static void inner_provider_class_init(InnerProviderClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = inner_dispose;
  GdkContentProviderClass *provider = GDK_CONTENT_PROVIDER_CLASS(klass);
  provider->ref_formats = inner_formats; provider->ref_storable_formats = inner_formats;
  provider->write_mime_type_async = inner_write; provider->write_mime_type_finish = inner_write_finish;
}
static void inner_provider_init(InnerProvider *self) { wl_list_init(&self->destroyed.link); }
static GdkContentProvider *inner_provider_new(struct wl_display *display, struct wlr_data_source *source) {
  InnerProvider *self = g_object_new(inner_provider_get_type(), NULL);
  self->source = source; self->display = display;
  GdkContentFormatsBuilder *builder = gdk_content_formats_builder_new();
  char **mime;
  wl_array_for_each(mime, &source->mime_types) if (*mime && strchr(*mime, '/')) gdk_content_formats_builder_add_mime_type(builder, *mime);
  self->formats = gdk_content_formats_builder_free_to_formats(builder);
  self->destroyed.notify = inner_source_destroyed;
  wl_signal_add(&source->events.destroy, &self->destroyed);
  return GDK_CONTENT_PROVIDER(self);
}

typedef struct {
  struct wlr_data_source source;
  GdkClipboard *clipboard;
  GCancellable *cancel;
  JupiterClipboardAllowed allowed;
  gpointer user;
} DesktopSource;
typedef struct {
  GOutputStream *output;
  GCancellable *cancel;
  char *mime;
} DesktopRead;
static void desktop_read_free(DesktopRead *read) {
  g_output_stream_close(read->output, NULL, NULL);
  g_object_unref(read->output); g_object_unref(read->cancel); g_free(read->mime); g_free(read);
}
static void desktop_written(GObject *object, GAsyncResult *result, gpointer data) {
  GError *error = NULL;
  gssize bytes = g_output_stream_splice_finish(G_OUTPUT_STREAM(object), result, &error);
  if (error) { g_warning("Desktop clipboard transfer failed: %s", error->message); g_error_free(error); }
  else g_message("Desktop clipboard transfer completed: %" G_GSSIZE_FORMAT " bytes (%s)", bytes, ((DesktopRead*)data)->mime);
  desktop_read_free(data);
}
static void desktop_read(GObject *object, GAsyncResult *result, gpointer data) {
  DesktopRead *read = data;
  GError *error = NULL;
  GInputStream *input = gdk_clipboard_read_finish(GDK_CLIPBOARD(object), result, NULL, &error);
  if (error) { g_warning("Desktop clipboard read failed: %s", error->message); g_error_free(error); }
  if (!input || g_cancellable_is_cancelled(read->cancel)) {
    g_clear_object(&input); desktop_read_free(read); return;
  }
  g_output_stream_splice_async(read->output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
    G_PRIORITY_DEFAULT, read->cancel, desktop_written, read);
  g_object_unref(input);
}
static void desktop_send(struct wlr_data_source *source, const char *mime, int32_t fd) {
  DesktopSource *self = wl_container_of(source, self, source);
  if (self->allowed && !self->allowed(self->user)) {
    g_message("Desktop clipboard transfer denied: content view is not focused");
    close(fd); return;
  }
  g_message("Desktop clipboard transfer requested: %s", mime);
  DesktopRead *read = g_new0(DesktopRead, 1);
  read->output = g_unix_output_stream_new(fd, TRUE); read->cancel = g_object_ref(self->cancel); read->mime = g_strdup(mime);
  const char *types[] = {read->mime, NULL};
  gdk_clipboard_read_async(self->clipboard, types, G_PRIORITY_DEFAULT, read->cancel, desktop_read, read);
}
static void desktop_destroy(struct wlr_data_source *source) {
  DesktopSource *self = wl_container_of(source, self, source);
  g_cancellable_cancel(self->cancel); g_object_unref(self->cancel); g_object_unref(self->clipboard); g_free(self);
}
static const struct wlr_data_source_impl desktop_impl = {.send = desktop_send, .destroy = desktop_destroy};
static void desktop_changed(GdkClipboard *clipboard, JupiterClipboardBridge *self) {
  if (self->exported && gdk_clipboard_get_content(clipboard) == self->exported) return;
  g_clear_object(&self->exported);
  DesktopSource *source = g_new0(DesktopSource, 1);
  source->clipboard = g_object_ref(clipboard); source->cancel = g_cancellable_new();
  source->allowed = self->allowed; source->user = self->user;
  wlr_data_source_init(&source->source, &desktop_impl);
  GdkContentFormats *formats = gdk_content_formats_union_serialize_mime_types(gdk_content_formats_ref(gdk_clipboard_get_formats(clipboard)));
  gsize count = 0; const char *const *mimes = gdk_content_formats_get_mime_types(formats, &count);
  for (gsize i = 0; i < count; i++) {
    char **item = wl_array_add(&source->source.mime_types, sizeof(char*));
    if (item) *item = strdup(mimes[i]);
  }
  gdk_content_formats_unref(formats);
  wlr_seat_set_selection(self->seat, &source->source, wl_display_next_serial(self->display));
  wl_display_flush_clients(self->display);
}
static void selection_requested(struct wl_listener *listener, void *data) {
  JupiterClipboardBridge *self = wl_container_of(listener, self, selection);
  struct wlr_seat_request_set_selection_event *event = data;
  if (self->allowed && !self->allowed(self->user)) return;
  wlr_seat_set_selection(self->seat, event->source, event->serial);
  GdkContentProvider *provider = event->source ? inner_provider_new(self->display, event->source) : NULL;
  g_set_object(&self->exported, provider);
  // The changed callback sees the provider set before the notification, avoiding
  // a desktop -> private -> desktop selection loop.
  gdk_clipboard_set_content(self->clipboard, provider);
  g_clear_object(&provider);
}
JupiterClipboardBridge *jupiter_clipboard_bridge_new(struct wl_display *display, struct wlr_seat *seat,
                                                    GdkClipboard *clipboard, JupiterClipboardAllowed allowed, gpointer user) {
  JupiterClipboardBridge *self = g_new0(JupiterClipboardBridge, 1);
  self->display = display; self->seat = seat; self->clipboard = g_object_ref(clipboard);
  self->allowed = allowed; self->user = user;
  self->selection.notify = selection_requested; wl_signal_add(&seat->events.request_set_selection, &self->selection);
  self->changed = g_signal_connect(clipboard, "changed", G_CALLBACK(desktop_changed), self);
  desktop_changed(clipboard, self);
  return self;
}
void jupiter_clipboard_bridge_free(JupiterClipboardBridge *self) {
  if (!self) return;
  wl_list_remove(&self->selection.link); g_signal_handler_disconnect(self->clipboard, self->changed);
  if (self->exported && gdk_clipboard_get_content(self->clipboard) == self->exported) gdk_clipboard_set_content(self->clipboard, NULL);
  g_clear_object(&self->exported); g_object_unref(self->clipboard); g_free(self);
}
