/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <webkit/webkit.h>
#include <json-glib/json-glib.h>
#include "ephy-engine-favicon.h"
#include "ephy-engine-session.h"
#include "ephy-engine-response.h"
#include "ephy-engine-settings.h"
#include "ephy-engine-permission.h"
#include "ephy-engine-authentication.h"
#include "ephy-engine-client-certificate.h"
G_BEGIN_DECLS

/* open-external-uri carries (uri, committed_context_uri, initiator_known,
 * GCancellable). The token is cancelled when its document, mapped tab or owning
 * window goes away. Context is not a permission origin unless initiator_known.
 */
#define EPHY_TYPE_ENGINE_VIEW (ephy_engine_web_view_get_type())
G_DECLARE_DERIVABLE_TYPE(EphyEngineView, ephy_engine_web_view, EPHY, ENGINE_VIEW, GtkWidget)
typedef struct _EphyEngineHistory EphyEngineHistory;
typedef struct _EphyEngineHistoryItem EphyEngineHistoryItem;
typedef struct _EphyEngineFind EphyEngineFind;
typedef struct _EphyEngineInspector EphyEngineInspector;
typedef struct _EphyEngineResource EphyEngineResource;
typedef struct _EphyEngineSessionState EphyEngineSessionState;
/* Requested CEF window.open geometry. Presence flags distinguish omitted values;
 * GTK/Wayland do not promise absolute placement. */
typedef struct {
  GdkRectangle geometry;
  gboolean x_set, y_set, width_set, height_set;
  gboolean is_popup;
} EphyEnginePopupProperties;

GType ephy_engine_history_item_get_type(void);
#define EPHY_ENGINE_HISTORY_ITEM(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),ephy_engine_history_item_get_type(),EphyEngineHistoryItem))
struct _EphyEngineViewClass {
    GtkWidgetClass parent;

    /*< public >*/
    void       (* load_changed)                (EphyEngineView               *web_view,
                                                WebKitLoadEvent              load_event);
    gboolean   (* load_failed)                 (EphyEngineView               *web_view,
                                                WebKitLoadEvent              load_event,
                                                const gchar                 *failing_uri,
                                                GError                      *error);

    GtkWidget *(* create_popup)                (EphyEngineView               *web_view,
                                                const EphyEnginePopupProperties *properties);

    void       (* ready_to_show)               (EphyEngineView               *web_view);
    void       (* run_as_modal)                (EphyEngineView               *web_view);
    void       (* close)                       (EphyEngineView               *web_view);

    gboolean   (* permission_request)          (EphyEngineView               *web_view,
                                                EphyEnginePermission        *permission_request);

    gboolean   (* enter_fullscreen)            (EphyEngineView               *web_view);
    gboolean   (* leave_fullscreen)            (EphyEngineView               *web_view);

    gboolean   (* load_failed_with_tls_errors) (EphyEngineView               *web_view,
                                                const gchar                 *failing_uri,
                                                GTlsCertificate             *certificate,
                                                GTlsCertificateFlags         errors);

};
EphyEngineHistoryItem *
ephy_engine_back_forward_list_get_current_item            (EphyEngineHistory *back_forward_list);

EphyEngineHistoryItem *
ephy_engine_back_forward_list_get_back_item               (EphyEngineHistory *back_forward_list);

EphyEngineHistoryItem *
ephy_engine_back_forward_list_get_forward_item            (EphyEngineHistory *back_forward_list);

GList *
ephy_engine_back_forward_list_get_back_list_with_limit    (EphyEngineHistory *back_forward_list,
                                                      guint                  limit);

GList *
ephy_engine_back_forward_list_get_forward_list_with_limit (EphyEngineHistory *back_forward_list,
                                                      guint                  limit);

const gchar *
ephy_engine_back_forward_list_item_get_uri          (EphyEngineHistoryItem* list_item);

const gchar *
ephy_engine_back_forward_list_item_get_title        (EphyEngineHistoryItem* list_item);

const gchar *
ephy_engine_back_forward_list_item_get_original_uri (EphyEngineHistoryItem* list_item);

void
ephy_engine_find_controller_search              (EphyEngineFind *find_controller,
                                            const gchar          *search_text,
                                            guint32               find_options,
                                            guint                 max_match_count);

void
ephy_engine_find_controller_search_finish       (EphyEngineFind *find_controller);

void
ephy_engine_find_controller_search_next         (EphyEngineFind *find_controller);

void
ephy_engine_find_controller_search_previous     (EphyEngineFind *find_controller);

void
ephy_engine_find_controller_count_matches       (EphyEngineFind *find_controller,
                                            const gchar          *search_text,
                                            guint32               find_options,
                                            guint                 max_match_count);

