/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostic-process.hpp"
#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <functional>
#include <dlfcn.h>
#include <fstream>
#include <set>
#include <source_location>
#include <string>

static void wait_for(const std::function<bool()>& predicate,
                     const std::source_location location = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline)
    g_main_context_iteration(nullptr, false);
  if (!predicate()) g_error("GTK selection fixture timed out at %s:%u", location.file_name(), location.line());
}
// This independent X client obtains actual server times without dispatching GTK's queue.
static Time server_time(Display *foreign, Window window, Atom property) {
  XEvent event;
  unsigned char value = 0;
  XChangeProperty(foreign, window, property, XA_INTEGER, 8, PropModeReplace, &value, 1);
  XWindowEvent(foreign, window, PropertyChangeMask, &event);
  g_assert_cmpint(event.type, ==, PropertyNotify);
  return event.xproperty.time;
}
static void claim(Display *display, Window window, Time time) {
  XSetSelectionOwner(display, XA_PRIMARY, window, time);
  XSync(display, False);
  g_assert_cmpuint(XGetSelectionOwner(display, XA_PRIMARY), ==, window);
}
static void queued_clear(Display *display, Window leader) {
  XEvent event;
  // Peek at a real server-generated event. Do not synthesize or consume it.
  XPeekIfEvent(display, &event, +[](Display *, XEvent *event, XPointer pointer) -> Bool {
    return event->type == SelectionClear && event->xselectionclear.selection == XA_PRIMARY &&
           event->xselectionclear.window == *reinterpret_cast<Window *>(pointer);
  }, reinterpret_cast<XPointer>(&leader));
  g_assert_false(event.xany.send_event);
}
static void drain(Display *display) {
  XSync(display, False);
  // The event queue is already synchronized; dispatch every pending GTK/X11 event.
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
}
static void selected(GtkEditable *entry, GdkClipboard *primary, Display *display, Window owner,
                     const char *phase, const std::source_location location = std::source_location::current()) {
  int start = -1, end = -1;
  const bool has_selection = gtk_editable_get_selection_bounds(entry, &start, &end);
  g_print("Selection phase=%s at %s:%u owner=%lu expected=%lu local=%d selected=%d range=%d..%d content=%p\n",
          phase, location.file_name(), location.line(), XGetSelectionOwner(display, XA_PRIMARY), owner,
          gdk_clipboard_is_local(primary), has_selection, start, end, gdk_clipboard_get_content(primary));
  g_assert_cmpuint(XGetSelectionOwner(display, XA_PRIMARY), ==, owner);
  g_assert_true(gdk_clipboard_is_local(primary));
  auto *content = gdk_clipboard_get_content(primary); g_assert_nonnull(content);
  GValue value = G_VALUE_INIT; g_value_init(&value, G_TYPE_STRING);
  GError *error = nullptr;
  g_assert_true(gdk_content_provider_get_value(content, &value, &error)); g_assert_no_error(error);
  g_assert_cmpstr(g_value_get_string(&value), ==, " select"); g_value_unset(&value);
  g_assert_true(gtk_editable_get_selection_bounds(entry, &start, &end));
  g_assert_cmpint(start, ==, 3); g_assert_cmpint(end, ==, 10);
  g_assert_cmpstr(gtk_editable_get_text(entry), ==, "GTK selection survives");
}
int main() {
  disable_diagnostic_core_dumps(); alarm(20);
  gtk_init();
  Dl_info info{}; g_assert_cmpint(dladdr(reinterpret_cast<void *>(gtk_init), &info), !=, 0);
  g_print("Runtime GTK %u.%u.%u gtk_init=%s\n", gtk_get_major_version(), gtk_get_minor_version(),
          gtk_get_micro_version(), info.dli_fname);
  std::ifstream maps("/proc/self/maps"); std::set<std::string> libraries; std::string line;
  while (std::getline(maps, line)) {
    const auto path = line.find('/');
    if (path != std::string::npos && line.find("/libgtk-4.so", path) != std::string::npos)
      libraries.insert(line.substr(path));
  }
  for (const auto& library : libraries) g_print("Mapped GTK: %s\n", library.c_str());
  g_assert_cmpuint(libraries.size(), ==, 1);
  auto *display = gdk_display_get_default(); g_assert_true(GDK_IS_X11_DISPLAY(display));
  auto *xdisplay = gdk_x11_display_get_xdisplay(display);
  auto *primary = gdk_display_get_primary_clipboard(display);
  auto *window = GTK_WINDOW(gtk_window_new());
  auto *entry = GTK_EDITABLE(gtk_entry_new());
  gtk_window_set_child(window, GTK_WIDGET(entry));
  gtk_window_present(window);
  wait_for([&] { return gtk_widget_get_mapped(GTK_WIDGET(entry)); });
  gtk_widget_grab_focus(GTK_WIDGET(entry));
  auto *surface = gtk_native_get_surface(GTK_NATIVE(window)); g_assert_nonnull(surface);
  XSetInputFocus(xdisplay, gdk_x11_surface_get_xid(surface), RevertToParent, CurrentTime);
  XSync(xdisplay, False);
  auto *text = GTK_WIDGET(gtk_editable_get_delegate(entry));
  wait_for([&] { return gtk_window_is_active(window) && gtk_widget_has_focus(text); });
  drain(xdisplay);
  gtk_editable_set_text(entry, "GTK selection survives");
  gtk_editable_select_region(entry, 3, 10);
  wait_for([&] { return gdk_clipboard_is_local(primary); });
  const Window leader = XGetSelectionOwner(xdisplay, XA_PRIMARY); g_assert_cmpuint(leader, !=, None);
  drain(xdisplay); selected(entry, primary, xdisplay, leader, "initial selection after startup focus/drain");

  auto *foreign = XOpenDisplay(nullptr); g_assert_nonnull(foreign);
  const Window outsider = XCreateSimpleWindow(foreign, DefaultRootWindow(foreign), 0, 0, 1, 1, 0, 0, 0);
  const Atom clock = XInternAtom(foreign, "GNOSIS_SELECTION_TEST_TIME", False);
  XSelectInput(foreign, outsider, PropertyChangeMask);
  unsigned changes = 0;
  g_signal_connect(primary, "changed", G_CALLBACK(+[](GdkClipboard *, unsigned *count) { ++*count; }), &changes);

  // GTK's own connection relinquishes and reclaims at the same real server time.
  auto time = server_time(foreign, outsider, clock);
  claim(xdisplay, None, time);
  claim(xdisplay, leader, time);
  queued_clear(xdisplay, leader);
  drain(xdisplay);
  selected(entry, primary, xdisplay, leader, "own clear then leader reclaim");
  g_assert_cmpuint(changes, ==, 0);
  g_print("Queued own SelectionClear followed by same-owner reclaim retains GtkText selection/content: PASS\n");

  // A different client genuinely takes PRIMARY and leaves it owned by its own window.
  time = server_time(foreign, outsider, clock);
  claim(foreign, outsider, time);
  queued_clear(xdisplay, leader);
  drain(xdisplay);
  g_print("Selection phase=foreign owner remains authoritative owner=%lu expected=%lu local=%d\n",
          XGetSelectionOwner(foreign, XA_PRIMARY), outsider, gdk_clipboard_is_local(primary));
  g_assert_cmpuint(XGetSelectionOwner(foreign, XA_PRIMARY), ==, outsider);
  g_assert_false(gdk_clipboard_is_local(primary));
  g_assert_null(gdk_clipboard_get_content(primary));
  g_assert_false(gtk_editable_get_selection_bounds(entry, nullptr, nullptr));
  g_assert_cmpuint(changes, >, 0);
  g_print("Real foreign PRIMARY takeover remains remote and clears GtkText selection: PASS\n");

  // The GtkText selection genuinely reclaims PRIMARY, then a foreign client briefly takes it.
  gtk_editable_select_region(entry, 3, 10);
  wait_for([&] { return gdk_clipboard_is_local(primary); });
  drain(xdisplay); selected(entry, primary, xdisplay, leader, "GtkText reclaim after foreign takeover");
  changes = 0;
  time = server_time(foreign, outsider, clock);
  claim(foreign, outsider, time);
  claim(xdisplay, leader, time);
  queued_clear(xdisplay, leader);
  drain(xdisplay);
  selected(entry, primary, xdisplay, leader, "foreign clear then leader reclaim");
  g_assert_cmpuint(changes, ==, 0);
  g_print("Foreign takeover then GTK reclaim before queued clear retains current GTK ownership/selection: PASS\n");

  g_signal_handlers_disconnect_by_data(primary, &changes);
  gtk_window_destroy(window);
  XDestroyWindow(foreign, outsider); XCloseDisplay(foreign);
  alarm(0);
}
