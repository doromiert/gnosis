/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2012 Igalia S.L.
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "ephy-engine-scheme.h"
#include "ephy-engine-view.h"

#include "ephy-about-handler.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ephy-embed-prefs.h"
#include "ephy-embed-shell.h"
#include "ephy-embed-utils.h"
#include "ephy-file-helpers.h"
#include "ephy-history-service.h"
#include "ephy-output-encoding.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "ephy-smaps.h"
#include "ephy-web-app-utils.h"

struct _EphyAboutHandler {
  GObject parent_instance;

  EphySMaps *smaps;
};

G_DEFINE_FINAL_TYPE (EphyAboutHandler, ephy_about_handler, G_TYPE_OBJECT)


#define EPHY_ABOUT_OVERVIEW_MAX_ITEMS 9

#define EPHY_PAGE_TEMPLATE_ABOUT_CSS        "ephy-resource:///org/gnome/epiphany/page-templates/about.css"
#define EPHY_APPLICATION_ICON_PREFIX       "/org/gnome/epiphany/application-icons/"
#define EPHY_APPLICATION_ICONS_KEY         "ephy-application-icons"
#define EPHY_APPLICATION_ICON_FALLBACK     "ephy-resource:///org/gnome/epiphany/page-icons/web-app-icon-missing.svg"

static GBytes *
load_application_icon (const char *path)
{
  const gsize max_size = 4 * 1024 * 1024;
  g_autoptr (GInputStream) file = NULL;
  g_autoptr (GInputStream) memory = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autofree char *png = NULL;
  struct stat info;
  gsize size;
  int fd;

  if (!path || !g_path_is_absolute (path))
    return NULL;

  fd = g_open (path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
  if (fd == -1)
    return NULL;
  file = g_unix_input_stream_new (fd, TRUE);
  if (fstat (fd, &info) != 0 || !S_ISREG (info.st_mode) ||
      (info.st_uid != getuid () && info.st_uid != 0) ||
      info.st_size <= 0 || info.st_size > max_size)
    return NULL;

  bytes = g_input_stream_read_bytes (file, max_size + 1, NULL, NULL);
  if (!bytes || g_bytes_get_size (bytes) > max_size)
    return NULL;
  memory = g_memory_input_stream_new_from_bytes (bytes);
  pixbuf = gdk_pixbuf_new_from_stream_at_scale (memory, 128, 128, TRUE, NULL, NULL);
  if (!pixbuf || !gdk_pixbuf_save_to_buffer (pixbuf, &png, &size, "png", NULL, NULL))
    return NULL;

  /* The desktop entry may be edited: expose decoded pixels, never file bytes. */
  return g_bytes_new_take (g_steal_pointer (&png), size);
}

static char *
application_icon_uri (GHashTable *icons,
                      GBytes     *bytes)
{
  g_autofree char *token = NULL;

  if (!bytes)
    return g_strdup (EPHY_APPLICATION_ICON_FALLBACK);

  token = g_uuid_string_random ();
  g_hash_table_insert (icons, g_strdup (token), g_bytes_ref (bytes));
  return g_strconcat ("ephy-resource://", EPHY_APPLICATION_ICON_PREFIX, token, NULL);
}

static char *
overview_empty_icon_uri (void)
{
  GtkIconTheme *icon_theme;
  g_autoptr (GtkIconPaintable) paintable = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autofree char *icon = g_strconcat (APPLICATION_ID, "-symbolic", NULL);
  g_autofree char *path = NULL;
  g_autofree char *base64 = NULL;
  gconstpointer data;
  gsize size;

  icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  paintable = gtk_icon_theme_lookup_icon (icon_theme,
                                          icon,
                                          NULL,
                                          128,
                                          1,
                                          GTK_TEXT_DIR_LTR,
                                          GTK_ICON_LOOKUP_NONE);
  if (paintable) {
    g_autoptr (GFile) file = gtk_icon_paintable_get_file (paintable);

    path = g_file_get_path (file);
  }

  bytes = load_application_icon (path);
  if (!bytes)
    return g_strdup ("ephy-resource:///org/gnome/epiphany/page-icons/web-watermark.svg");

  data = g_bytes_get_data (bytes, &size);
  base64 = g_base64_encode (data, size);
  return g_strconcat ("data:image/png;base64,", base64, NULL);
}

gboolean
ephy_about_handler_handle_application_icon (EphyEngineSchemeRequest *request)
{
  const char *path = ephy_engine_uri_scheme_request_get_path (request);
  EphyEngineView *view = ephy_engine_uri_scheme_request_get_web_view (request);
  const char *token;
  GHashTable *icons = g_object_get_data (G_OBJECT (view), EPHY_APPLICATION_ICONS_KEY);
  GBytes *bytes = NULL;
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GError) error = NULL;

  if (!path || !g_str_has_prefix (path, EPHY_APPLICATION_ICON_PREFIX))
    return FALSE;
  token = path + strlen (EPHY_APPLICATION_ICON_PREFIX);
  if (g_strcmp0 (ephy_engine_web_view_get_uri (view), "ephy-about:applications") == 0 &&
      icons && g_uuid_string_is_valid (token))
    bytes = g_hash_table_lookup (icons, token);
  if (!bytes) {
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown application icon");
    ephy_engine_uri_scheme_request_finish_error (request, error);
    return TRUE;
  }

  stream = g_memory_input_stream_new_from_bytes (bytes);
  ephy_engine_uri_scheme_request_finish (request, stream, g_bytes_get_size (bytes), "image/png");
  return TRUE;
}

