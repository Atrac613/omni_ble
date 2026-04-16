#include "include/omni_ble/omni_ble_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "omni_ble_plugin_private.h"

#define OMNI_BLE_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), omni_ble_plugin_get_type(), \
                              OmniBlePlugin))

namespace {

constexpr char kBluezBusName[] = "org.bluez";
constexpr char kBluezRootPath[] = "/";
constexpr char kAdapterInterface[] = "org.bluez.Adapter1";
constexpr char kDeviceInterface[] = "org.bluez.Device1";
constexpr char kGattServiceInterface[] = "org.bluez.GattService1";
constexpr char kGattCharacteristicInterface[] = "org.bluez.GattCharacteristic1";
constexpr char kGattDescriptorInterface[] = "org.bluez.GattDescriptor1";

std::string normalize_uuid(const gchar* value) {
  if (value == nullptr) {
    return {};
  }

  std::string uuid(value);
  std::transform(uuid.begin(), uuid.end(), uuid.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  if (uuid.size() == 4) {
    return "0000" + uuid + "-0000-1000-8000-00805f9b34fb";
  }
  if (uuid.size() == 8) {
    return uuid + "-0000-1000-8000-00805f9b34fb";
  }
  return uuid;
}

}  // namespace

struct _OmniBlePlugin {
  GObject parent_instance;
  FlEventChannel* event_channel;
  gboolean event_listening;
  gboolean is_scanning;
  gboolean allow_duplicates;
  GDBusObjectManager* object_manager;
  GHashTable* seen_devices;
  GPtrArray* service_filters;
  gulong object_added_handler_id;
  gulong properties_changed_handler_id;
};

G_DEFINE_TYPE(OmniBlePlugin, omni_ble_plugin, g_object_get_type())

static FlMethodResponse* success_response(FlValue* value = nullptr) {
  return FL_METHOD_RESPONSE(fl_method_success_response_new(value));
}

static FlMethodResponse* error_response(const gchar* code,
                                        const gchar* message,
                                        FlValue* details = nullptr) {
  return FL_METHOD_RESPONSE(
      fl_method_error_response_new(code, message, details));
}

static void clear_service_filters(OmniBlePlugin* self) {
  if (self->service_filters == nullptr) {
    self->service_filters = g_ptr_array_new_with_free_func(g_free);
    return;
  }

  while (self->service_filters->len > 0) {
    g_ptr_array_remove_index(self->service_filters,
                             self->service_filters->len - 1);
  }
}

static void clear_seen_devices(OmniBlePlugin* self) {
  if (self->seen_devices == nullptr) {
    self->seen_devices =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    return;
  }

  g_hash_table_remove_all(self->seen_devices);
}

static gboolean send_event(OmniBlePlugin* self, FlValue* event) {
  if (!self->event_listening || self->event_channel == nullptr) {
    return FALSE;
  }

  g_autoptr(GError) error = nullptr;
  if (!fl_event_channel_send(self->event_channel, event, nullptr, &error)) {
    g_warning("Failed to send omni_ble event: %s", error->message);
    return FALSE;
  }

  return TRUE;
}

static void emit_adapter_state(OmniBlePlugin* self, const gchar* state) {
  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type",
                           fl_value_new_string("adapterStateChanged"));
  fl_value_set_string_take(event, "state", fl_value_new_string(state));
  send_event(self, event);
}

static void emit_connection_state(OmniBlePlugin* self,
                                  const gchar* device_id,
                                  const gchar* state) {
  if (device_id == nullptr || strlen(device_id) == 0) {
    return;
  }

  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type",
                           fl_value_new_string("connectionStateChanged"));
  fl_value_set_string_take(event, "deviceId", fl_value_new_string(device_id));
  fl_value_set_string_take(event, "state", fl_value_new_string(state));
  send_event(self, event);
}

static GVariant* proxy_property(GDBusProxy* proxy, const gchar* property_name) {
  if (proxy == nullptr) {
    return nullptr;
  }

  return g_dbus_proxy_get_cached_property(proxy, property_name);
}

static gboolean proxy_property_bool(GDBusProxy* proxy,
                                    const gchar* property_name,
                                    gboolean default_value) {
  g_autoptr(GVariant) value = proxy_property(proxy, property_name);
  if (value == nullptr) {
    return default_value;
  }

  return g_variant_get_boolean(value);
}

static gint16 proxy_property_int16(GDBusProxy* proxy,
                                   const gchar* property_name,
                                   gint16 default_value) {
  g_autoptr(GVariant) value = proxy_property(proxy, property_name);
  if (value == nullptr) {
    return default_value;
  }

  return g_variant_get_int16(value);
}

static gchar* proxy_property_string_dup(GDBusProxy* proxy,
                                        const gchar* property_name) {
  g_autoptr(GVariant) value = proxy_property(proxy, property_name);
  if (value == nullptr) {
    return nullptr;
  }

  return g_variant_dup_string(value, nullptr);
}

static gboolean proxy_property_has(GDBusProxy* proxy, const gchar* property_name) {
  g_autoptr(GVariant) value = proxy_property(proxy, property_name);
  return value != nullptr;
}

