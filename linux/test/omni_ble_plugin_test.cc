#include <flutter_linux/flutter_linux.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "include/omni_ble/omni_ble_plugin.h"
#include "omni_ble_plugin_private.h"

// This demonstrates a simple unit test of the C portion of this plugin's
// implementation.
//
// Once you have built the plugin's example app, you can run these tests
// from the command line. For instance, for a plugin called my_plugin
// built for x64 debug, run:
// $ build/linux/x64/debug/plugins/my_plugin/my_plugin_test

namespace omni_ble {
namespace test {

TEST(OmniBlePlugin, GetCapabilities) {
  g_autoptr(FlMethodResponse) response = get_capabilities();
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(FL_IS_METHOD_SUCCESS_RESPONSE(response));
  FlValue* result = fl_method_success_response_get_result(
      FL_METHOD_SUCCESS_RESPONSE(response));
  ASSERT_EQ(fl_value_get_type(result), FL_VALUE_TYPE_MAP);
  EXPECT_STREQ(fl_value_get_string(fl_value_lookup_string(result, "platform")),
               "linux");
}

}  // namespace test
}  // namespace omni_ble
