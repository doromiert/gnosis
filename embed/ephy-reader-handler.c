/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2020-2026 Jan-Michael Brummer <jan.brummer@tabos.org>
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
#include "ephy-json-utils.h"
#include "ephy-engine-scheme.h"
#include "ephy-engine-view.h"

#include "ephy-reader-handler.h"

#include <string.h>

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "ephy-embed-container.h"
#include "ephy-embed-shell.h"
#include "ephy-lib-type-builtins.h"
#include "ephy-output-encoding.h"
#include "ephy-settings.h"
#include "ephy-web-view.h"

struct _EphyReaderHandler {
  GObject parent_instance;

  GList *outstanding_requests;
};

G_DEFINE_FINAL_TYPE (EphyReaderHandler, ephy_reader_handler, G_TYPE_OBJECT)

typedef struct {
  EphyReaderHandler *source_handler;
  EphyEngineSchemeRequest *scheme_request;
  EphyEngineView *web_view;
  GCancellable *cancellable;
  gulong load_changed_id;
} EphyReaderRequest;

static EphyReaderRequest *
ephy_reader_request_new (EphyReaderHandler      *handler,
                         EphyEngineSchemeRequest *request)
{
  EphyReaderRequest *reader_request;

  reader_request = g_new (EphyReaderRequest, 1);
  reader_request->source_handler = g_object_ref (handler);
  reader_request->scheme_request = g_object_ref (request);
  reader_request->web_view = NULL; /* created only if required */
  reader_request->cancellable = g_cancellable_new ();
  reader_request->load_changed_id = 0;

  return reader_request;
}

static void
ephy_reader_request_free (EphyReaderRequest *request)
{
  if (request->load_changed_id > 0)
    g_signal_handler_disconnect (request->web_view, request->load_changed_id);

  g_object_unref (request->source_handler);
  g_object_unref (request->scheme_request);
  g_clear_object (&request->web_view);

  g_cancellable_cancel (request->cancellable);
  g_object_unref (request->cancellable);

  g_free (request);
}

static void
finish_uri_scheme_request (EphyReaderRequest *request,
                           gchar             *data,
                           GError            *error)
{
  GInputStream *stream;
  gssize data_length;

  g_assert ((data && !error) || (!data && error));

  if (error) {
    ephy_engine_uri_scheme_request_finish_error (request->scheme_request, error);
  } else {
    data_length = MIN (strlen (data), G_MAXSSIZE);
    stream = g_memory_input_stream_new_from_data (data, data_length, g_free);
    ephy_engine_uri_scheme_request_finish (request->scheme_request, stream, data_length, "text/html");
    g_object_unref (stream);
  }

  request->source_handler->outstanding_requests =
    g_list_remove (request->source_handler->outstanding_requests,
                   request);

  ephy_reader_request_free (request);
}

static const char *
enum_nick (GType enum_type,
           int   value)
{
  GEnumClass *enum_class;
  const GEnumValue *enum_value;
  const char *nick = NULL;

  enum_class = g_type_class_ref (enum_type);
  enum_value = g_enum_get_value (enum_class, value);
  if (enum_value)
    nick = enum_value->value_nick;

  g_type_class_unref (enum_class);
  return nick;
}