static void
ephy_about_handler_finalize (GObject *object)
{
  EphyAboutHandler *handler = EPHY_ABOUT_HANDLER (object);

  g_clear_object (&handler->smaps);

  G_OBJECT_CLASS (ephy_about_handler_parent_class)->finalize (object);
}

static void
ephy_about_handler_init (EphyAboutHandler *handler)
{
}

static void
ephy_about_handler_class_init (EphyAboutHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ephy_about_handler_finalize;
}

static EphySMaps *
ephy_about_handler_get_smaps (EphyAboutHandler *handler)
{
  if (!handler->smaps)
    handler->smaps = ephy_smaps_new ();

  return handler->smaps;
}

static void
ephy_about_handler_finish_request (EphyEngineSchemeRequest *request,
                                   gchar                  *data,
                                   gssize                  data_length)
{
  GInputStream *stream;

  data_length = data_length != -1 ? data_length : (gssize)strlen (data);
  stream = g_memory_input_stream_new_from_data (data, data_length, g_free);
  ephy_engine_uri_scheme_request_finish (request, stream, data_length, "text/html");
  g_object_unref (stream);
}

static void
handle_memory_finished_cb (EphyAboutHandler       *handler,
                           GAsyncResult           *result,
                           EphyEngineSchemeRequest *request)
{
  GString *data_str;
  gsize data_length;
  char *memory;

  data_str = g_string_new ("<html>");

  memory = g_task_propagate_pointer (G_TASK (result), NULL);
  if (memory) {
    g_string_append_printf (data_str, "<head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "</head><body>"
                            "<div id='memory'>",
                            _("Memory usage"));

    g_string_append_printf (data_str, "<h1>%s</h1>", _("Memory usage"));
    g_string_append (data_str, memory);
    g_free (memory);

    g_string_append (data_str, "</div>");
  }

  g_string_append (data_str, "</html>");

  data_length = data_str->len;
  ephy_about_handler_finish_request (request, g_string_free_and_steal (data_str), data_length);
  g_object_unref (request);
}

static void
handle_memory_sync (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  EphyAboutHandler *handler = EPHY_ABOUT_HANDLER (source_object);

  g_task_return_pointer (task,
                         ephy_smaps_to_html (ephy_about_handler_get_smaps (handler)),
                         g_free);
}

