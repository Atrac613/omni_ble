//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <omni_ble/omni_ble_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) omni_ble_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "OmniBlePlugin");
  omni_ble_plugin_register_with_registrar(omni_ble_registrar);
}