static void
reader_html_evaluated_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  EphyEngineView *web_view = EPHY_ENGINE_VIEW (object);
  GTask *task = user_data;
  g_autoptr (JsonObject) script_result = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *byline = NULL;
  g_autofree gchar *encoded_byline = NULL;
  g_autofree gchar *content = NULL;
  g_autofree gchar *reading_time = NULL;
  g_autofree gchar *encoded_title = NULL;
  g_autoptr (GString) html = NULL;
  g_autoptr (GBytes) style_css = NULL;
  const gchar *title;
  const gchar *font_style;
  const gchar *color_scheme;
  AdwStyleManager *style_manager;
  JsonObject *article;

  script_result = ephy_engine_web_view_evaluate_javascript_finish (web_view, result, &error);
  if (!script_result) {
    g_task_return_error (task, g_steal_pointer (&error));
    g_object_unref (task);
    return;
  }

  article = ephy_json_object_get_object (script_result, "value");
  if (!article) {
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Readability returned no article");
    g_task_return_error (task, g_steal_pointer (&error));
    g_object_unref (task);
    return;
  }
  byline = ephy_json_object_dup_string (article, "byline");
  content = ephy_json_object_dup_string (article, "content");
  reading_time = ephy_json_object_dup_string (article, "reading_time");
  /* Readability captured the source title before the reader navigation. */
  title = ephy_json_object_get_string (article, "title");
  if (!title || !*title)
    title = ephy_engine_web_view_get_title (web_view);

  encoded_byline = byline ? ephy_encode_for_html (byline) : g_strdup ("");
  encoded_title = ephy_encode_for_html (title);

  html = g_string_new (NULL);
  style_css = g_resources_lookup_data ("/org/gnome/epiphany/readability/reader.css", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);

  font_style = enum_nick (EPHY_TYPE_PREFS_READER_FONT_STYLE,
                          g_settings_get_enum (EPHY_SETTINGS_READER,
                                               EPHY_PREFS_READER_FONT_STYLE));

  style_manager = adw_style_manager_get_default ();

  if (adw_style_manager_get_system_supports_color_schemes (style_manager))
    color_scheme = adw_style_manager_get_dark (style_manager) ? "dark" : "light";
  else
    color_scheme = enum_nick (EPHY_TYPE_PREFS_READER_COLOR_SCHEME,
                              g_settings_get_enum (EPHY_SETTINGS_READER,
                                                   EPHY_PREFS_READER_COLOR_SCHEME));

  g_string_append_printf (html, "<style>%s</style>"
                          "<title>%s</title>"
                          "<meta http-equiv='Content-Type' content='text/html;' charset='UTF-8'>" \
                          "<meta http-equiv='Content-Security-Policy' content=\"script-src 'none'\">" \
                          "<body class='%s %s'>"
                          "<article>"
                          "<h2>"
                          "%s"
                          "</h2>"
                          "<i>"
                          "%s"
                          "</i>"
                          "<br>"
                          "<br>",
                          (gchar *)g_bytes_get_data (style_css, NULL),
                          encoded_title,
                          font_style,
                          color_scheme,
                          encoded_title,
                          encoded_byline);

  g_string_append (html, reading_time ? reading_time : "");
  g_string_append (html, "<br/><hr/>");

  /* We cannot encode the page content because it contains HTML tags inserted by
   * Readability.js. Upstream recommends that we use an XSS sanitizer like
   * DOMPurify plus Content-Security-Policy, but I'm not keen on adding more
   * bundled JS dependencies, and we have an advantage over Firefox in that we
   * don't need scripts to work at this point. So instead the above CSP
   * completely blocks all scripts, which should hopefully obviate the need for
   * a DOM purifier.
   *
   * Note the encoding for page title and byline is still required, as they're
   * not supposed to contain markup, and Readability.js unescapes them before
   * returning them to us.
   */
  g_string_append (html, content ? content : "");
  g_string_append (html, "</article>");
  g_string_append (html, "</body>");

  g_task_return_pointer (task, g_string_free (g_steal_pointer (&html), FALSE), g_free);
  g_object_unref (task);
}

void
ephy_reader_handler_get_html (EphyEngineView     *web_view,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
  GTask *task = g_task_new (web_view, cancellable, callback, user_data);
  gsize length;
  const char *script;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_resources_lookup_data ("/org/gnome/epiphany/readability/Readability.js",
                                                      G_RESOURCE_LOOKUP_FLAGS_NONE, &error);

  if (!bytes) {
    g_task_return_error (task, g_steal_pointer (&error));
    g_object_unref (task);
    return;
  }
  script = g_bytes_get_data (bytes, &length);
  ephy_engine_web_view_evaluate_javascript (web_view, script, length, NULL,
                                           "resource:///org/gnome/epiphany/readability/Readability.js",
                                           cancellable, reader_html_evaluated_cb, task);
}