static FlValue* bytes_value_from_variant(GVariant* value) {
  g_autoptr(FlValue) bytes = fl_value_new_list();
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE("ay"))) {
    return fl_value_ref(bytes);
  }

  gsize length = 0;
  const guint8* data =
      static_cast<const guint8*>(g_variant_get_fixed_array(value, &length,
                                                           sizeof(guint8)));
  for (gsize index = 0; index < length; index++) {
    fl_value_append_take(bytes, fl_value_new_int(data[index]));
  }
  return fl_value_ref(bytes);
}

static FlValue* bytes_property_from_proxy(GDBusProxy* proxy,
                                          const gchar* property_name) {
  g_autoptr(GVariant) value = proxy_property(proxy, property_name);
  return bytes_value_from_variant(value);
}

static gboolean changed_properties_contains(GVariant* changed_properties,
                                            const gchar* property_name) {
  if (changed_properties == nullptr ||
      !g_variant_is_of_type(changed_properties, G_VARIANT_TYPE("a{sv}"))) {
    return FALSE;
  }

  g_autoptr(GVariant) value =
      g_variant_lookup_value(changed_properties, property_name, nullptr);
  return value != nullptr;
}

static gboolean object_path_is_child_of(const gchar* object_path,
                                        const gchar* parent_path) {
  if (object_path == nullptr || parent_path == nullptr ||
      !g_str_has_prefix(object_path, parent_path)) {
    return FALSE;
  }
  const gsize parent_length = strlen(parent_path);
  return object_path[parent_length] == '/' || object_path[parent_length] == '\0';
}

static FlValue* service_uuids_from_proxy(GDBusProxy* proxy) {
  g_autoptr(FlValue) uuids = fl_value_new_list();
  g_autoptr(GVariant) value = proxy_property(proxy, "UUIDs");
  if (value == nullptr) {
    return fl_value_ref(uuids);
  }

  GVariantIter iter;
  const gchar* uuid = nullptr;
  g_variant_iter_init(&iter, value);
  while (g_variant_iter_next(&iter, "&s", &uuid)) {
    fl_value_append_take(uuids,
                         fl_value_new_string(normalize_uuid(uuid).c_str()));
  }

  return fl_value_ref(uuids);
}

static gboolean device_matches_filters(OmniBlePlugin* self, GDBusProxy* proxy) {
  if (self->service_filters == nullptr || self->service_filters->len == 0) {
    return TRUE;
  }

  g_autoptr(GVariant) value = proxy_property(proxy, "UUIDs");
  if (value == nullptr) {
    return FALSE;
  }

  GVariantIter iter;
  const gchar* uuid = nullptr;
  g_variant_iter_init(&iter, value);
  while (g_variant_iter_next(&iter, "&s", &uuid)) {
    const std::string normalized = normalize_uuid(uuid);
    for (guint index = 0; index < self->service_filters->len; index++) {
      const gchar* filter = static_cast<const gchar*>(
          g_ptr_array_index(self->service_filters, index));
      if (normalized == filter) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

static FlValue* service_data_from_proxy(GDBusProxy* proxy) {
  g_autoptr(FlValue) service_data = fl_value_new_map();
  g_autoptr(GVariant) value = proxy_property(proxy, "ServiceData");
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE("a{sv}"))) {
    return fl_value_ref(service_data);
  }

  GVariantIter iter;
  const gchar* uuid = nullptr;
  GVariant* bytes_variant = nullptr;
  g_variant_iter_init(&iter, value);
  while (g_variant_iter_next(&iter, "{&sv}", &uuid, &bytes_variant)) {
    g_autoptr(GVariant) owned_variant = bytes_variant;
    fl_value_set_string_take(service_data, normalize_uuid(uuid).c_str(),
                             bytes_value_from_variant(owned_variant));
  }

  return fl_value_ref(service_data);
}

static FlValue* manufacturer_data_from_proxy(GDBusProxy* proxy) {
  g_autoptr(GVariant) value = proxy_property(proxy, "ManufacturerData");
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE("a{qv}"))) {
    return nullptr;
  }

  GVariantIter iter;
  guint16 manufacturer_id = 0;
  GVariant* bytes_variant = nullptr;
  g_variant_iter_init(&iter, value);
  if (!g_variant_iter_next(&iter, "{qv}", &manufacturer_id, &bytes_variant)) {
    return nullptr;
  }

  g_autoptr(GVariant) owned_variant = bytes_variant;
  g_autoptr(GVariant) raw_bytes = g_variant_get_variant(owned_variant);
  g_autoptr(FlValue) bytes = fl_value_new_list();
  fl_value_append_take(bytes, fl_value_new_int(manufacturer_id & 0xFF));
  fl_value_append_take(bytes, fl_value_new_int((manufacturer_id >> 8) & 0xFF));

  if (raw_bytes != nullptr &&
      g_variant_is_of_type(raw_bytes, G_VARIANT_TYPE("ay"))) {
    gsize length = 0;
    const guint8* data = static_cast<const guint8*>(
        g_variant_get_fixed_array(raw_bytes, &length, sizeof(guint8)));
    for (gsize index = 0; index < length; index++) {
      fl_value_append_take(bytes, fl_value_new_int(data[index]));
    }
  }

  return fl_value_ref(bytes);
}

