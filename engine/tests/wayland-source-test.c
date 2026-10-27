/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wayland-source.h"

static int configured;
static void configure(void *data) { (*(int *)data)++; }

int main(void) {
  struct wl_display *display = wl_display_create();
  guint source = gnosis_wayland_source_attach(display);
  /* No client, focus, pointer or incoming fd traffic: only a GTK-side resize. */
  for (int resize = 1; resize <= 3; resize++) {
    wl_event_loop_add_idle(wl_display_get_event_loop(display), configure, &configured);
    g_main_context_iteration(NULL, FALSE);
    g_assert_cmpint(configured, ==, resize);
  }
  g_source_remove(source);
  wl_display_destroy(display);
  g_print("WAYLAND_SOURCE_PASS resize idles dispatch without client input\n");
  return 0;
}