guint32
ephy_engine_find_controller_get_options         (EphyEngineFind *find_controller);

void
ephy_engine_web_inspector_show                (EphyEngineInspector *inspector);

void
ephy_engine_web_inspector_close               (EphyEngineInspector *inspector);

const gchar *
ephy_engine_web_resource_get_uri         (EphyEngineResource  *resource);

EphyEngineResponse *
ephy_engine_web_resource_get_response    (EphyEngineResource  *resource);

void
ephy_engine_web_resource_get_data        (EphyEngineResource  *resource,
                                     GCancellable       *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer            user_data);

guchar *
ephy_engine_web_resource_get_data_finish (EphyEngineResource  *resource,
                                     GAsyncResult       *result,
                                     gsize              *length,
                                     GError            **error);

gboolean
ephy_engine_web_view_is_controlled_by_automation          (EphyEngineView             *web_view);

void
ephy_engine_web_view_try_close                            (EphyEngineView             *web_view);

void
ephy_engine_web_view_load_uri                             (EphyEngineView             *web_view,
                                                      const gchar               *uri);

/* Internal top-level documents only. The detailed page-message signal carries
 * a borrowed JsonNode; websites and subframes cannot send these messages. */
void ephy_engine_web_view_register_page_message_handler (EphyEngineView *view, const char *name);
void ephy_engine_web_view_unregister_page_message_handler (EphyEngineView *view, const char *name);
/* Native URI, not an alternate document's presentation alias. The first check
 * permits the current requested main navigation; the second requires its frame. */
gboolean ephy_engine_web_view_has_native_uri (EphyEngineView *view, const char *uri);
gboolean ephy_engine_web_view_is_current_page (EphyEngineView *view, const char *uri);
/* TRUE means queued to the captured document; stale contexts drop delivery. */
gboolean ephy_engine_web_view_send_page_message (EphyEngineView *view, const char *expected_uri,
                                                const char *name, JsonNode *message);


void
ephy_engine_web_view_load_alternate_html                  (EphyEngineView             *web_view,
                                                      const gchar               *content,
                                                      const gchar               *content_uri,
                                                      const gchar               *base_uri);



void
ephy_engine_web_view_stop_loading                         (EphyEngineView             *web_view);

gboolean
ephy_engine_web_view_is_loading                           (EphyEngineView             *web_view);

gboolean
ephy_engine_web_view_is_playing_audio                     (EphyEngineView             *web_view);

void
ephy_engine_web_view_set_is_muted                         (EphyEngineView             *web_view,
                                                      gboolean                   muted);

gboolean
ephy_engine_web_view_get_is_muted                         (EphyEngineView             *web_view);

guint64
ephy_engine_web_view_get_page_id                          (EphyEngineView             *web_view);

const gchar *
ephy_engine_web_view_get_title                            (EphyEngineView             *web_view);

void
ephy_engine_web_view_reload                               (EphyEngineView             *web_view);

void
ephy_engine_web_view_reload_bypass_cache                  (EphyEngineView             *web_view);

gdouble
ephy_engine_web_view_get_estimated_load_progress          (EphyEngineView             *web_view);

void
ephy_engine_web_view_go_back                              (EphyEngineView             *web_view);

gboolean
ephy_engine_web_view_can_go_back                          (EphyEngineView             *web_view);

void
ephy_engine_web_view_go_forward                           (EphyEngineView             *web_view);

gboolean
ephy_engine_web_view_can_go_forward                       (EphyEngineView             *web_view);

EphyEngineHistory *
ephy_engine_web_view_get_back_forward_list                (EphyEngineView             *web_view);

void
ephy_engine_web_view_go_to_back_forward_list_item         (EphyEngineView             *web_view,
                                                      EphyEngineHistoryItem *list_item);

const gchar *
ephy_engine_web_view_get_uri                              (EphyEngineView             *web_view);

GdkTexture *
ephy_engine_web_view_get_favicon                          (EphyEngineView             *web_view);

const gchar *
ephy_engine_web_view_get_custom_charset                   (EphyEngineView             *web_view);

void
ephy_engine_web_view_set_custom_charset                   (EphyEngineView             *web_view,
                                                      const gchar               *charset);

const EphyEnginePopupProperties *
ephy_engine_web_view_get_popup_properties                (EphyEngineView             *web_view);

void
ephy_engine_web_view_set_zoom_level                       (EphyEngineView             *web_view,
                                                      gdouble                    zoom_level);

gdouble
ephy_engine_web_view_get_zoom_level                       (EphyEngineView             *web_view);

void
ephy_engine_web_view_execute_editing_command              (EphyEngineView             *web_view,
                                                      const gchar               *command);

