/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "brave/browser.hpp"
#include "brave/debug-adapters.h"
#include <glib.h>
#include <cstring>
#include <set>
int main(int argc,char** argv) {
  const std::string mode=argc>1?argv[1]:"defaults";
  if (mode=="defaults") {
    JupiterBrave::configure_debug_adapters("{}");
    std::set<std::string> ids;
    for (const auto& row:gnosis_brave_adapters) {
      g_assert_true(ids.insert(row.id).second);
      g_assert_true(JupiterBrave::adapter_enabled(row.id));
      g_assert_false(JupiterBrave::adapter_overridden(row.id));
    }
    g_assert_cmpstr(JupiterBrave::disabled_adapters().c_str(),==,"");
  } else if (mode=="overrides") {
    JupiterBrave::configure_debug_adapters(R"({"adapters":{"keepassxc":false,"permission-dialogs":false,"ublock":true,"unknown":false,"vim":"invalid"}})");
    g_assert_false(JupiterBrave::adapter_enabled("keepassxc"));
    g_assert_false(JupiterBrave::adapter_enabled("permission-dialogs"));
    g_assert_true(JupiterBrave::adapter_enabled("ublock"));
    g_assert_true(JupiterBrave::adapter_enabled("vim"));
    g_assert_false(JupiterBrave::adapter_overridden("unknown"));
    g_assert_cmpstr(JupiterBrave::disabled_adapters().c_str(),==,"keepassxc,permission-dialogs");
    JupiterBrave::configure_debug_adapters(R"({"adapters":{"keepassxc":true}})");
    g_assert_false(JupiterBrave::adapter_enabled("keepassxc")); // next launch, never half-applied
  } else if (mode=="invalid") {
    JupiterBrave::configure_debug_adapters(R"({"adapters":false})");
    g_assert_true(JupiterBrave::adapter_enabled("keepassxc"));
  } else return 2;
  return 0;
}