static FlValue* build_scan_result_from_proxy(GDBusProxy* proxy) {
  g_autoptr(FlValue) result = fl_value_new_map();
  g_autofree gchar* address = proxy_property_string_dup(proxy, "Address");
  if (address == nullptr || strlen(address) == 0) {
    return fl_value_ref(result);
  }

  fl_value_set_string_take(result, "deviceId", fl_value_new_string(address));
  {
    g_autofree gchar* name = proxy_property_string_dup(proxy, "Alias");
    if (name == nullptr || strlen(name) == 0) {
      g_clear_pointer(&name, g_free);
      name = proxy_property_string_dup(proxy, "Name");
    }
    if (name != nullptr && strlen(name) > 0) {
      fl_value_set_string_take(result, "name", fl_value_new_string(name));
    }
  }
  fl_value_set_string_take(result, "rssi",
                           fl_value_new_int(proxy_property_int16(proxy, "RSSI", 0)));
  fl_value_set_string_take(result, "serviceUuids", service_uuids_from_proxy(proxy));
  fl_value_set_string_take(result, "serviceData", service_data_from_proxy(proxy));
  if (FlValue* manufacturer_data = manufacturer_data_from_proxy(proxy);
      manufacturer_data != nullptr) {
    fl_value_set_string_take(result, "manufacturerData", manufacturer_data);
  }
  fl_value_set_string_take(result, "connectable", fl_value_new_bool(TRUE));

  return fl_value_ref(result);
}

static gboolean ensure_object_manager(OmniBlePlugin* self, GError** error) {
  if (self->object_manager != nullptr) {
    return TRUE;
  }

  self->object_manager = g_dbus_object_manager_client_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, kBluezBusName,
      kBluezRootPath, nullptr, nullptr, nullptr, nullptr, error);
  if (self->object_manager == nullptr) {
    return FALSE;
  }

  self->object_added_handler_id =
      g_signal_connect(self->object_manager, "object-added",
                       G_CALLBACK(+[](GDBusObjectManager* manager,
                                      GDBusObject* object,
                                      gpointer user_data) {
                         auto* plugin =
                             static_cast<OmniBlePlugin*>(user_data);
                         if (!plugin->is_scanning) {
                           return;
                         }

                         g_autoptr(GDBusInterface) interface_ =
                             g_dbus_object_get_interface(object,
                                                         kDeviceInterface);
                         if (interface_ == nullptr) {
                           return;
                         }

                         GDBusProxy* proxy = G_DBUS_PROXY(interface_);
                         if (!device_matches_filters(plugin, proxy)) {
                           return;
                         }

                         gchar* address = proxy_property_string_dup(proxy, "Address");
                         if (address == nullptr) {
                           return;
                         }

                         const gboolean seen = g_hash_table_contains(
                             plugin->seen_devices, address);
                         if (!plugin->allow_duplicates && seen) {
                           g_free(address);
                           return;
                         }

                         if (!seen) {
                           g_hash_table_add(plugin->seen_devices, address);
                         } else {
                           g_free(address);
                         }

                         g_autoptr(FlValue) event = fl_value_new_map();
                         fl_value_set_string_take(event, "type",
                                                  fl_value_new_string(
                                                      "scanResult"));
                         fl_value_set_string_take(event, "result",
                                                  build_scan_result_from_proxy(
                                                      proxy));
                         send_event(plugin, event);
                       }),
                       self);

  self->properties_changed_handler_id = g_signal_connect(
      self->object_manager, "interface-proxy-properties-changed",
      G_CALLBACK(+[](GDBusObjectManagerClient* manager,
                     GDBusObjectProxy* object_proxy,
                     GDBusProxy* interface_proxy,
                     GVariant* changed_properties,
                     const gchar* const* invalidated_properties,
                     gpointer user_data) {
        auto* plugin = static_cast<OmniBlePlugin*>(user_data);
        const gchar* interface_name =
            g_dbus_proxy_get_interface_name(interface_proxy);
        if (g_strcmp0(interface_name, kAdapterInterface) == 0) {
          emit_adapter_state(
              plugin,
              proxy_property_bool(interface_proxy, "Powered", FALSE)
                  ? "poweredOn"
                  : "poweredOff");
          return;
        }

        if (g_strcmp0(interface_name, kDeviceInterface) == 0) {
          g_autofree gchar* address =
              proxy_property_string_dup(interface_proxy, "Address");
          if (changed_properties_contains(changed_properties, "Connected") &&
              address != nullptr) {
            emit_connection_state(
                plugin, address,
                proxy_property_bool(interface_proxy, "Connected", FALSE)
                    ? "connected"
                    : "disconnected");
          }

          if (!plugin->is_scanning) {
            return;
          }
          if (!device_matches_filters(plugin, interface_proxy)) {
            return;
          }
          if (address == nullptr) {
            return;
          }

          const gboolean seen =
              g_hash_table_contains(plugin->seen_devices, address);
          if (!plugin->allow_duplicates && seen) {
            return;
          }

          if (!seen) {
            g_hash_table_add(plugin->seen_devices, g_strdup(address));
          }

          g_autoptr(FlValue) event = fl_value_new_map();
          fl_value_set_string_take(event, "type", fl_value_new_string("scanResult"));
          fl_value_set_string_take(event, "result",
                                   build_scan_result_from_proxy(interface_proxy));
          send_event(plugin, event);
          return;
        }
      }),
      self);

  return TRUE;
}

