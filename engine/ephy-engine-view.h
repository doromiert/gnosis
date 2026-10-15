/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <webkit/webkit.h>
#include "ephy-engine-favicon.h"
G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_VIEW (ephy_engine_web_view_get_type())
G_DECLARE_DERIVABLE_TYPE(EphyEngineView, ephy_engine_web_view, EPHY, ENGINE_VIEW, GtkWidget)
typedef struct _EphyEngineHistory EphyEngineHistory;
typedef struct _EphyEngineHistoryItem EphyEngineHistoryItem;
typedef struct _EphyEngineFind EphyEngineFind;
typedef struct _EphyEngineInspector EphyEngineInspector;
typedef struct _EphyEngineResource EphyEngineResource;
typedef struct _EphyEngineSessionState EphyEngineSessionState;
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

    GtkWidget *(* create)                      (EphyEngineView               *web_view,
                                                WebKitNavigationAction      *navigation_action);

    void       (* ready_to_show)               (EphyEngineView               *web_view);
    void       (* run_as_modal)                (EphyEngineView               *web_view);
    void       (* close)                       (EphyEngineView               *web_view);

    gboolean   (* script_dialog)               (EphyEngineView               *web_view,
                                                WebKitScriptDialog          *dialog)  ;

    gboolean   (* decide_policy)               (EphyEngineView               *web_view,
                                                WebKitPolicyDecision        *decision,
                                                WebKitPolicyDecisionType     type);
    gboolean   (* permission_request)          (EphyEngineView               *web_view,
                                                WebKitPermissionRequest     *permission_request);
    void       (* mouse_target_changed)        (EphyEngineView               *web_view,
                                                WebKitHitTestResult         *hit_test_result,
                                                guint                        modifiers);

    gboolean   (* print)                       (EphyEngineView               *web_view,
                                                WebKitPrintOperation        *print_operation);

    void       (* resource_load_started)       (EphyEngineView               *web_view,
                                                EphyEngineResource           *resource,
                                                WebKitURIRequest            *request);
    gboolean   (* enter_fullscreen)            (EphyEngineView               *web_view);
    gboolean   (* leave_fullscreen)            (EphyEngineView               *web_view);
    gboolean   (* run_file_chooser)            (EphyEngineView               *web_view,
                                                WebKitFileChooserRequest    *request);

    gboolean   (* context_menu)                (EphyEngineView               *web_view,
                                                WebKitContextMenu           *context_menu,
                                                WebKitHitTestResult         *hit_test_result);

    void       (* context_menu_dismissed)      (EphyEngineView               *web_view);
    void       (* submit_form)                 (EphyEngineView               *web_view,
                                                WebKitFormSubmissionRequest *request);
    void       (* insecure_content_detected)   (EphyEngineView               *web_view,
                                                WebKitInsecureContentEvent   event);
    gboolean   (* web_process_crashed)         (EphyEngineView               *web_view);

    gboolean   (* authenticate)                (EphyEngineView               *web_view,
                                                WebKitAuthenticationRequest *request);
    gboolean   (* load_failed_with_tls_errors) (EphyEngineView               *web_view,
                                                const gchar                 *failing_uri,
                                                GTlsCertificate             *certificate,
                                                GTlsCertificateFlags         errors);
    gboolean   (* show_notification)           (EphyEngineView               *web_view,
                                                WebKitNotification          *notification);

    gboolean   (* run_color_chooser)           (EphyEngineView               *web_view,
                                                WebKitColorChooserRequest   *request);
    gboolean   (* show_option_menu)            (EphyEngineView               *web_view,
                                                WebKitOptionMenu            *menu,
                                                GdkRectangle                *rectangle);

    void       (* web_process_terminated)      (EphyEngineView               *web_view,
                                                WebKitWebProcessTerminationReason reason);
    gboolean   (* user_message_received)       (EphyEngineView               *web_view,
                                                WebKitUserMessage           *message);


    gboolean   (* query_permission_state)      (EphyEngineView               *web_view,
                                                WebKitPermissionStateQuery  *query);


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

WebKitURIResponse *
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

WebKitWebContext *
ephy_engine_web_view_get_context                          (EphyEngineView             *web_view);

void
ephy_engine_web_view_try_close                            (EphyEngineView             *web_view);

void
ephy_engine_web_view_load_uri                             (EphyEngineView             *web_view,
                                                      const gchar               *uri);

void
ephy_engine_web_view_load_alternate_html                  (EphyEngineView             *web_view,
                                                      const gchar               *content,
                                                      const gchar               *content_uri,
                                                      const gchar               *base_uri);

void
ephy_engine_web_view_load_request                         (EphyEngineView             *web_view,
                                                      WebKitURIRequest          *request);

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

WebKitWindowProperties *
ephy_engine_web_view_get_window_properties                (EphyEngineView             *web_view);

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

JSCValue *
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

WebKitEditorState *
ephy_engine_web_view_get_editor_state                     (EphyEngineView             *web_view);

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
int ephy_engine_execute_process(int argc, char** argv);
gboolean ephy_engine_initialize(int argc, char** argv, const char* profile, gboolean self_test);
void ephy_engine_shutdown(void);
void ephy_engine_web_view_print(EphyEngineView*, GtkPageSetup*, GtkPrintSettings*, GAsyncReadyCallback, gpointer);
GtkPrintSetup* ephy_engine_web_view_print_finish(EphyEngineView*, GAsyncResult*, GError**);
G_END_DECLS
