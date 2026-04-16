#ifndef FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_
#define FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace omni_ble {

class OmniBlePlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  OmniBlePlugin();

  virtual ~OmniBlePlugin();

  // Disallow copy and assign.
  OmniBlePlugin(const OmniBlePlugin&) = delete;
  OmniBlePlugin& operator=(const OmniBlePlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

}  // namespace omni_ble

#endif  // FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_
