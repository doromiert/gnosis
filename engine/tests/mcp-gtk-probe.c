/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only observation of native GTK controls in a disposable profile. */
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>

static GtkWidget *
find_named (GtkWidget *widget, const char *name)
{
  if (!g_strcmp0 (gtk_widget_get_name (widget), name)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *result = find_named (child, name);
    if (result) return result;
  }
  return NULL;
}

static gboolean
press_response (GtkWidget *widget, gboolean cleanup)
{
  if (GTK_IS_BUTTON (widget)) {
    const char *label = gtk_button_get_label (GTK_BUTTON (widget));
    g_autofree char *clean = g_strdup (label ? label : "");
    char *out = clean;
    for (char *p = clean; *p; ++p) if (*p != '_') *out++ = *p;
    *out = 0;
    if (!strcmp (clean, "No") || (cleanup &&
        (!strcmp (clean, "Close") || !strcmp (clean, "Close Tabs") || !strcmp (clean, "Close Window") ||
         !strcmp (clean, "Close All Tabs") || !strcmp (clean, "Close Anyway") || !strcmp (clean, "Leave")))) {
      return gtk_widget_activate (widget);
    }
  }
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    if (press_response (child, cleanup)) return TRUE;
  return FALSE;
}

static GtkWidget *
find_type (GtkWidget *widget, const char *type)
{
  if (!strcmp (G_OBJECT_TYPE_NAME (widget), type)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_type (child, type);
    if (found) return found;
  }
  return NULL;
}

static GtkWidget *
find_retry (GtkWidget *widget)
{
  if (GTK_IS_BUTTON (widget) && !g_strcmp0 (gtk_button_get_label (GTK_BUTTON (widget)), "Try Again")) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_retry (child);
    if (found) return found;
  }
  return NULL;
}

