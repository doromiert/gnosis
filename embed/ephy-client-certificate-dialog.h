/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "ephy-engine-client-certificate.h"

#include <adwaita.h>

G_BEGIN_DECLS

AdwAlertDialog *ephy_client_certificate_dialog_new          (EphyEngineClientCertificate *request);
gboolean        ephy_client_certificate_dialog_get_selected (AdwAlertDialog              *dialog,
                                                             guint                       *index);

G_END_DECLS
