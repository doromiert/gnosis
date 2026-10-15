/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
G_BEGIN_DECLS
typedef struct _EphyEngineOrigin EphyEngineOrigin;
/* Chromium URL canonicalization requires the CEF runtime to be initialized. */
EphyEngineOrigin *ephy_engine_origin_new_for_uri(const char *uri);
EphyEngineOrigin *ephy_engine_origin_new(const char *scheme, const char *host, guint16 port);
EphyEngineOrigin *ephy_engine_origin_ref(EphyEngineOrigin *);
void ephy_engine_origin_unref(EphyEngineOrigin *);
const char *ephy_engine_origin_get_protocol(EphyEngineOrigin *);
const char *ephy_engine_origin_get_host(EphyEngineOrigin *);
guint16 ephy_engine_origin_get_port(EphyEngineOrigin *);
char *ephy_engine_origin_to_string(EphyEngineOrigin *);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EphyEngineOrigin, ephy_engine_origin_unref)
G_END_DECLS
