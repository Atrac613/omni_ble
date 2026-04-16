#include "include/omni_ble/omni_ble_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "omni_ble_plugin.h"

void OmniBlePluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  omni_ble::OmniBlePlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