static GDBusProxy* first_proxy_for_interface(OmniBlePlugin* self,
                                             const gchar* interface_name) {
  if (self->object_manager == nullptr) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, interface_name);
    if (interface_ != nullptr) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);
  return proxy;
}

static GDBusProxy* device_proxy_for_id(OmniBlePlugin* self,
                                       const gchar* device_id) {
  if (self->object_manager == nullptr || device_id == nullptr ||
      strlen(device_id) == 0) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kDeviceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* candidate = G_DBUS_PROXY(interface_);
    g_autofree gchar* address = proxy_property_string_dup(candidate, "Address");
    if (g_strcmp0(address, device_id) == 0) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);
  return proxy;
}

static gboolean wait_for_device_connection_state(GDBusProxy* proxy,
                                                 gboolean connected,
                                                 gint timeout_ms) {
  const gint64 timeout_usecs =
      static_cast<gint64>(timeout_ms <= 0 ? 10000 : timeout_ms) * 1000;
  const gint64 deadline = g_get_monotonic_time() + timeout_usecs;

  while (g_get_monotonic_time() < deadline) {
    if (proxy_property_bool(proxy, "Connected", FALSE) == connected) {
      return TRUE;
    }

    while (g_main_context_pending(nullptr)) {
      g_main_context_iteration(nullptr, FALSE);
    }
    g_usleep(100000);
  }

  return proxy_property_bool(proxy, "Connected", FALSE) == connected;
}

static gboolean wait_for_services_resolved(GDBusProxy* device_proxy,
                                           gint timeout_ms) {
  const gint64 timeout_usecs =
      static_cast<gint64>(timeout_ms <= 0 ? 10000 : timeout_ms) * 1000;
  const gint64 deadline = g_get_monotonic_time() + timeout_usecs;

  while (g_get_monotonic_time() < deadline) {
    if (!proxy_property_bool(device_proxy, "Connected", FALSE)) {
      return FALSE;
    }
    if (proxy_property_bool(device_proxy, "ServicesResolved", FALSE)) {
      return TRUE;
    }

    while (g_main_context_pending(nullptr)) {
      g_main_context_iteration(nullptr, FALSE);
    }
    g_usleep(100000);
  }

  return proxy_property_bool(device_proxy, "ServicesResolved", FALSE);
}

static FlValue* gatt_properties_from_flags(GDBusProxy* proxy) {
  g_autoptr(FlValue) properties = fl_value_new_list();
  g_autoptr(GVariant) flags = proxy_property(proxy, "Flags");
  if (flags == nullptr || !g_variant_is_of_type(flags, G_VARIANT_TYPE("as"))) {
    return fl_value_ref(properties);
  }

  gboolean has_read = FALSE;
  gboolean has_write = FALSE;
  gboolean has_write_without_response = FALSE;
  gboolean has_notify = FALSE;
  gboolean has_indicate = FALSE;

  GVariantIter iter;
  const gchar* flag = nullptr;
  g_variant_iter_init(&iter, flags);
  while (g_variant_iter_next(&iter, "&s", &flag)) {
    if (g_strcmp0(flag, "read") == 0) {
      has_read = TRUE;
    } else if (g_strcmp0(flag, "write") == 0 ||
               g_strcmp0(flag, "reliable-write") == 0) {
      has_write = TRUE;
    } else if (g_strcmp0(flag, "write-without-response") == 0) {
      has_write_without_response = TRUE;
    } else if (g_strcmp0(flag, "notify") == 0) {
      has_notify = TRUE;
    } else if (g_strcmp0(flag, "indicate") == 0) {
      has_indicate = TRUE;
    }
  }

  if (has_read) {
    fl_value_append_take(properties, fl_value_new_string("read"));
  }
  if (has_write) {
    fl_value_append_take(properties, fl_value_new_string("write"));
  }
  if (has_write_without_response) {
    fl_value_append_take(properties,
                         fl_value_new_string("writeWithoutResponse"));
  }
  if (has_notify) {
    fl_value_append_take(properties, fl_value_new_string("notify"));
  }
  if (has_indicate) {
    fl_value_append_take(properties, fl_value_new_string("indicate"));
  }

  return fl_value_ref(properties);
}

