#include "include/omni_ble/omni_ble_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <cstring>

#include "omni_ble_plugin_private.h"

#define OMNI_BLE_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), omni_ble_plugin_get_type(), \
                              OmniBlePlugin))

struct _OmniBlePlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(OmniBlePlugin, omni_ble_plugin, g_object_get_type())

static FlMethodResponse* permission_status_response(FlValue* arguments) {
  g_autoptr(FlValue) permissions = fl_value_new_map();
  FlValue* requested_permissions = nullptr;

  if (arguments != nullptr && fl_value_get_type(arguments) == FL_VALUE_TYPE_MAP) {
    requested_permissions = fl_value_lookup_string(arguments, "permissions");
  }

  if (requested_permissions != nullptr &&
      fl_value_get_type(requested_permissions) == FL_VALUE_TYPE_LIST) {
    const size_t permission_count = fl_value_get_length(requested_permissions);
    for (size_t index = 0; index < permission_count; index++) {
      FlValue* permission = fl_value_get_list_value(requested_permissions, index);
      if (fl_value_get_type(permission) != FL_VALUE_TYPE_STRING) {
        continue;
      }
      fl_value_set_string_take(permissions, fl_value_get_string(permission),
                               fl_value_new_string("notRequired"));
    }
  }

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "permissions", fl_value_ref(permissions));
  fl_value_set_string_take(result, "allGranted", fl_value_new_bool(TRUE));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

// Called when a method call is received from Flutter.
static void omni_ble_plugin_handle_method_call(
    OmniBlePlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getCapabilities") == 0) {
    response = get_capabilities();
  } else if (strcmp(method, "checkPermissions") == 0 ||
             strcmp(method, "requestPermissions") == 0) {
    response = permission_status_response(fl_method_call_get_args(method_call));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_capabilities() {
  struct utsname uname_data = {};
  uname(&uname_data);

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "platform", fl_value_new_string("linux"));
  fl_value_set_string_take(result, "platformVersion",
                           fl_value_new_string(uname_data.version));
  fl_value_set_string_take(result, "availableFeatures", fl_value_new_list());
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void omni_ble_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(omni_ble_plugin_parent_class)->dispose(object);
}

static void omni_ble_plugin_class_init(OmniBlePluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = omni_ble_plugin_dispose;
}

static void omni_ble_plugin_init(OmniBlePlugin* self) {}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  OmniBlePlugin* plugin = OMNI_BLE_PLUGIN(user_data);
  omni_ble_plugin_handle_method_call(plugin, method_call);
}

void omni_ble_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  OmniBlePlugin* plugin = OMNI_BLE_PLUGIN(
      g_object_new(omni_ble_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "omni_ble/methods",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
