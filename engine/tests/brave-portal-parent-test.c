/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <glib.h>
static const char *parent = "wayland:real-gtk-window";
int gnosis_adapter_enabled(const char *id) { (void)id; return 1; }
char *gnosis_portal_parent(void) { return g_strdup(parent); }
#include "native-notifications.c"
int main(void) {
  for (unsigned method = 0; method < 3; ++method) {
    const char *methods[] = {"OpenFile", "SaveFile", "SaveFiles"};
    DBusMessage *source = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser", methods[method]);
    DBusMessageIter args, options, entry, value;
    dbus_message_iter_init_append(source, &args);
    const char *empty = "", *title = "Choose a directory";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &empty);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
    for (unsigned i = 0; i < 2; ++i) {
      const char *key = i ? "directory" : "modal"; dbus_bool_t yes = TRUE;
      dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &value);
      dbus_message_iter_append_basic(&value, DBUS_TYPE_BOOLEAN, &yes);
      dbus_message_iter_close_container(&entry, &value);
      dbus_message_iter_close_container(&options, &entry);
    }
    dbus_message_iter_close_container(&args, &options);
    DBusMessage *copy = portal_message(source); assert(copy);
    const char *actual;
    dbus_message_iter_init(copy, &args); dbus_message_iter_get_basic(&args, &actual);
    assert(!strcmp(actual, parent));
    dbus_message_iter_next(&args); dbus_message_iter_get_basic(&args, &actual); assert(!strcmp(actual, title));
    dbus_message_iter_next(&args); dbus_message_iter_recurse(&args, &options);
    for (unsigned i = 0; i < 2; ++i) {
      dbus_message_iter_recurse(&options, &entry); dbus_message_iter_get_basic(&entry, &actual);
      assert(!strcmp(actual, i ? "directory" : "modal"));
      dbus_message_iter_next(&entry); dbus_message_iter_recurse(&entry, &value);
      dbus_bool_t yes = FALSE; dbus_message_iter_get_basic(&value, &yes); assert(yes);
      dbus_message_iter_next(&options);
    }
    assert(dbus_message_has_signature(copy, "ssa{sv}"));
    dbus_message_unref(copy);
    parent = ""; assert(!portal_message(source)); parent = "wayland:real-gtk-window";
    dbus_message_unref(source);
  }
  g_print("PORTAL_PARENT_PASS open/save/directory modal flags, parent substitution, unchanged options, missing-parent fallback\n");
}
