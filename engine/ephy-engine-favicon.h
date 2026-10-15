/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gdk/gdk.h>

G_BEGIN_DECLS
#define EPHY_TYPE_ENGINE_FAVICON_DATABASE (ephy_engine_favicon_database_get_type())
G_DECLARE_FINAL_TYPE(EphyEngineFaviconDatabase, ephy_engine_favicon_database, EPHY, ENGINE_FAVICON_DATABASE, GObject)
EphyEngineFaviconDatabase *ephy_engine_favicon_database_new(const char *directory);
EphyEngineFaviconDatabase *ephy_engine_favicon_database_get_default(void);
void ephy_engine_favicon_database_set_directory(EphyEngineFaviconDatabase *, const char *directory);
void ephy_engine_favicon_database_store(EphyEngineFaviconDatabase *, const char *page_uri, const char *icon_uri, GdkTexture *);
void ephy_engine_favicon_database_get_favicon(EphyEngineFaviconDatabase *, const char *page_uri, GCancellable *, GAsyncReadyCallback, gpointer);
GdkTexture *ephy_engine_favicon_database_get_favicon_finish(EphyEngineFaviconDatabase *, GAsyncResult *, GError **);
char *ephy_engine_favicon_database_get_favicon_uri(EphyEngineFaviconDatabase *, const char *page_uri);
G_END_DECLS