static FlValue* gatt_permissions_from_flags(GDBusProxy* proxy) {
  g_autoptr(FlValue) permissions = fl_value_new_list();
  g_autoptr(GVariant) flags = proxy_property(proxy, "Flags");
  if (flags == nullptr || !g_variant_is_of_type(flags, G_VARIANT_TYPE("as"))) {
    return fl_value_ref(permissions);
  }

  gboolean has_read = FALSE;
  gboolean has_write = FALSE;
  gboolean has_read_encrypted = FALSE;
  gboolean has_write_encrypted = FALSE;

  GVariantIter iter;
  const gchar* flag = nullptr;
  g_variant_iter_init(&iter, flags);
  while (g_variant_iter_next(&iter, "&s", &flag)) {
    if (g_strcmp0(flag, "read") == 0) {
      has_read = TRUE;
    } else if (g_strcmp0(flag, "write") == 0 ||
               g_strcmp0(flag, "write-without-response") == 0 ||
               g_strcmp0(flag, "reliable-write") == 0) {
      has_write = TRUE;
    } else if (g_strcmp0(flag, "encrypt-read") == 0 ||
               g_strcmp0(flag, "encrypt-authenticated-read") == 0) {
      has_read_encrypted = TRUE;
    } else if (g_strcmp0(flag, "encrypt-write") == 0 ||
               g_strcmp0(flag, "encrypt-authenticated-write") == 0) {
      has_write_encrypted = TRUE;
    }
  }

  if (has_read) {
    fl_value_append_take(permissions, fl_value_new_string("read"));
  }
  if (has_write) {
    fl_value_append_take(permissions, fl_value_new_string("write"));
  }
  if (has_read_encrypted) {
    fl_value_append_take(permissions, fl_value_new_string("readEncrypted"));
  }
  if (has_write_encrypted) {
    fl_value_append_take(permissions, fl_value_new_string("writeEncrypted"));
  }

  return fl_value_ref(permissions);
}

static GDBusProxy* adapter_proxy(OmniBlePlugin* self) {
  if (self->object_manager == nullptr) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kAdapterInterface);
    if (interface_ != nullptr) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);

  return proxy;
}

static const gchar* current_adapter_state(OmniBlePlugin* self) {
  g_autoptr(GDBusProxy) proxy = adapter_proxy(self);
  if (proxy == nullptr) {
    return "unavailable";
  }

  return proxy_property_bool(proxy, "Powered", FALSE) ? "poweredOn"
                                                       : "poweredOff";
}

static void emit_existing_devices(OmniBlePlugin* self) {
  if (self->object_manager == nullptr || !self->is_scanning) {
    return;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kDeviceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* proxy = G_DBUS_PROXY(interface_);
    if (!device_matches_filters(self, proxy)) {
      continue;
    }

    gchar* address = proxy_property_string_dup(proxy, "Address");
    if (address == nullptr) {
      continue;
    }

    const gboolean seen = g_hash_table_contains(self->seen_devices, address);
    if (!self->allow_duplicates && seen) {
      g_free(address);
      continue;
    }
    if (!seen) {
      g_hash_table_add(self->seen_devices, address);
    } else {
      g_free(address);
    }

    g_autoptr(FlValue) event = fl_value_new_map();
    fl_value_set_string_take(event, "type", fl_value_new_string("scanResult"));
    fl_value_set_string_take(event, "result", build_scan_result_from_proxy(proxy));
    send_event(self, event);
  }

  g_list_free_full(objects, g_object_unref);
}

static const gchar* lookup_string_argument(FlValue* arguments,
                                           const gchar* key) {
  if (arguments == nullptr || fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }

  FlValue* value = fl_value_lookup_string(arguments, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
    return nullptr;
  }
  return fl_value_get_string(value);
}

static gint lookup_int_argument(FlValue* arguments,
                                const gchar* key,
                                gint default_value) {
  if (arguments == nullptr || fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
    return default_value;
  }

  FlValue* value = fl_value_lookup_string(arguments, key);
  if (value == nullptr) {
    return default_value;
  }
  if (fl_value_get_type(value) == FL_VALUE_TYPE_INT) {
    return static_cast<gint>(fl_value_get_int(value));
  }
  return default_value;
}

static FlValue* build_descriptor_payload(GDBusProxy* proxy) {
  g_autoptr(FlValue) descriptor = fl_value_new_map();
  g_autofree gchar* uuid = proxy_property_string_dup(proxy, "UUID");
  fl_value_set_string_take(
      descriptor, "uuid",
      fl_value_new_string(normalize_uuid(uuid).c_str()));
  fl_value_set_string_take(descriptor, "permissions",
                           gatt_permissions_from_flags(proxy));
  fl_value_set_string_take(descriptor, "initialValue",
                           bytes_property_from_proxy(proxy, "Value"));
  return fl_value_ref(descriptor);
}