static void
menu_modes (GMenuModel *model, JsonArray *modes)
{
  for (int i = 0; i < g_menu_model_get_n_items (model); ++i) {
    g_autoptr(GVariant) action = g_menu_model_get_item_attribute_value (model, i, G_MENU_ATTRIBUTE_ACTION, G_VARIANT_TYPE_STRING);
    if (action && !strcmp (g_variant_get_string (action, NULL), "app.ai-browsing-mode")) {
      g_autoptr(GVariant) target = g_menu_model_get_item_attribute_value (model, i, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
      if (target) json_array_add_string_element (modes, g_variant_get_string (target, NULL));
    }
    const char *links[] = { G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU };
    for (guint j = 0; j < G_N_ELEMENTS (links); ++j) {
      g_autoptr(GMenuModel) child = g_menu_model_get_item_link (model, i, links[j]);
      if (child) menu_modes (child, modes);
    }
  }
}

static gboolean
observe (gpointer data)
{
  (void)data;
  GApplication *application = g_application_get_default ();
  const char *state_file = g_getenv ("GNOSIS_MCP_TEST_STATE");
  if (!application || !state_file || !GTK_IS_APPLICATION (application) ||
      !g_application_get_is_registered (application)) return G_SOURCE_CONTINUE;
  g_autoptr(JsonObject) state = json_object_new ();
  g_autoptr(JsonNode) node = json_node_new (JSON_NODE_OBJECT);
  g_autofree char *json = NULL;
  g_autofree char *capture_file = g_strconcat (state_file, ".capture", NULL);
  g_autofree char *capture = NULL;
  g_autofree char *cleanup_file = g_strconcat (state_file, ".cleanup", NULL);
  g_autofree char *command_file = g_strconcat (state_file, ".command", NULL);
  g_autofree char *command = NULL;
  g_file_get_contents (command_file, &command, NULL, NULL);
  gboolean cleanup = g_file_test (cleanup_file, G_FILE_TEST_EXISTS);
  gboolean capture_requested = g_file_get_contents (capture_file, &capture, NULL, NULL);
  JsonArray *windows = json_array_new ();
  guint index = 0;
  json_object_set_int_member (state, "pid", getpid ());
  g_autoptr(GVariant) mode = g_action_group_get_action_state (G_ACTION_GROUP (application), "ai-browsing-mode");
  json_object_set_string_member (state, "mode", mode ? g_variant_get_string (mode, NULL) : "unavailable");

  for (GList *it = gtk_application_get_windows (GTK_APPLICATION (application)); it; it = it->next) {
    GtkWindow *window = GTK_WINDOW (it->data);
    if (!ADW_IS_APPLICATION_WINDOW (window)) continue;
    AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window));
    const char *heading = dialog && ADW_IS_ALERT_DIALOG (dialog) ? adw_alert_dialog_get_heading (ADW_ALERT_DIALOG (dialog)) : NULL;
    if (dialog && ADW_IS_ALERT_DIALOG (dialog) &&
        (cleanup || (heading && strstr (heading, "Default Browser"))) &&
        !g_object_get_data (G_OBJECT (dialog), "gnosis-mcp-test-answered")) {
      /* Repeated activation restarts GTK's delayed click instead of answering. */
      g_object_set_data (G_OBJECT (dialog), "gnosis-mcp-test-answered", GINT_TO_POINTER (1));
      if (press_response (GTK_WIDGET (dialog), cleanup)) return G_SOURCE_CONTINUE;
      g_object_set_data (G_OBJECT (dialog), "gnosis-mcp-test-answered", NULL);
    }
    gboolean owned = g_object_get_data (G_OBJECT (window), "gnosis-ai-window") != NULL;
    if (command && !strcmp (command, "close-ai") && owned) {
      unlink (command_file); gtk_window_close (window); return G_SOURCE_CONTINUE;
    }
    GtkWidget *retry = find_retry (GTK_WIDGET (window));
    if (command && !owned && retry && (!strcmp (command, "show-retry") || !strcmp (command, "hide-retry"))) {
      GtkWidget *status = gtk_widget_get_ancestor (retry, ADW_TYPE_STATUS_PAGE);
      if (status) {
        adw_status_page_set_title (ADW_STATUS_PAGE (status), "Browser Process Stopped");
        gtk_widget_set_visible (status, !strcmp (command, "show-retry"));
      }
      unlink (command_file); return G_SOURCE_CONTINUE;
    }
    GtkWidget *control = find_named (GTK_WIDGET (window), "gnosis-ai-browsing");
    GtkWidget *cursor = find_named (GTK_WIDGET (window), "gnosis-ai-cursor");
    JsonObject *item = json_object_new ();
    guint64 (*get_uid)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_window_get_uid");
    g_autofree char *id = g_strdup_printf ("%" G_GUINT64_FORMAT, get_uid ? get_uid (window) : index);
    json_object_set_string_member (item, "window_id", id);
    json_object_set_boolean_member (item, "visible", gtk_widget_get_visible (GTK_WIDGET (window)));
    json_object_set_boolean_member (item, "mapped", gtk_widget_get_mapped (GTK_WIDGET (window)));
    json_object_set_boolean_member (item, "realized", gtk_widget_get_realized (GTK_WIDGET (window)));
    json_object_set_string_member (item, "title", gtk_window_get_title (window) ? gtk_window_get_title (window) : "");
    json_object_set_string_member (item, "dialog", heading ? heading : "");
    json_object_set_boolean_member (item, "header_control", control != NULL);
    json_object_set_boolean_member (item, "ai_window", owned);
    json_object_set_boolean_member (item, "cursor", cursor && gtk_widget_get_visible (cursor));
    json_object_set_boolean_member (item, "retry_pill", retry && gtk_widget_has_css_class (retry, "pill"));
    json_object_set_boolean_member (item, "retry_centered", retry && gtk_widget_get_halign (retry) == GTK_ALIGN_CENTER);
    json_object_set_boolean_member (item, "retry_visible", retry && gtk_widget_is_visible (retry));
    json_object_set_int_member (item, "retry_width", retry ? gtk_widget_get_width (retry) : 0);
    json_object_set_int_member (item, "window_width", gtk_widget_get_width (GTK_WIDGET (window)));
    GtkWidget *page_menu = find_type (GTK_WIDGET (window), "EphyPageMenuButton");
    GtkWidget *menu_button = page_menu ? find_type (page_menu, "GtkMenuButton") : NULL;
    JsonArray *modes = json_array_new ();
    if (menu_button) {
      GtkPopover *popover = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu_button));
      if (GTK_IS_POPOVER_MENU (popover)) menu_modes (gtk_popover_menu_get_menu_model (GTK_POPOVER_MENU (popover)), modes);
    }
    json_object_set_array_member (item, "menu_modes", modes);
    gpointer (*get_tabs)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_window_get_tab_view");
    int (*tab_count)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_tab_view_get_n_pages");
    GtkWidget *(*tab_at)(gpointer,int) = dlsym (RTLD_DEFAULT, "ephy_tab_view_get_nth_page");
    GtkWidget *(*get_view)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_embed_get_web_view");
    gboolean (*approved)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_engine_web_view_close_is_approved");
    const char *(*get_uri)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_engine_web_view_get_uri");
    gpointer (*get_active)(gpointer) = dlsym (RTLD_DEFAULT, "ephy_window_get_active_embed");
    JsonArray *tab_states = json_array_new ();
    if (get_tabs && tab_count && tab_at && get_view && approved && get_uri && get_active) {
      gpointer tab_view = get_tabs (window);
      for (int i = 0; i < tab_count (tab_view); ++i) {
        GtkWidget *embed = tab_at (tab_view, i);
        GtkWidget *view = get_view (embed);
        JsonObject *tab = json_object_new ();
        const char *uri = get_uri (view);
        json_object_set_string_member (tab, "uri", uri ? uri : "");
        json_object_set_boolean_member (tab, "approved", approved (view));
        json_object_set_boolean_member (tab, "rooted", gtk_widget_get_root (view) == GTK_ROOT (window));
        json_object_set_boolean_member (tab, "active", get_active (window) == embed);
        json_array_add_object_element (tab_states, tab);
      }
    }
    json_object_set_array_member (item, "tabs", tab_states);
    json_array_add_object_element (windows, item);
    if (capture_requested && capture && !strchr (capture, '/') && !strstr (capture, "..") &&
        gtk_widget_get_mapped (GTK_WIDGET (window))) {
      GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (window));
      g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (window));
      GtkSnapshot *snapshot = gtk_snapshot_new ();
      gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot),
                              gtk_widget_get_width (GTK_WIDGET (window)), gtk_widget_get_height (GTK_WIDGET (window)));
      g_autoptr(GskRenderNode) render = gtk_snapshot_free_to_node (snapshot);
      if (renderer && render) {
        g_autoptr(GdkTexture) texture = gsk_renderer_render_texture (renderer, render, NULL);
        g_autofree char *filename = g_strdup_printf ("%s.%s.%s.png", state_file, capture, id);
        gdk_texture_save_to_png (texture, filename);
      }
    }
    ++index;
  }
  if (capture_requested) unlink (capture_file);
  json_object_set_array_member (state, "windows", windows);
  json_node_set_object (node, state);
  json = json_to_string (node, FALSE);
  g_file_set_contents (state_file, json, -1, NULL);
  return G_SOURCE_CONTINUE;
}

__attribute__((constructor)) static void
start (void)
{
  char executable[2048];
  ssize_t length = readlink ("/proc/self/exe", executable, sizeof executable - 1);
  if (length < 0) return;
  executable[length] = 0;
  if (strstr (executable, "/.gnosis-wrapped") && g_getenv ("GNOSIS_MCP_TEST_STATE"))
    g_timeout_add (150, observe, NULL);
}
