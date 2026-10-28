/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
gboolean gnosis_brave_profile_prepare(const char *directory, GError **error);

/* Own profile only: never change another browser's native host registration. */
gboolean gnosis_brave_profile_prepare_keepassxc(const char *directory, GError **error);

G_END_DECLS