static FlValue* build_characteristic_payload(GList* objects,
                                             const gchar* characteristic_path,
                                             GDBusProxy* characteristic_proxy) {
  g_autoptr(FlValue) characteristic = fl_value_new_map();
  g_autofree gchar* uuid = proxy_property_string_dup(characteristic_proxy, "UUID");
  fl_value_set_string_take(
      characteristic, "uuid",
      fl_value_new_string(normalize_uuid(uuid).c_str()));
  fl_value_set_string_take(characteristic, "properties",
                           gatt_properties_from_flags(characteristic_proxy));
  fl_value_set_string_take(characteristic, "permissions",
                           gatt_permissions_from_flags(characteristic_proxy));
  fl_value_set_string_take(characteristic, "initialValue",
                           bytes_property_from_proxy(characteristic_proxy, "Value"));

  g_autoptr(FlValue) descriptors = fl_value_new_list();
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattDescriptorInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* descriptor_proxy = G_DBUS_PROXY(interface_);
    const gchar* descriptor_path = g_dbus_proxy_get_object_path(descriptor_proxy);
    if (!object_path_is_child_of(descriptor_path, characteristic_path) ||
        g_strcmp0(descriptor_path, characteristic_path) == 0) {
      continue;
    }

    fl_value_append_take(descriptors, build_descriptor_payload(descriptor_proxy));
  }

  fl_value_set_string_take(characteristic, "descriptors", fl_value_ref(descriptors));
  return fl_value_ref(characteristic);
}

static FlValue* build_service_payload(GList* objects,
                                      const gchar* service_path,
                                      GDBusProxy* service_proxy) {
  g_autoptr(FlValue) service = fl_value_new_map();
  g_autofree gchar* uuid = proxy_property_string_dup(service_proxy, "UUID");
  fl_value_set_string_take(
      service, "uuid", fl_value_new_string(normalize_uuid(uuid).c_str()));
  fl_value_set_string_take(
      service, "primary",
      fl_value_new_bool(proxy_property_bool(service_proxy, "Primary", TRUE)));

  g_autoptr(FlValue) characteristics = fl_value_new_list();
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattCharacteristicInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* characteristic_proxy = G_DBUS_PROXY(interface_);
    const gchar* characteristic_path =
        g_dbus_proxy_get_object_path(characteristic_proxy);
    if (!object_path_is_child_of(characteristic_path, service_path) ||
        g_strcmp0(characteristic_path, service_path) == 0) {
      continue;
    }

    fl_value_append_take(
        characteristics,
        build_characteristic_payload(objects, characteristic_path,
                                     characteristic_proxy));
  }

  fl_value_set_string_take(service, "characteristics",
                           fl_value_ref(characteristics));
  return fl_value_ref(service);
}

static FlValue* build_services_for_device(OmniBlePlugin* self,
                                          const gchar* device_path) {
  g_autoptr(FlValue) services = fl_value_new_list();
  if (self->object_manager == nullptr || device_path == nullptr) {
    return fl_value_ref(services);
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattServiceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* service_proxy = G_DBUS_PROXY(interface_);
    const gchar* service_path = g_dbus_proxy_get_object_path(service_proxy);
    if (!object_path_is_child_of(service_path, device_path) ||
        g_strcmp0(service_path, device_path) == 0) {
      continue;
    }

    fl_value_append_take(
        services,
        build_service_payload(objects, service_path, service_proxy));
  }

  g_list_free_full(objects, g_object_unref);
  return fl_value_ref(services);
}