static gboolean
ephy_about_handler_handle_memory (EphyAboutHandler       *handler,
                                  EphyEngineSchemeRequest *request)
{
  GTask *task;

  task = g_task_new (handler, NULL,
                     (GAsyncReadyCallback)handle_memory_finished_cb,
                     g_object_ref (request));
  g_task_set_source_tag (task, ephy_about_handler_handle_memory);
  g_task_run_in_thread (task, handle_memory_sync);
  g_object_unref (task);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_about (EphyAboutHandler       *handler,
                                 EphyEngineSchemeRequest *request)
{
  char *data;
  const char *application_name;
  const char *about_title;
  g_autofree char *version = NULL;
  g_autofree char *engine_version = NULL;
  g_autofree char *engine_version_html = NULL;

  if (g_str_equal (PROFILE, "Canary")) {
    application_name = _("Gnosis Canary");
    about_title = _("About Gnosis Canary");
#if TECH_PREVIEW
  } else {
    application_name = _("Gnosis Technology Preview");
    about_title = _("About Gnosis Technology Preview");
#else
  } else {
    application_name = _("Gnosis");
    about_title = _("About Gnosis");
#endif
  }

  version = g_strdup_printf (_("Version %s"), VERSION);
  engine_version = ephy_engine_dup_version_info ();
  engine_version_html = g_markup_escape_text (engine_version, -1);

  data = g_strdup_printf ("<html><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                          "</head><body>"
                          "<div id=\"about-app\">"
                          "<div class=\"dialog\">"
                          "<img id=\"about-icon\" src=\"ephy-resource:///org/gnome/epiphany/page-icons/application.svg\"/>"
                          "<h1 id=\"about-title\">%s</h1>"
                          "<h2 id=\"about-subtitle\">%s</h2>"
                          "<p id=\"about-tagline\">%s</p>"
                          "<table class=\"properties\">"
                          "<tr><td class=\"prop-label\">%s</td><td class=\"prop-value\">%s</td></tr>"
                          "</table>"
                          "</div></div></body></html>",
                          about_title,
                          application_name,
                          version,
                          _("A simple, clean, beautiful view of the web"),
                          _("Engine"), engine_version_html);

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_epiphany (EphyAboutHandler       *handler,
                                    EphyEngineSchemeRequest *request)
{
  char *data;

  data = g_strdup_printf ("<html class=\"epiphany-html\"><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                          "</head><body class=\"epiphany-body\">"
                          "<div id=\"ephytext\">"
                          "« Il semble que la perfection soit atteinte non quand il n'y a plus rien à"
                          " ajouter, mais quand il n'y a plus rien à retrancher. »"
                          "</div>"
                          "<div id=\"from\">"
                          "<!-- Terre des Hommes, III: L'Avion, p. 60 -->"
                          "Antoine de Saint-Exupéry"
                          "</div></body></html>",
                          _("Web"));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static void
handle_applications_finished_cb (EphyAboutHandler       *handler,
                                 GAsyncResult           *result,
                                 EphyEngineSchemeRequest *request)
{
  EphyEngineView *view;
  GString *data_str;
  gsize data_length;
  GList *applications, *p;
  GHashTable *loaded_icons = g_task_get_task_data (G_TASK (result));
  GHashTable *icons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);

  view = ephy_engine_uri_scheme_request_get_web_view (request);
  g_object_set_data_full (G_OBJECT (view), EPHY_APPLICATION_ICONS_KEY, icons, (GDestroyNotify)g_hash_table_unref);
  ephy_web_view_register_message_handler (EPHY_WEB_VIEW (view), EPHY_WEB_VIEW_ABOUT_APPS_MESSAGE_HANDLER, EPHY_WEB_VIEW_REGISTER_MESSAGE_HANDLER_FOR_CURRENT_PAGE);

  data_str = g_string_new (NULL);
  applications = g_task_propagate_pointer (G_TASK (result), NULL);

  if (g_list_length (applications) > 0) {
    g_string_append_printf (data_str, "<html><head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "<script>"
                            "  function launchWebApp(appID, appName) {"
                            "    Jupiter.postMessage('aboutApps', {action: 'launch', app: appID, name: appName, page: %" G_GUINT64_FORMAT "});"
                            "  }"
                            "  function deleteWebApp(appID, appName) {"
                            "    Jupiter.postMessage('aboutApps', {action: 'remove', app: appID, name: appName, page: %" G_GUINT64_FORMAT "});"
                            "  }"
                            "</script>"
                            "</head><div id=\"applications\"><body class=\"applications-body\"><h1>%s</h1>"
                            "<p>%s</p>",
                            _("Apps"),
                            ephy_engine_web_view_get_page_id (view),
                            ephy_engine_web_view_get_page_id (view),
                            _("Apps"),
                            _("List of installed web apps"));

    g_string_append (data_str, "<table>");

    for (p = applications; p; p = p->next) {
      EphyWebApplication *app = (EphyWebApplication *)p->data;
      g_autofree char *application_id = g_strconcat (EPHY_WEB_APP_GAPPLICATION_ID_PREFIX, app->id, NULL);
      g_autofree char *icon_uri = NULL;
      g_autofree char *encoded_name = NULL;
      g_autofree char *encoded_url = NULL;
      g_autoptr (GDate) date = NULL;
      char install_date[128];

      if (!g_application_id_is_valid (application_id) || ephy_web_application_is_system (app))
        continue;

      date = g_date_new ();
      g_date_set_time_t (date, (time_t)app->install_date_uint64);
      g_date_strftime (install_date, 127, "%x", date);

      icon_uri = application_icon_uri (icons, g_hash_table_lookup (loaded_icons, app->id));
      encoded_name = ephy_encode_for_html (app->name);
      encoded_url = ephy_encode_for_html (app->url);
      g_string_append_printf (data_str,
                              "<tbody><tr id =\"%s\">"
                              "<td class=\"icon\"><img width=64 height=64 src=\"%s\"></img></td>"
                              "<td class=\"data\"><div class=\"appname\">%s</div><div class=\"appurl\">%s</div></td>"
                              "<td class=\"input\"><input type=\"button\" value=\"%s\" "
                              "onclick=\"const appRow = this.closest('tr'); launchWebApp(appRow.id, appRow.querySelector('.appname').innerText);\" "
                              "class=\"suggested-action\"></td>  "
                              "<td class=\"input\"><input type=\"button\" value=\"%s\" "
                              "onclick=\"const appRow = this.closest('tr'); deleteWebApp(appRow.id, appRow.querySelector('.appname').innerText);\" "
                              "class=\"destructive-action\"></td>"
                              "<td class=\"date\">%s <br /> %s</td></tr></tbody>",
                              app->id, icon_uri, encoded_name, encoded_url, _("Launch"), _("Delete"),
                              /* Note for translators: this refers to the installation date. */
                              _("Installed on:"), install_date);
    }

    g_string_append (data_str, "</table></div></body></html>");
  } else {
    GtkIconTheme *icon_theme;
    g_autoptr (GtkIconPaintable) paintable = NULL;
    g_autofree char *path = NULL;
    g_autofree char *icon_uri = NULL;
    g_autoptr (GBytes) icon_bytes = NULL;

    g_string_append_printf (data_str, "<html><head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "</head><body class=\"applications-body\">",
                            _("Apps"));

    icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    paintable = gtk_icon_theme_lookup_icon (icon_theme,
                                            "application-x-addon-symbolic",
                                            NULL,
                                            128,
                                            1,
                                            GTK_TEXT_DIR_LTR,
                                            GTK_ICON_LOOKUP_NONE);

    if (paintable) {
      g_autoptr (GFile) file = gtk_icon_paintable_get_file (paintable);

      path = g_file_get_path (file);
    }

    icon_bytes = load_application_icon (path);
    icon_uri = application_icon_uri (icons, icon_bytes);
    g_string_append_printf (data_str,
                            "  <div id=\"overview\" class=\"overview-empty\">\n"
                            "    <img src=\"%s\" alt=\"\"/>\n"
                            "    <div><h1>%s</h1></div>\n"
                            "    <div><p>%s</p></div>\n"
                            "  </div>\n"
                            "</body></html>\n",
                            icon_uri,
                            /* Displayed when opening applications without any installed web apps. */
                            _("Apps"), _("You can add your favorite website by clicking <b>Install as Web App…</b> within the page menu."));
  }

  ephy_web_application_free_application_list (applications);

  data_length = data_str->len;
  ephy_about_handler_finish_request (request, g_string_free_and_steal (data_str), data_length);
  g_object_unref (request);
}

static void
handle_applications_sync (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  GList *applications = ephy_web_application_get_application_list ();
  GHashTable *icons = g_task_get_task_data (task);

  for (GList *p = applications; p; p = p->next) {
    EphyWebApplication *app = p->data;
    g_autofree char *application_id = g_strconcat (EPHY_WEB_APP_GAPPLICATION_ID_PREFIX, app->id, NULL);
    GBytes *bytes;

    if (!g_application_id_is_valid (application_id) || ephy_web_application_is_system (app))
      continue;
    bytes = load_application_icon (xdp_portal_running_under_sandbox () ? app->tmp_icon_path : app->icon_path);
    if (bytes)
      g_hash_table_insert (icons, g_strdup (app->id), bytes);
  }
  g_task_return_pointer (task, applications, (GDestroyNotify)ephy_web_application_free_application_list);
}

static gboolean
ephy_about_handler_handle_applications (EphyAboutHandler       *handler,
                                        EphyEngineSchemeRequest *request)
{
  GTask *task;

  task = g_task_new (handler, NULL,
                     (GAsyncReadyCallback)handle_applications_finished_cb,
                     g_object_ref (request));
  g_task_set_source_tag (task, ephy_about_handler_handle_applications);
  g_task_set_task_data (task,
                        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref),
                        (GDestroyNotify)g_hash_table_unref);
  g_task_run_in_thread (task, handle_applications_sync);
  g_object_unref (task);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_newtab (EphyAboutHandler       *handler,
                                  EphyEngineSchemeRequest *request);

static void
history_service_query_urls_cb (EphyHistoryService     *history,
                               GAsyncResult           *result,
                               EphyEngineSchemeRequest *request_param)
{
  g_autoptr (EphyEngineSchemeRequest) request = request_param;
  EphyEmbedShell *shell;
  GString *data_str;
  gsize data_length;
  g_autofree char *lang = NULL;
  g_autofree char *empty_icon_uri = NULL;
  g_autofree char *empty_icon_attribute = NULL;
  g_autofree char *empty_title_attribute = NULL;
  g_autofree char *empty_title_entity = NULL;
  g_autofree char *empty_subtitle_attribute = NULL;
  g_autofree char *empty_subtitle_entity = NULL;
  g_autofree char *remove_attribute = NULL;
  g_autofree char *pin_attribute = NULL;
  g_autofree char *unpin_attribute = NULL;
  GList *l;
  g_autolist (EphyHistoryURL) urls = NULL;
  g_autoptr (GError) error = NULL;
  guint list_length;

  urls = ephy_history_service_query_urls_finish (history, result, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to query overview URLs: %s", error->message);
    return;
  }

  shell = ephy_embed_shell_get_default ();

  data_str = g_string_new (NULL);

  lang = g_strdup (pango_language_to_string (gtk_get_default_language ()));
  g_strdelimit (lang, "_-@", '\0');

  empty_icon_uri = overview_empty_icon_uri ();
  empty_icon_attribute = ephy_encode_for_html_attribute (empty_icon_uri);
  empty_title_attribute = ephy_encode_for_html_attribute (_("Welcome to Web"));
  empty_title_entity = ephy_encode_for_html (_("Welcome to Web"));
  empty_subtitle_attribute = ephy_encode_for_html_attribute (_("Start browsing and your most-visited sites will appear here."));
  empty_subtitle_entity = ephy_encode_for_html (_("Start browsing and your most-visited sites will appear here."));
  remove_attribute = ephy_encode_for_html_attribute (_("Remove from overview"));
  pin_attribute = ephy_encode_for_html_attribute (_("Pin to overview"));
  unpin_attribute = ephy_encode_for_html_attribute (_("Unpin from overview"));

  g_string_append_printf (data_str,
                          "<html xml:lang=\"%s\" lang=\"%s\" dir=\"%s\">\n"
                          "<head>\n"
                          "  <title>%s</title>\n"
                          "  <meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\" />\n"
                          "  <meta name=\"viewport\" content=\"width=device-width\">"
                          "  <link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">\n"
                          "  <script defer src=\"ephy-resource:///org/gnome/epiphany/overview.js\"></script>\n"
                          "</head>\n"
                          "<body>\n",
                          lang, lang,
                          ((gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL) ? "rtl" : "ltr"),
                          _(NEW_TAB_PAGE_TITLE));

  list_length = g_list_length (urls);

  g_string_append_printf (data_str,
                          "  <div id=\"overview\"%s data-empty-icon=\"%s\" data-empty-title=\"%s\""
                          " data-empty-subtitle=\"%s\" data-remove-label=\"%s\""
                          " data-pin-label=\"%s\" data-unpin-label=\"%s\">\n",
                          list_length == 0 ? " class=\"overview-empty\"" : "",
                          empty_icon_attribute, empty_title_attribute, empty_subtitle_attribute,
                          remove_attribute, pin_attribute, unpin_attribute);

  if (list_length == 0) {
    g_string_append_printf (data_str,
                            "    <img src=\"%s\"/>\n"
                            "    <div><h1>%s</h1></div>\n"
                            "    <div><p>%s</p></div>\n"
                            "  </div>\n"
                            "</body></html>\n",
                            empty_icon_attribute,
                            /* Displayed when opening the browser for the first time. */
                            empty_title_entity, empty_subtitle_entity);
    goto out;
  }

  g_string_append (data_str,
                   "<div id=\"most-visited-grid\">\n");

  for (l = urls; l; l = g_list_next (l)) {
    EphyHistoryURL *url = (EphyHistoryURL *)l->data;
    g_autofree char *thumbnail_uri = NULL;
    g_autofree char *thumbnail_attribute = NULL;
    g_autofree char *thumbnail_style = NULL;
    g_autofree char *entity_encoded_title = NULL;
    g_autofree char *attribute_encoded_title = NULL;
    g_autofree char *encoded_url = NULL;

    thumbnail_uri = ephy_embed_shell_get_overview_thumbnail_uri (shell, url->url);
    if (thumbnail_uri) {
      thumbnail_attribute = ephy_encode_for_html_attribute (thumbnail_uri);
      thumbnail_style = g_strdup_printf (" style=\"background-image: url(&quot;%s&quot;); background-size: 100%%; background-position: top;\"",
                                         thumbnail_attribute);
    } else {
      ephy_embed_shell_schedule_thumbnail_update (shell, url);
    }

    /* Title and URL are controlled by web content and could be malicious. */
    entity_encoded_title = ephy_encode_for_html (url->title);
    attribute_encoded_title = ephy_encode_for_html_attribute (url->title);
    encoded_url = ephy_encode_for_html_attribute (url->url);
    g_string_append_printf (data_str,
                            "<a class=\"%s\" title=\"%s\" href=\"%s\">"
                            "  <div class=\"overview-close-button\" title=\"%s\"></div>"
                            "  <div class=\"overview-pin-button\" title=\"%s\"></div>"
                            "  <span class=\"overview-thumbnail\"%s></span>"
                            "  <span class=\"overview-title\">%s</span>"
                            "</a>",
                            url->pinned ? "overview-item overview-item-pinned" : "overview-item",
                            attribute_encoded_title, encoded_url, remove_attribute,
                            url->pinned ? unpin_attribute : pin_attribute,
                            thumbnail_style ? thumbnail_style : "",
                            entity_encoded_title);
  }

  g_string_append (data_str,
                   "  </div>\n"
                   "  </div>\n"
                   "</body></html>\n");

out:
  data_length = data_str->len;
  ephy_about_handler_finish_request (request, g_string_free_and_steal (data_str), data_length);
}

static gboolean
ephy_about_handler_handle_newtab (EphyAboutHandler       *handler,
                                  EphyEngineSchemeRequest *request)
{
  char *data;

  data = g_strdup_printf ("<html style=\"color-scheme: light dark;\"><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "</head><body></body></html>",
                          _(NEW_TAB_PAGE_TITLE));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

EphyHistoryQuery *
ephy_history_query_new_for_overview (void)
{
  EphyHistoryQuery *query;

  query = ephy_history_query_new ();
  query->sort_type = EPHY_HISTORY_SORT_MOST_VISITED;
  query->limit = EPHY_ABOUT_OVERVIEW_MAX_ITEMS;
  query->ignore_hidden = TRUE;
  query->ignore_local = TRUE;

  return query;
}

static gboolean
ephy_about_handler_handle_html_overview (EphyAboutHandler       *handler,
                                         EphyEngineSchemeRequest *request)
{
  EphyEngineView *view = ephy_engine_uri_scheme_request_get_web_view (request);
  EphyEmbedShell *shell = ephy_embed_shell_get_default ();
  EphyHistoryService *history;
  EphyHistoryQuery *query;

  if (!ephy_engine_uri_scheme_request_is_main_resource (request) ||
      !ephy_embed_shell_can_show_overview (shell, view))
    return ephy_about_handler_handle_newtab (handler, request);

  ephy_web_view_register_message_handler (EPHY_WEB_VIEW (view),
                                          EPHY_WEB_VIEW_OVERVIEW_MESSAGE_HANDLER,
                                          EPHY_WEB_VIEW_REGISTER_MESSAGE_HANDLER_FOR_CURRENT_PAGE);
  history = ephy_embed_shell_get_global_history_service (shell);
  query = ephy_history_query_new_for_overview ();
  ephy_history_service_query_urls (history, query, NULL,
                                   (GAsyncReadyCallback)history_service_query_urls_cb,
                                   g_object_ref (request));
  ephy_history_query_free (query);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_incognito (EphyAboutHandler       *handler,
                                     EphyEngineSchemeRequest *request)
{
  char *data;

  if (ephy_embed_shell_get_mode (ephy_embed_shell_get_default ()) != EPHY_EMBED_SHELL_MODE_INCOGNITO)
    return FALSE;

  data = g_strdup_printf ("<html>\n"
                          "<div dir=\"%s\">\n"
                          "<head>\n"
                          "<title>%s</title>\n"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">\n"
                          "</head>\n"
                          "<body class=\"incognito-body\">\n"
                          "  <img class=\"incognito-body-image\" src=\"ephy-resource:///org/gnome/epiphany/page-icons/private-mode.svg\">\n" \
                          "  <br/>\n"
                          "  <h1>%s</h1>\n"
                          "  <p>%s</p>\n"
                          "  <p><strong>%s</strong> %s</p>\n"
                          "</body>\n"
                          "</div>\n"
                          "</html>\n",
                          gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL ? "rtl" : "ltr",
                          _("Private Browsing"),
                          _("Private Browsing"),
                          _("You are currently browsing incognito. Pages viewed in this "
                            "mode will not show up in your browsing history and all stored "
                            "information will be cleared when you close the window. Files you "
                            "download will be kept."),
                          _("Incognito mode hides your activity only from people using this "
                            "computer."),
                          _("It will not hide your activity from your employer if you are at "
                            "work. Your internet service provider, your government, other "
                            "governments, the websites that you visit, and advertisers on "
                            "these websites may still be tracking you."));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static void
ephy_about_handler_handle_blank (EphyAboutHandler       *handler,
                                 EphyEngineSchemeRequest *request)
{
  ephy_about_handler_finish_request (request, g_strdup ("<html></html>"), -1);
}

EphyAboutHandler *
ephy_about_handler_new (void)
{
  return EPHY_ABOUT_HANDLER (g_object_new (EPHY_TYPE_ABOUT_HANDLER, NULL));
}

void
ephy_about_handler_handle_request (EphyAboutHandler       *handler,
                                   EphyEngineSchemeRequest *request)
{
  const char *path;
  gboolean handled = FALSE;

  path = ephy_engine_uri_scheme_request_get_path (request);

  if (g_strcmp0 (path, "memory") == 0)
    handled = ephy_about_handler_handle_memory (handler, request);
  else if (g_strcmp0 (path, "epiphany") == 0)
    handled = ephy_about_handler_handle_epiphany (handler, request);
  else if (g_strcmp0 (path, "applications") == 0)
    handled = ephy_about_handler_handle_applications (handler, request);
  else if (g_strcmp0 (path, "newtab") == 0)
    handled = ephy_about_handler_handle_newtab (handler, request);
  else if (g_strcmp0 (path, "overview") == 0)
    handled = ephy_about_handler_handle_html_overview (handler, request);
  else if (g_strcmp0 (path, "incognito") == 0)
    handled = ephy_about_handler_handle_incognito (handler, request);
  else if (!path || path[0] == '\0' || g_strcmp0 (path, "Web") == 0 || g_strcmp0 (path, "web") == 0)
    handled = ephy_about_handler_handle_about (handler, request);

  if (!handled)
    ephy_about_handler_handle_blank (handler, request);
}
