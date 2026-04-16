#include <flutter/method_call.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <memory>
#include <string>
#include <variant>

#include "omni_ble_plugin.h"

namespace omni_ble {
namespace test {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResultFunctions;

}  // namespace

TEST(OmniBlePlugin, GetPlatformVersion) {
  OmniBlePlugin plugin;
  EncodableMap result_map;
  plugin.HandleMethodCall(
      MethodCall("getCapabilities", std::make_unique<EncodableValue>()),
      std::make_unique<MethodResultFunctions<>>(
          [&result_map](const EncodableValue* result) {
            result_map = std::get<EncodableMap>(*result);
          },
          nullptr, nullptr));

  EXPECT_EQ(std::get<std::string>(result_map[EncodableValue("platform")]),
            "windows");
  const auto& features =
      std::get<flutter::EncodableList>(result_map[EncodableValue("availableFeatures")]);
  EXPECT_EQ(features.size(), 2U);
  EXPECT_EQ(std::get<std::string>(features[0]), "central");
  EXPECT_EQ(std::get<std::string>(features[1]), "scanning");
}

TEST(OmniBlePlugin, ConnectRequiresDeviceId) {
  OmniBlePlugin plugin;
  std::string error_code;
  std::string error_message;
  plugin.HandleMethodCall(
      MethodCall("connect", std::make_unique<EncodableValue>(EncodableMap{})),
      std::make_unique<MethodResultFunctions<>>(
          nullptr,
          [&error_code, &error_message](const std::string& code,
                                        const std::string& message,
                                        const flutter::EncodableValue* details) {
            error_code = code;
            error_message = message;
          },
          nullptr));

  EXPECT_EQ(error_code, "invalid-argument");
  EXPECT_EQ(error_message, "`deviceId` is required to connect.");
}

TEST(OmniBlePlugin, DiscoverServicesRequiresConnection) {
  OmniBlePlugin plugin;
  std::string error_code;
  std::string error_message;
  EncodableMap arguments;
  arguments[EncodableValue("deviceId")] =
      EncodableValue("AA:BB:CC:DD:EE:FF");
  plugin.HandleMethodCall(
      MethodCall("discoverServices",
                 std::make_unique<EncodableValue>(arguments)),
      std::make_unique<MethodResultFunctions<>>(
          nullptr,
          [&error_code, &error_message](const std::string& code,
                                        const std::string& message,
                                        const flutter::EncodableValue* details) {
            error_code = code;
            error_message = message;
          },
          nullptr));

  EXPECT_EQ(error_code, "not-connected");
  EXPECT_EQ(error_message,
            "Bluetooth device must be connected before discovering services.");
}

}  // namespace test
}  // namespace omni_ble
