/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ephy-engine-session.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
JsonArray *ephy_engine_extensions_dup_native_snapshot(void);
void ephy_engine_extensions_query_async(EphyEngineSession *, GCancellable *, GAsyncReadyCallback, gpointer);
JsonArray *ephy_engine_extensions_query_finish(EphyEngineSession *, GAsyncResult *, GError **);
void ephy_engine_extensions_set_enabled_async(EphyEngineSession *, const char *id, gboolean enabled,
                                             GCancellable *, GAsyncReadyCallback, gpointer);
JsonArray *ephy_engine_extensions_set_enabled_finish(EphyEngineSession *, GAsyncResult *, GError **);
/* Copies an unpacked local directory into the profile store. Native loading
 * happens on restart; the result explicitly reports loaded=FALSE. */
void ephy_engine_extensions_install_unpacked_async(EphyEngineSession *, GFile *directory,
                                                  GCancellable *, GAsyncReadyCallback, gpointer);
JsonObject *ephy_engine_extensions_install_unpacked_finish(EphyEngineSession *, GAsyncResult *, GError **);
G_END_DECLS

#ifdef __cplusplus
#include <functional>
class Extensions;
/* UI-thread service, normal profile only. Catalog and context are retained. */
void ephy_engine_extensions_initialize(JsonArray *owned, CefRefPtr<CefRequestContext>,
                                       std::function<void(JsonArray *)> changed, Extensions *store);
void ephy_engine_extensions_initialize_brave(Extensions *store);
void ephy_engine_extensions_close();
bool ephy_engine_extensions_is_closed();
void ephy_engine_extensions_shutdown();
#endif
