/* SPDX-License-Identifier: GPL-3.0-or-later
 * Present embedded-browser notifications and portal file pickers with the
 * GTK shell's identity and parent. Keep native payloads and callbacks intact.
 */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <dlfcn.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static dbus_bool_t copy_value(DBusMessageIter *from, DBusMessageIter *to, const char *replacement, unsigned depth, dbus_bool_t notification) {
  if (depth > 32) return FALSE;
  int type = dbus_message_iter_get_arg_type(from);
  if (replacement && type == DBUS_TYPE_STRING)
    return dbus_message_iter_append_basic(to, type, &replacement);
  if (dbus_type_is_basic(type)) {
    DBusBasicValue value;
    dbus_message_iter_get_basic(from, &value);
    return dbus_message_iter_append_basic(to, type, &value);
  }
  if (type != DBUS_TYPE_ARRAY && type != DBUS_TYPE_VARIANT && type != DBUS_TYPE_STRUCT && type != DBUS_TYPE_DICT_ENTRY)
    return FALSE;
  DBusMessageIter input, output;
  dbus_message_iter_recurse(from, &input);
  char *signature = NULL;
  const char *contained = NULL;
  if (type == DBUS_TYPE_ARRAY) {
    signature = dbus_message_iter_get_signature(from); contained = signature ? signature + 1 : NULL;
  } else if (type == DBUS_TYPE_VARIANT) {
    signature = dbus_message_iter_get_signature(&input); contained = signature;
  }
  if (!dbus_message_iter_open_container(to, type, contained, &output)) { dbus_free(signature); return FALSE; }
  dbus_free(signature);
  const char *key = NULL;
  unsigned index = 0;
  while (dbus_message_iter_get_arg_type(&input) != DBUS_TYPE_INVALID) {
    if (type == DBUS_TYPE_DICT_ENTRY && !index && dbus_message_iter_get_arg_type(&input) == DBUS_TYPE_STRING)
      dbus_message_iter_get_basic(&input, &key);
    dbus_bool_t ok;
    if (notification && type == DBUS_TYPE_DICT_ENTRY && index == 1 && key && !strcmp(key, "desktop-entry") &&
        dbus_message_iter_get_arg_type(&input) == DBUS_TYPE_VARIANT) {
      DBusMessageIter value;
      const char *application = "com.neg-zero.Gnosis";
      ok = dbus_message_iter_open_container(&output, DBUS_TYPE_VARIANT, "s", &value) &&
           dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &application) &&
           dbus_message_iter_close_container(&output, &value);
    } else ok = copy_value(&input, &output, NULL, depth + 1, notification);
    if (!ok) { dbus_message_iter_abandon_container(to, &output); return FALSE; }
    ++index;
    if (!dbus_message_iter_next(&input)) break;
  }
  return dbus_message_iter_close_container(to, &output);
}

extern int gnosis_adapter_enabled(const char *id);
static DBusMessage *notification_message(DBusMessage *source) {
  if (!gnosis_adapter_enabled("notifications")) return NULL;
  if (!dbus_message_is_method_call(source, "org.freedesktop.Notifications", "Notify") ||
      !dbus_message_has_path(source, "/org/freedesktop/Notifications")) return NULL;
  DBusMessageIter input, output;
  const char *name = NULL;
  if (!dbus_message_iter_init(source, &input) || dbus_message_iter_get_arg_type(&input) != DBUS_TYPE_STRING) return NULL;
  dbus_message_iter_get_basic(&input, &name);
  if (!name || (strcmp(name,"Brave") && strcmp(name,"Brave Browser"))) return NULL;
  DBusMessage *message = dbus_message_new_method_call(dbus_message_get_destination(source),
    dbus_message_get_path(source), dbus_message_get_interface(source), dbus_message_get_member(source));
  if (!message) return NULL;
  dbus_message_set_auto_start(message, dbus_message_get_auto_start(source));
  dbus_message_set_no_reply(message, dbus_message_get_no_reply(source));
  dbus_message_iter_init_append(message, &output);
  unsigned index = 0;
  do {
    const char *replacement = index == 0 ? "Gnosis" : NULL;
    if (index == 2 && dbus_message_iter_get_arg_type(&input) == DBUS_TYPE_STRING) {
      const char *icon = NULL;
      dbus_message_iter_get_basic(&input, &icon);
      if (icon && (!*icon || !strcmp(icon,"brave-browser") || !strcmp(icon,"brave"))) {
        replacement = getenv("GNOSIS_NOTIFICATION_ICON");
        if (!replacement || !*replacement) replacement = "com.neg-zero.Gnosis";
      }
    }
    if (!copy_value(&input, &output, replacement, 0, TRUE)) { dbus_message_unref(message); return NULL; }
    ++index;
  } while (dbus_message_iter_next(&input));
  return message;
}

