#include "omni_ble_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <sstream>

namespace omni_ble {

namespace {

flutter::EncodableMap PermissionStatusPayload(
    const flutter::EncodableValue* arguments) {
  flutter::EncodableMap permissions;

  if (arguments != nullptr &&
      std::holds_alternative<flutter::EncodableMap>(*arguments)) {
    const auto& argument_map = std::get<flutter::EncodableMap>(*arguments);
    const auto permissions_it =
        argument_map.find(flutter::EncodableValue("permissions"));
    if (permissions_it != argument_map.end() &&
        std::holds_alternative<flutter::EncodableList>(permissions_it->second)) {
      const auto& requested_permissions =
          std::get<flutter::EncodableList>(permissions_it->second);
      for (const auto& permission : requested_permissions) {
        if (const auto* permission_name =
                std::get_if<std::string>(&permission)) {
          permissions[flutter::EncodableValue(*permission_name)] =
              flutter::EncodableValue("notRequired");
        }
      }
    }
  }

  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("permissions")] =
      flutter::EncodableValue(permissions);
  payload[flutter::EncodableValue("allGranted")] = flutter::EncodableValue(true);
  return payload;
}

}  // namespace

// static
void OmniBlePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "omni_ble/methods",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<OmniBlePlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

OmniBlePlugin::OmniBlePlugin() {}

OmniBlePlugin::~OmniBlePlugin() {}

void OmniBlePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("getCapabilities") == 0) {
    std::ostringstream version_stream;
    version_stream << "Windows ";
    if (IsWindows10OrGreater()) {
      version_stream << "10+";
    } else if (IsWindows8OrGreater()) {
      version_stream << "8";
    } else if (IsWindows7OrGreater()) {
      version_stream << "7";
    }
    flutter::EncodableMap payload;
    payload[flutter::EncodableValue("platform")] =
        flutter::EncodableValue("windows");
    payload[flutter::EncodableValue("platformVersion")] =
        flutter::EncodableValue(version_stream.str());
    payload[flutter::EncodableValue("availableFeatures")] =
        flutter::EncodableValue(flutter::EncodableList{});
    result->Success(flutter::EncodableValue(payload));
  } else if (method_call.method_name().compare("checkPermissions") == 0 ||
             method_call.method_name().compare("requestPermissions") == 0) {
    result->Success(
        flutter::EncodableValue(PermissionStatusPayload(method_call.arguments())));
  } else {
    result->NotImplemented();
  }
}

}  // namespace omni_ble
