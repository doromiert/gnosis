#pragma once
#include "include/cef_command_line.h"
#include "include/cef_request_context.h"
inline void configure_extensions(CefRefPtr<CefCommandLine> command, const std::string& paths) {
  command->AppendSwitchWithValue("load-extension", paths);
  command->AppendSwitch("allow-legacy-extension-manifests");
  command->AppendSwitch("enable-unsafe-extension-debugging");
  command->AppendSwitchWithValue("enable-features", "AllowLegacyMV2Extensions");
  command->AppendSwitchWithValue(
      "disable-features",
      "ExtensionManifestV2Disabled,ExtensionManifestV2Unsupported,ExtensionManifestV2DeprecationWarning");
}
inline bool enable_extension_developer_mode() {
  auto value = CefValue::Create();
  value->SetBool(true);
  CefString error;
  return CefRequestContext::GetGlobalContext()->SetPreference("extensions.ui.developer_mode", value, error);
}