extern char *gnosis_portal_parent(void);
static DBusMessage *portal_message(DBusMessage *source) {
  if (!dbus_message_has_destination(source, "org.freedesktop.portal.Desktop") ||
      !dbus_message_has_path(source, "/org/freedesktop/portal/desktop") ||
      !(dbus_message_is_method_call(source, "org.freedesktop.portal.FileChooser", "OpenFile") ||
        dbus_message_is_method_call(source, "org.freedesktop.portal.FileChooser", "SaveFile") ||
        dbus_message_is_method_call(source, "org.freedesktop.portal.FileChooser", "SaveFiles")) ||
      !dbus_message_has_signature(source, "ssa{sv}")) return NULL;
  g_autofree char *parent = gnosis_portal_parent();
  if (!parent || !*parent) return NULL;
  DBusMessage *message = dbus_message_new_method_call(dbus_message_get_destination(source),
    dbus_message_get_path(source), dbus_message_get_interface(source), dbus_message_get_member(source));
  if (!message) return NULL;
  dbus_message_set_auto_start(message, dbus_message_get_auto_start(source));
  dbus_message_set_no_reply(message, dbus_message_get_no_reply(source));
  DBusMessageIter input, output;
  dbus_message_iter_init(source, &input); dbus_message_iter_init_append(message, &output);
  unsigned index = 0;
  do {
    if (!copy_value(&input, &output, index++ == 0 ? parent : NULL, 0, FALSE)) {
      dbus_message_unref(message); return NULL;
    }
  } while (dbus_message_iter_next(&input));
  return message;
}

static DBusMessage *presentation_message(DBusMessage *source) {
  DBusMessage *message = portal_message(source);
  return message ? message : notification_message(source);
}

dbus_bool_t dbus_connection_send_with_reply(DBusConnection *connection, DBusMessage *message,
                                            DBusPendingCall **pending, int timeout) {
  typedef dbus_bool_t (*Send)(DBusConnection *, DBusMessage *, DBusPendingCall **, int);
  Send real = (Send)dlsym(RTLD_NEXT,"dbus_connection_send_with_reply");
  DBusMessage *replacement = presentation_message(message);
  dbus_bool_t result = real(connection, replacement ? replacement : message, pending, timeout);
  if (replacement) { dbus_message_set_serial(message, dbus_message_get_serial(replacement)); dbus_message_unref(replacement); }
  return result;
}

dbus_bool_t dbus_connection_send(DBusConnection *connection, DBusMessage *message, dbus_uint32_t *serial) {
  typedef dbus_bool_t (*Send)(DBusConnection *, DBusMessage *, dbus_uint32_t *);
  Send real = (Send)dlsym(RTLD_NEXT,"dbus_connection_send");
  DBusMessage *replacement = presentation_message(message);
  dbus_bool_t result = real(connection, replacement ? replacement : message, serial);
  if (replacement) { dbus_message_set_serial(message, dbus_message_get_serial(replacement)); dbus_message_unref(replacement); }
  return result;
}