static FlMethodResponse* connect_device(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  g_autoptr(GDBusProxy) adapter = first_proxy_for_interface(self, kAdapterInterface);
  if (adapter == nullptr) {
    return error_response("adapter-unavailable",
                          "Bluetooth adapter must be available before connecting.");
  }
  if (!proxy_property_bool(adapter, "Powered", FALSE)) {
    g_autoptr(FlValue) details = fl_value_new_map();
    fl_value_set_string_take(details, "state", fl_value_new_string("poweredOff"));
    return error_response(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before connecting.", details);
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  if (device_id == nullptr || strlen(device_id) == 0) {
    return error_response("invalid-argument",
                          "`deviceId` is required to connect.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth device is not available. Scan first or reconnect to a known device.");
  }

  if (proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return success_response();
  }

  emit_connection_state(self, device_id, "connecting");
  gint timeout_ms = lookup_int_argument(arguments, "timeoutMs", 10000);
  if (g_dbus_proxy_call_sync(device_proxy, "Connect", g_variant_new("()"),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    emit_connection_state(self, device_id, "disconnected");
    return error_response("connection-failed",
                          error != nullptr ? error->message
                                           : "Failed to connect to the device.");
  }

  if (!wait_for_device_connection_state(device_proxy, TRUE, timeout_ms)) {
    emit_connection_state(self, device_id, "disconnected");
    return error_response("connection-failed",
                          "Bluetooth connection timed out on Linux.");
  }

  return success_response();
}

static FlMethodResponse* disconnect_device(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  if (device_id == nullptr || strlen(device_id) == 0) {
    return error_response("invalid-argument",
                          "`deviceId` is required to disconnect.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr ||
      !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return success_response();
  }

  emit_connection_state(self, device_id, "disconnecting");
  if (g_dbus_proxy_call_sync(device_proxy, "Disconnect", g_variant_new("()"),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    return error_response("disconnect-failed",
                          error != nullptr ? error->message
                                           : "Failed to disconnect from the device.");
  }

  wait_for_device_connection_state(device_proxy, FALSE, 5000);
  return success_response();
}

static FlMethodResponse* discover_services(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  if (device_id == nullptr || strlen(device_id) == 0) {
    return error_response("invalid-argument",
                          "`deviceId` is required to discover services.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before discovering services.");
  }

  if (!wait_for_services_resolved(device_proxy, 10000)) {
    return error_response("discovery-failed",
                          "Bluetooth service discovery timed out on Linux.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  return success_response(build_services_for_device(self, device_path));
}

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
  return success_response(result);
}

static FlMethodResponse* permission_rationale_response(FlValue* arguments) {
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
                               fl_value_new_bool(FALSE));
    }
  }

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "permissions", fl_value_ref(permissions));
  return success_response(result);
}

static FlMethodResponse* start_scan(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  g_autoptr(GDBusProxy) proxy = adapter_proxy(self);
  if (proxy == nullptr) {
    return error_response("adapter-unavailable",
                          "Bluetooth adapter must be available before scanning.");
  }

  if (!proxy_property_bool(proxy, "Powered", FALSE)) {
    g_autoptr(FlValue) details = fl_value_new_map();
    fl_value_set_string_take(details, "state",
                             fl_value_new_string("poweredOff"));
    return error_response("adapter-unavailable",
                          "Bluetooth adapter must be powered on before scanning.",
                          details);
  }

  clear_service_filters(self);
  clear_seen_devices(self);
  self->allow_duplicates = FALSE;

  if (arguments != nullptr && fl_value_get_type(arguments) == FL_VALUE_TYPE_MAP) {
    FlValue* allow_duplicates = fl_value_lookup_string(arguments, "allowDuplicates");
    if (allow_duplicates != nullptr &&
        fl_value_get_type(allow_duplicates) == FL_VALUE_TYPE_BOOL) {
      self->allow_duplicates = fl_value_get_bool(allow_duplicates);
    }

    FlValue* service_uuids = fl_value_lookup_string(arguments, "serviceUuids");
    if (service_uuids != nullptr &&
        fl_value_get_type(service_uuids) == FL_VALUE_TYPE_LIST) {
      const size_t uuid_count = fl_value_get_length(service_uuids);
      for (size_t index = 0; index < uuid_count; index++) {
        FlValue* uuid_value = fl_value_get_list_value(service_uuids, index);
        if (fl_value_get_type(uuid_value) != FL_VALUE_TYPE_STRING) {
          continue;
        }
        const std::string normalized =
            normalize_uuid(fl_value_get_string(uuid_value));
        g_ptr_array_add(self->service_filters, g_strdup(normalized.c_str()));
      }
    }
  }

  GVariantBuilder filter_builder;
  g_variant_builder_init(&filter_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&filter_builder, "{sv}", "Transport",
                        g_variant_new_string("le"));
  g_variant_builder_add(&filter_builder, "{sv}", "DuplicateData",
                        g_variant_new_boolean(self->allow_duplicates));

  if (self->service_filters->len > 0) {
    GVariantBuilder uuid_builder;
    g_variant_builder_init(&uuid_builder, G_VARIANT_TYPE("as"));
    for (guint index = 0; index < self->service_filters->len; index++) {
      g_variant_builder_add(
          &uuid_builder, "s",
          static_cast<const gchar*>(g_ptr_array_index(self->service_filters,
                                                      index)));
    }
    g_variant_builder_add(&filter_builder, "{sv}", "UUIDs",
                          g_variant_builder_end(&uuid_builder));
  }

  g_autoptr(GVariant) filter = g_variant_builder_end(&filter_builder);
  g_autoptr(GVariant) set_filter_params = g_variant_new_tuple(&filter, 1);
  if (g_dbus_proxy_call_sync(proxy, "SetDiscoveryFilter", set_filter_params,
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    return error_response("scan-failed",
                          error != nullptr ? error->message
                                           : "Failed to configure discovery filter.");
  }

  g_clear_error(&error);
  if (g_dbus_proxy_call_sync(proxy, "StartDiscovery", g_variant_new("()"),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    return error_response("scan-failed",
                          error != nullptr ? error->message
                                           : "Failed to start discovery.");
  }

  self->is_scanning = TRUE;
  emit_adapter_state(self, current_adapter_state(self));
  emit_existing_devices(self);
  return success_response();
}

static FlMethodResponse* stop_scan(OmniBlePlugin* self) {
  self->is_scanning = FALSE;

  if (self->object_manager == nullptr) {
    return success_response();
  }

  g_autoptr(GDBusProxy) proxy = adapter_proxy(self);
  if (proxy == nullptr) {
    return success_response();
  }

  g_autoptr(GError) error = nullptr;
  if (g_dbus_proxy_call_sync(proxy, "StopDiscovery", g_variant_new("()"),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    return error_response("scan-failed",
                          error != nullptr ? error->message
                                           : "Failed to stop discovery.");
  }

  return success_response();
}

// Called when a method call is received from Flutter.
static void omni_ble_plugin_handle_method_call(OmniBlePlugin* self,
                                               FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getCapabilities") == 0) {
    response = get_capabilities();
  } else if (strcmp(method, "checkPermissions") == 0 ||
             strcmp(method, "requestPermissions") == 0) {
    response = permission_status_response(fl_method_call_get_args(method_call));
  } else if (strcmp(method, "shouldShowRequestRationale") == 0) {
    response = permission_rationale_response(fl_method_call_get_args(method_call));
  } else if (strcmp(method, "openAppSettings") == 0 ||
             strcmp(method, "openBluetoothSettings") == 0) {
    response = success_response(fl_value_new_bool(FALSE));
  } else if (strcmp(method, "startScan") == 0) {
    response = start_scan(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "stopScan") == 0) {
    response = stop_scan(self);
  } else if (strcmp(method, "connect") == 0) {
    response = connect_device(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "disconnect") == 0) {
    response = disconnect_device(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "discoverServices") == 0) {
    response = discover_services(self, fl_method_call_get_args(method_call));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_capabilities() {
  struct utsname uname_data = {};
  uname(&uname_data);

  g_autoptr(FlValue) features = fl_value_new_list();
  fl_value_append_take(features, fl_value_new_string("central"));
  fl_value_append_take(features, fl_value_new_string("scanning"));

  g_autoptr(FlValue) metadata = fl_value_new_map();
  fl_value_set_string_take(metadata, "adapterState",
                           fl_value_new_string("unknown"));

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "platform", fl_value_new_string("linux"));
  fl_value_set_string_take(result, "platformVersion",
                           fl_value_new_string(uname_data.version));
  fl_value_set_string_take(result, "availableFeatures", fl_value_ref(features));
  fl_value_set_string_take(result, "metadata", fl_value_ref(metadata));
  return success_response(result);
}

static FlMethodErrorResponse* event_listen_cb(FlEventChannel* channel,
                                              FlValue* args,
                                              gpointer user_data) {
  auto* self = OMNI_BLE_PLUGIN(user_data);
  self->event_listening = TRUE;

  g_autoptr(GError) error = nullptr;
  if (ensure_object_manager(self, &error)) {
    emit_adapter_state(self, current_adapter_state(self));
    if (self->is_scanning) {
      emit_existing_devices(self);
    }
    return nullptr;
  }

  emit_adapter_state(self, "unavailable");
  return nullptr;
}

static FlMethodErrorResponse* event_cancel_cb(FlEventChannel* channel,
                                              FlValue* args,
                                              gpointer user_data) {
  auto* self = OMNI_BLE_PLUGIN(user_data);
  self->event_listening = FALSE;
  return nullptr;
}

static void omni_ble_plugin_dispose(GObject* object) {
  OmniBlePlugin* self = OMNI_BLE_PLUGIN(object);

  if (self->event_channel != nullptr) {
    g_object_unref(self->event_channel);
    self->event_channel = nullptr;
  }

  if (self->object_manager != nullptr) {
    if (self->object_added_handler_id != 0) {
      g_signal_handler_disconnect(self->object_manager,
                                  self->object_added_handler_id);
      self->object_added_handler_id = 0;
    }
    if (self->properties_changed_handler_id != 0) {
      g_signal_handler_disconnect(self->object_manager,
                                  self->properties_changed_handler_id);
      self->properties_changed_handler_id = 0;
    }
    g_object_unref(self->object_manager);
    self->object_manager = nullptr;
  }

  if (self->service_filters != nullptr) {
    g_ptr_array_unref(self->service_filters);
    self->service_filters = nullptr;
  }

  if (self->seen_devices != nullptr) {
    g_hash_table_unref(self->seen_devices);
    self->seen_devices = nullptr;
  }

  G_OBJECT_CLASS(omni_ble_plugin_parent_class)->dispose(object);
}

static void omni_ble_plugin_class_init(OmniBlePluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = omni_ble_plugin_dispose;
}

static void omni_ble_plugin_init(OmniBlePlugin* self) {
  self->allow_duplicates = FALSE;
  self->service_filters = g_ptr_array_new_with_free_func(g_free);
  self->seen_devices =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
}

static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  OmniBlePlugin* plugin = OMNI_BLE_PLUGIN(user_data);
  omni_ble_plugin_handle_method_call(plugin, method_call);
}

void omni_ble_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  OmniBlePlugin* plugin = OMNI_BLE_PLUGIN(
      g_object_new(omni_ble_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) method_channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "omni_ble/methods", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(method_channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  plugin->event_channel = fl_event_channel_new(
      fl_plugin_registrar_get_messenger(registrar), "omni_ble/events",
      FL_METHOD_CODEC(codec));
  fl_event_channel_set_stream_handlers(plugin->event_channel, event_listen_cb,
                                       event_cancel_cb, g_object_ref(plugin),
                                       g_object_unref);

  g_object_unref(plugin);
}
