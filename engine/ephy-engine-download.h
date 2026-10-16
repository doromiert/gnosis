/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_DOWNLOAD (ephy_engine_download_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineDownload, ephy_engine_download, EPHY, ENGINE_DOWNLOAD, GObject)
typedef struct _EphyEngineSession EphyEngineSession;
EphyEngineDownload *ephy_engine_session_download_uri(EphyEngineSession *, const char *uri);
const char *ephy_engine_download_get_uri(EphyEngineDownload *);
const char *ephy_engine_download_get_response_uri(EphyEngineDownload *);
const char *ephy_engine_download_get_mime_type(EphyEngineDownload *);
guint64 ephy_engine_download_get_content_length(EphyEngineDownload *);
guint64 ephy_engine_download_get_received_data_length(EphyEngineDownload *);
double ephy_engine_download_get_estimated_progress(EphyEngineDownload *);
double ephy_engine_download_get_elapsed_time(EphyEngineDownload *);
const char *ephy_engine_download_get_destination(EphyEngineDownload *);
void ephy_engine_download_set_destination(EphyEngineDownload *, const char *path);
gboolean ephy_engine_download_get_allow_overwrite(EphyEngineDownload *);
void ephy_engine_download_set_allow_overwrite(EphyEngineDownload *, gboolean);
gboolean ephy_engine_download_destination_exists(EphyEngineDownload *, const char *path);
void ephy_engine_download_cancel(EphyEngineDownload *);
G_END_DECLS

#ifdef __cplusplus
#include "include/cef_download_handler.h"
void ephy_engine_download_before(EphyEngineSession *, CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>,
                                const CefString&, CefRefPtr<CefBeforeDownloadCallback>, EphyEngineDownload *existing = nullptr);
void ephy_engine_download_updated(EphyEngineSession *, CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>,
                                 CefRefPtr<CefDownloadItemCallback>, EphyEngineDownload *existing = nullptr);
void ephy_engine_downloads_shutdown();
void ephy_engine_download_browser_closed(int browser_id);
#endif
