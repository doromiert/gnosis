/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2020 Jan-Michael Brummer <jan.brummer@tabos.org>
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

#pragma once
#include "ephy-engine-scheme.h"

#include "ephy-engine-view.h"

G_BEGIN_DECLS

#define EPHY_TYPE_READER_HANDLER (ephy_reader_handler_get_type ())

G_DECLARE_FINAL_TYPE (EphyReaderHandler, ephy_reader_handler, EPHY, READER_HANDLER, GObject)

#define EPHY_READER_SCHEME "ephy-reader"

EphyReaderHandler *ephy_reader_handler_new            (void);

void               ephy_reader_handler_handle_request (EphyReaderHandler  *handler,
                                                       EphyEngineSchemeRequest *request);
void               ephy_reader_handler_get_html       (EphyEngineView *view, GCancellable *cancellable,
                                                        GAsyncReadyCallback callback, gpointer user_data);
char              *ephy_reader_handler_get_html_finish (EphyEngineView *view, GAsyncResult *result, GError **error);
G_END_DECLS
