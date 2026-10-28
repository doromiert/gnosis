/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "prefs-debugging-page.h"
#include "ephy-settings.h"
#include "brave/debug-adapters.h"
#include <glib/gi18n.h>

struct _PrefsDebuggingPage { AdwPreferencesPage parent_instance; };
G_DEFINE_FINAL_TYPE (PrefsDebuggingPage, prefs_debugging_page, ADW_TYPE_PREFERENCES_PAGE)

static gboolean get_enabled (GValue *value, GVariant *variant, gpointer data) {
  gboolean enabled = TRUE;
  g_variant_lookup (variant, data, "b", &enabled);
  g_value_set_boolean (value, enabled);
  return TRUE;
}
static GVariant *set_enabled (const GValue *value, const GVariantType *type, gpointer data) {
  const char *id = data;
  g_autoptr (GVariant) current = g_settings_get_value (EPHY_SETTINGS_MAIN, "brave-adapter-overrides");
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  gboolean enabled;
  (void)type;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sb}"));
  g_variant_iter_init (&iter, current);
  while (g_variant_iter_loop (&iter, "{&sb}", &key, &enabled))
    if (!g_str_equal (key, id))
      g_variant_builder_add (&builder, "{sb}", key, enabled);
  g_variant_builder_add (&builder, "{sb}", id, g_value_get_boolean (value));
  return g_variant_builder_end (&builder);
}
static void reset_all (GtkButton *button, gpointer data) {
  GVariantBuilder builder;
  gboolean enabled = GPOINTER_TO_INT (data);
  (void)button;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sb}"));
  for (guint i = 0; i < G_N_ELEMENTS (gnosis_brave_adapters); ++i)
    g_variant_builder_add (&builder, "{sb}", gnosis_brave_adapters[i].id, enabled);
  g_settings_set_value (EPHY_SETTINGS_MAIN, "brave-adapter-overrides", g_variant_builder_end (&builder));
}
static void prefs_debugging_page_class_init (PrefsDebuggingPageClass *klass) { (void)klass; }
static void prefs_debugging_page_init (PrefsDebuggingPage *self) {
  AdwPreferencesGroup *group = NULL;
  const char *previous = NULL;
  GtkWidget *actions, *disable, *restore;
  adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (self), "debugging-page");
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (self), _("Debugging"));
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (self), "applications-engineering-symbolic");
  group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (group, _("Brave Adapters"));
  adw_preferences_group_set_description (group, _("Restart Gnosis after changing adapters. Disabled dialog adapters use Chromium's interface. Beforeunload checks, sandboxing and internal-page access checks stay enabled."));
  actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  disable = gtk_button_new_with_label (_("Disable All"));
  restore = gtk_button_new_with_label (_("Restore Defaults"));
  gtk_widget_set_hexpand (disable, TRUE); gtk_widget_set_hexpand (restore, TRUE);
  g_signal_connect (disable, "clicked", G_CALLBACK (reset_all), GINT_TO_POINTER (FALSE));
  g_signal_connect (restore, "clicked", G_CALLBACK (reset_all), GINT_TO_POINTER (TRUE));
  gtk_box_append (GTK_BOX (actions), disable); gtk_box_append (GTK_BOX (actions), restore);
  adw_preferences_group_add (group, actions);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (self), group);
  for (guint i = 0; i < G_N_ELEMENTS (gnosis_brave_adapters); ++i) {
    const GnosisBraveAdapter *adapter = &gnosis_brave_adapters[i];
    AdwSwitchRow *row;
    if (!previous || !g_str_equal (previous, adapter->group)) {
      group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (group, _(adapter->group));
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (self), group);
      previous = adapter->group;
    }
    row = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _(adapter->title));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _(adapter->description));
    g_settings_bind_with_mapping (EPHY_SETTINGS_MAIN, "brave-adapter-overrides", row, "active",
                                  G_SETTINGS_BIND_DEFAULT, get_enabled, set_enabled, (gpointer)adapter->id, NULL);
    adw_preferences_group_add (group, GTK_WIDGET (row));
  }
}
