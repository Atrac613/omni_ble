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
}

}  // namespace test
}  // namespace omni_ble