EphyEngineFind *
ephy_engine_web_view_get_find_controller                  (EphyEngineView             *web_view);

void
ephy_engine_web_view_evaluate_javascript                  (EphyEngineView             *web_view,
                                                      const char                *script,
                                                      gssize                     length,
                                                      const char                *world_name,
                                                      const char                *source_uri,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback        callback,
                                                      gpointer                   user_data);

/* Returns an owned Chromium Runtime.RemoteObject as JSON. Its type/subtype,
 * optional value, and unserializableValue preserve undefined, null, BigInt,
 * and non-finite numbers without creating another JavaScript engine.
 * Remote object handles are released; this result contains serialized data. */
JsonObject *
ephy_engine_web_view_evaluate_javascript_finish           (EphyEngineView             *web_view,
                                                      GAsyncResult              *result,
                                                      GError                   **error);

EphyEngineResource *
ephy_engine_web_view_get_main_resource                    (EphyEngineView             *web_view);

EphyEngineInspector *
ephy_engine_web_view_get_inspector                        (EphyEngineView             *web_view);

void
ephy_engine_web_view_save_to_file                         (EphyEngineView             *web_view,
                                                      GFile                     *file,
                                                      WebKitSaveMode             save_mode,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback        callback,
                                                      gpointer                   user_data);

gboolean
ephy_engine_web_view_get_tls_info                         (EphyEngineView             *web_view,
                                                      GTlsCertificate          **certificate,
                                                      GTlsCertificateFlags      *errors);
gboolean ephy_engine_web_view_tls_is_secure (EphyEngineView *web_view);

void
ephy_engine_web_view_get_snapshot                         (EphyEngineView             *web_view,
                                                      WebKitSnapshotRegion       region,
                                                      WebKitSnapshotOptions      options,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback        callback,
                                                      gpointer                   user_data);

GdkTexture *
ephy_engine_web_view_get_snapshot_finish                  (EphyEngineView             *web_view,
                                                      GAsyncResult              *result,
                                                      GError                   **error);

WebKitUserContentManager *
ephy_engine_web_view_get_user_content_manager             (EphyEngineView             *web_view);



EphyEngineSessionState *
ephy_engine_web_view_get_session_state                    (EphyEngineView             *web_view);

void
ephy_engine_web_view_restore_session_state                (EphyEngineView             *web_view,
                                                      EphyEngineSessionState *state);

void
ephy_engine_web_view_send_message_to_page                 (EphyEngineView             *web_view,
                                                      WebKitUserMessage         *message,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback        callback,
                                                      gpointer                   user_data);

WebKitUserMessage *
ephy_engine_web_view_send_message_to_page_finish          (EphyEngineView             *web_view,
                                                      GAsyncResult              *result,
                                                      GError                   **error);

void
ephy_engine_web_view_set_cors_allowlist                   (EphyEngineView             *web_view,
                                                      const gchar * const       *allowlist);

gboolean
ephy_engine_web_view_get_is_web_process_responsive        (EphyEngineView             *web_view);

void
ephy_engine_web_view_terminate_web_process                (EphyEngineView             *web_view);

EphyEngineSessionState *
ephy_engine_web_view_session_state_new       (GBytes *data);

EphyEngineSessionState *
ephy_engine_web_view_session_state_ref       (EphyEngineSessionState *state);

void
ephy_engine_web_view_session_state_unref     (EphyEngineSessionState *state);

GBytes *
ephy_engine_web_view_session_state_serialize (EphyEngineSessionState *state);
/* Startup configuration inventory, not a query of Chromium's live extension state. */
JsonArray *ephy_engine_extensions_dup_installed(void);
gint ephy_engine_web_view_get_extension_tab_id(EphyEngineView *view);
gboolean ephy_engine_web_view_set_extension_popup(EphyEngineView *popup, EphyEngineView *owner);
char *ephy_engine_dup_version_info(void);
void ephy_engine_web_view_set_user_agent_override(EphyEngineView *, const char *);
/* Recognized CEF subprocesses exit here without unwinding the caller's stack.
 * Browser execution returns -1; preparation failure returns an exit status. */
int ephy_engine_execute_process(int argc, char** argv);
gboolean ephy_engine_initialize(int argc, char** argv, const char* profile, gboolean self_test);
gboolean ephy_engine_prepare_shutdown(void);
void ephy_engine_shutdown(void);
void ephy_engine_web_view_print(EphyEngineView*, GtkPageSetup*, GtkPrintSettings*, GAsyncReadyCallback, gpointer);
GtkPrintSetup* ephy_engine_web_view_print_finish(EphyEngineView*, GAsyncResult*, GError**);
G_END_DECLS
