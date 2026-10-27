/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "loopback.h"
int main(void) {
  const char *local[] = {"http://localhost:5173", "https://LOCALHOST./", "http://dev.localhost:8080", "http://127.0.0.2/", "https://[::1]:5173/path"};
  const char *remote[] = {"https://localhost.example/", "https://notlocalhost/", "https://192.168.1.1/", "https://example.com/?host=localhost", "file:///tmp/localhost", "https://localhost@evil.example/"};
  for (guint i = 0; i < G_N_ELEMENTS(local); ++i) g_assert_true(gnosis_uri_is_loopback(local[i]));
  for (guint i = 0; i < G_N_ELEMENTS(remote); ++i) g_assert_false(gnosis_uri_is_loopback(remote[i]));
  g_print("LOOPBACK_PASS local hostnames and addresses; remote lookalikes stay encrypted\n");
}
