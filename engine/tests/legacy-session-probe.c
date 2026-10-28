/* SPDX-License-Identifier: GPL-3.0-or-later
 * Preload into a disposable packaged GTK run with GSETTINGS_BACKEND=memory.
 * Assert ordinary startup never creates WebKit's retired network session,
 * then explicitly request it and verify laziness, identity and ITP policy.
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

static guint regular_created, ephemeral_created, itp_calls, credential_calls;
static gboolean last_itp, last_credentials;
static gint64 changed;
static gboolean initial_itp;
static GSettings *settings;
static gpointer shell;
static gpointer (*website_data_manager)(gpointer);
static guint stage;

static void fail(const char *reason) {
  g_printerr("LEGACY_SESSION_FAIL %s\n", reason);
  _exit(77);
}

gpointer webkit_network_session_new(const char *data_directory, const char *cache_directory) {
  gpointer (*real)(const char *, const char *) = dlsym(RTLD_NEXT, "webkit_network_session_new");
  regular_created++;
  return real(data_directory, cache_directory);
}
gpointer webkit_network_session_new_ephemeral(void) {
  gpointer (*real)(void) = dlsym(RTLD_NEXT, "webkit_network_session_new_ephemeral");
  ephemeral_created++;
  return real();
}
void webkit_network_session_set_itp_enabled(gpointer session, gboolean enabled) {
  void (*real)(gpointer, gboolean) = dlsym(RTLD_NEXT, "webkit_network_session_set_itp_enabled");
  itp_calls++; last_itp = enabled;
  real(session, enabled);
}
void webkit_network_session_set_persistent_credential_storage_enabled(gpointer session, gboolean enabled) {
  void (*real)(gpointer, gboolean) = dlsym(RTLD_NEXT, "webkit_network_session_set_persistent_credential_storage_enabled");
  credential_calls++; last_credentials = enabled;
  real(session, enabled);
}

static gboolean check(gpointer unused) {
  (void)unused;
  GApplication *app = g_application_get_default();
  if (!GTK_IS_APPLICATION(app) || !gtk_application_get_active_window(GTK_APPLICATION(app)))
    return G_SOURCE_CONTINUE;
  if (stage == 0) {
    if (g_strcmp0(g_getenv("GSETTINGS_BACKEND"), "memory"))
      fail("requires GSETTINGS_BACKEND=memory and a disposable --profile");
    if (regular_created || ephemeral_created || itp_calls || credential_calls)
      fail("ordinary startup touched WebKit's legacy session");
    GSettings *(*get_settings)(const char *) = dlsym(RTLD_DEFAULT, "ephy_settings_get");
    gpointer (*get_shell)(void) = dlsym(RTLD_DEFAULT, "ephy_embed_shell_get_default");
    website_data_manager = dlsym(RTLD_DEFAULT, "ephy_embed_shell_get_website_data_manager");
    if (!get_settings || !get_shell || !website_data_manager)
      fail("shell accessors unavailable");
    settings = get_settings("com.neg-zero.Gnosis.web");
    shell = get_shell();
    initial_itp = g_settings_get_boolean(settings, "enable-itp");
    if (!g_settings_set_boolean(settings, "enable-itp", !initial_itp))
      fail("cannot change isolated test preference");
    changed = g_get_monotonic_time(); stage = 1;
  } else if (stage == 1 && g_get_monotonic_time() - changed > 200000) {
    if (regular_created || ephemeral_created || itp_calls || credential_calls)
      fail("ITP change eagerly opened the legacy session");
    gpointer first = website_data_manager(shell), second = website_data_manager(shell);
    gboolean expected_ephemeral = g_strcmp0(g_getenv("GNOSIS_EXPECT_LEGACY_EPHEMERAL"), "1") == 0;
    if (!first || first != second || regular_created + ephemeral_created != 1 ||
        ephemeral_created != expected_ephemeral || regular_created != !expected_ephemeral)
      fail("legacy session is not lazy, idempotent or correctly private");
    if (itp_calls != 1 || last_itp != !initial_itp || credential_calls != 1 || last_credentials)
      fail("lazy session lost current ITP/credential configuration");
    g_settings_set_boolean(settings, "enable-itp", initial_itp);
    changed = g_get_monotonic_time(); stage = 2;
  } else if (stage == 2 && g_get_monotonic_time() - changed > 200000) {
    if (itp_calls != 2 || last_itp != initial_itp || regular_created + ephemeral_created != 1)
      fail("existing legacy session did not follow later ITP changes");
    g_print("LEGACY_SESSION_PASS no startup session, lazy identity, privacy, current/later ITP and disabled credential storage\n");
    g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
__attribute__((constructor)) static void setup(void) {
  char executable[2048];
  ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
  if (length < 0) return;
  executable[length] = 0;
  if (strstr(executable, "/.gnosis-wrapped"))
    g_timeout_add_seconds(5, check, NULL);
}