char *
ephy_reader_handler_get_html_finish (EphyEngineView *web_view,
                                     GAsyncResult  *result,
                                     GError       **error)
{
  g_assert (g_task_is_valid (result, web_view));
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
reader_html_ready_cb (EphyEngineView    *web_view,
                      GAsyncResult     *result,
                      EphyReaderRequest *request)
{
  g_autoptr (GError) error = NULL;
  char *html = ephy_reader_handler_get_html_finish (web_view, result, &error);

  finish_uri_scheme_request (request, html, error);
}

static void
ephy_reader_request_begin_get_source_from_web_view (EphyReaderRequest *request,
                                                    EphyEngineView     *web_view)
{
  ephy_reader_handler_get_html (web_view, request->cancellable,
                                (GAsyncReadyCallback)reader_html_ready_cb, request);
}

static void
load_changed_cb (EphyEngineView     *web_view,
                 WebKitLoadEvent    load_event,
                 EphyReaderRequest *request)
{
  if (load_event == WEBKIT_LOAD_FINISHED) {
    g_clear_signal_handler (&request->load_changed_id, request->web_view);

    ephy_reader_request_begin_get_source_from_web_view (request, web_view);
  }
}

static void
ephy_reader_request_begin_get_source_from_uri (EphyReaderRequest *request,
                                               const char        *uri)
{
  EphyEmbedShell *shell = ephy_embed_shell_get_default ();
  EphyEngineSession *network_session = ephy_embed_shell_get_network_session (shell);

  g_assert (!request->web_view);
  request->web_view = EPHY_ENGINE_VIEW (g_object_ref_sink (g_object_new (EPHY_TYPE_ENGINE_VIEW,
                                                                        "network-session", network_session,
                                                                        NULL)));

  g_assert (request->load_changed_id == 0);
  request->load_changed_id = g_signal_connect (request->web_view, "load-changed",
                                               G_CALLBACK (load_changed_cb),
                                               request);

  ephy_engine_web_view_load_uri (request->web_view, uri);
}

static void
ephy_reader_request_start (EphyReaderRequest *request)
{
  g_autoptr (GUri) uri = NULL;
  const char *original_uri;
  EphyEngineView *web_view;
  char *prepared_html = NULL;

  original_uri = ephy_engine_uri_scheme_request_get_uri (request->scheme_request);
  uri = g_uri_parse (original_uri, G_URI_FLAGS_PARSE_RELAXED, NULL);

  if (!uri) {
    /* Can't assert because user could theoretically input something weird */
    g_autoptr (GError) error = g_error_new (G_IO_ERROR,
                                           G_IO_ERROR_INVALID_ARGUMENT,
                                 _("%s is not a valid URI"),
                                 original_uri);
    finish_uri_scheme_request (request, NULL, error);
    return;
  }

  web_view = ephy_engine_uri_scheme_request_get_web_view (request->scheme_request);
  if (web_view && EPHY_IS_WEB_VIEW (web_view))
    prepared_html = ephy_web_view_take_reader_html (EPHY_WEB_VIEW (web_view), original_uri);
  if (prepared_html) {
    finish_uri_scheme_request (request, prepared_html, NULL);
    return;
  }

  /* Explicit reader URLs load their source in a separate native view. */
  ephy_reader_request_begin_get_source_from_uri (request, original_uri + strlen (EPHY_READER_SCHEME ":"));
}

static void
cancel_outstanding_request (EphyReaderRequest *request)
{
  g_cancellable_cancel (request->cancellable);
}

static void
ephy_reader_handler_dispose (GObject *object)
{
  EphyReaderHandler *handler = EPHY_READER_HANDLER (object);

  if (handler->outstanding_requests) {
    g_list_foreach (handler->outstanding_requests, (GFunc)cancel_outstanding_request, NULL);
    g_clear_list (&handler->outstanding_requests, NULL);
  }

  G_OBJECT_CLASS (ephy_reader_handler_parent_class)->dispose (object);
}

static void
ephy_reader_handler_init (EphyReaderHandler *handler)
{
}

static void
ephy_reader_handler_class_init (EphyReaderHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ephy_reader_handler_dispose;
}

EphyReaderHandler *
ephy_reader_handler_new (void)
{
  return EPHY_READER_HANDLER (g_object_new (EPHY_TYPE_READER_HANDLER, NULL));
}

void
ephy_reader_handler_handle_request (EphyReaderHandler      *handler,
                                    EphyEngineSchemeRequest *scheme_request)
{
  EphyReaderRequest *reader_request;

  reader_request = ephy_reader_request_new (handler, scheme_request);
  handler->outstanding_requests = g_list_prepend (handler->outstanding_requests, reader_request);
  ephy_reader_request_start (reader_request);
}
