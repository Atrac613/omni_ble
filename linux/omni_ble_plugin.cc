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
constexpr char kGattManagerInterface[] = "org.bluez.GattManager1";
constexpr char kGattServiceInterface[] = "org.bluez.GattService1";
constexpr char kGattCharacteristicInterface[] = "org.bluez.GattCharacteristic1";
constexpr char kGattDescriptorInterface[] = "org.bluez.GattDescriptor1";
constexpr char kAdvertisingManagerInterface[] = "org.bluez.LEAdvertisingManager1";
constexpr char kAdvertisementInterface[] = "org.bluez.LEAdvertisement1";
constexpr char kObjectManagerInterface[] = "org.freedesktop.DBus.ObjectManager";
constexpr char kApplicationPath[] = "/dev/noguwo/omni_ble";
constexpr char kAdvertisementPath[] = "/dev/noguwo/omni_ble/advertisement0";

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

typedef struct {
  gchar* uuid;
  gchar* path;
  gchar* characteristic_path;
  guint registration_id;
  GByteArray* value;
  GPtrArray* flags;
} OmniBleServerDescriptor;

typedef struct {
  gchar* uuid;
  gchar* path;
  gchar* service_path;
  gchar* service_uuid;
  guint registration_id;
  GByteArray* value;
  GPtrArray* flags;
  gboolean notifying;
  GPtrArray* descriptors;
} OmniBleServerCharacteristic;

typedef struct {
  gchar* uuid;
  gchar* path;
  guint registration_id;
  gboolean primary;
  GPtrArray* characteristics;
} OmniBleServerService;

typedef struct {
  gchar* request_id;
  GDBusMethodInvocation* invocation;
  OmniBleServerCharacteristic* characteristic;
  gchar* device_id;
  guint offset;
} PendingServerReadRequest;

typedef struct {
  gchar* request_id;
  GDBusMethodInvocation* invocation;
  OmniBleServerCharacteristic* characteristic;
  gchar* device_id;
  guint offset;
  GByteArray* value;
  gboolean response_needed;
  gboolean prepared_write;
} PendingServerWriteRequest;

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
  GDBusConnection* server_connection;
  guint app_registration_id;
  gboolean app_registered;
  GPtrArray* server_services;
  GHashTable* server_characteristics;
  GHashTable* pending_server_read_requests;
  GHashTable* pending_server_write_requests;
  guint64 next_request_id;
  guint advertisement_registration_id;
  gboolean advertisement_registered;
  gboolean is_advertising;
  gchar* advertisement_local_name;
  gboolean advertisement_connectable;
  gboolean advertisement_include_tx_power;
  GPtrArray* advertisement_service_uuids;
  GHashTable* advertisement_service_data;
  GByteArray* advertisement_manufacturer_data;
};

static void reset_advertisement_state(OmniBlePlugin* self);
static void clear_server_database_state(OmniBlePlugin* self);
static FlMethodResponse* publish_gatt_database(OmniBlePlugin* self,
                                               FlValue* arguments);
static FlMethodResponse* clear_gatt_database_response(OmniBlePlugin* self);
static FlMethodResponse* start_advertising(OmniBlePlugin* self,
                                           FlValue* arguments);
static FlMethodResponse* stop_advertising_response(OmniBlePlugin* self);
static FlMethodResponse* notify_characteristic_value(OmniBlePlugin* self,
                                                     FlValue* arguments);
static FlMethodResponse* respond_to_read_request(OmniBlePlugin* self,
                                                 FlValue* arguments);
static FlMethodResponse* respond_to_write_request(OmniBlePlugin* self,
                                                  FlValue* arguments);

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

static void destroy_byte_array(gpointer data) {
  if (data != nullptr) {
    g_byte_array_unref(static_cast<GByteArray*>(data));
  }
}

static void destroy_server_descriptor(gpointer data) {
  auto* descriptor = static_cast<OmniBleServerDescriptor*>(data);
  if (descriptor == nullptr) {
    return;
  }

  g_free(descriptor->uuid);
  g_free(descriptor->path);
  g_free(descriptor->characteristic_path);
  if (descriptor->value != nullptr) {
    g_byte_array_unref(descriptor->value);
  }
  if (descriptor->flags != nullptr) {
    g_ptr_array_unref(descriptor->flags);
  }
  g_free(descriptor);
}

static void destroy_server_characteristic(gpointer data) {
  auto* characteristic = static_cast<OmniBleServerCharacteristic*>(data);
  if (characteristic == nullptr) {
    return;
  }

  g_free(characteristic->uuid);
  g_free(characteristic->path);
  g_free(characteristic->service_path);
  g_free(characteristic->service_uuid);
  if (characteristic->value != nullptr) {
    g_byte_array_unref(characteristic->value);
  }
  if (characteristic->flags != nullptr) {
    g_ptr_array_unref(characteristic->flags);
  }
  if (characteristic->descriptors != nullptr) {
    g_ptr_array_unref(characteristic->descriptors);
  }
  g_free(characteristic);
}

static void destroy_server_service(gpointer data) {
  auto* service = static_cast<OmniBleServerService*>(data);
  if (service == nullptr) {
    return;
  }

  g_free(service->uuid);
  g_free(service->path);
  if (service->characteristics != nullptr) {
    g_ptr_array_unref(service->characteristics);
  }
  g_free(service);
}

static void destroy_pending_read_request(gpointer data) {
  auto* request = static_cast<PendingServerReadRequest*>(data);
  if (request == nullptr) {
    return;
  }

  g_free(request->request_id);
  g_free(request->device_id);
  if (request->invocation != nullptr) {
    g_object_unref(request->invocation);
  }
  g_free(request);
}

static void destroy_pending_write_request(gpointer data) {
  auto* request = static_cast<PendingServerWriteRequest*>(data);
  if (request == nullptr) {
    return;
  }

  g_free(request->request_id);
  g_free(request->device_id);
  if (request->invocation != nullptr) {
    g_object_unref(request->invocation);
  }
  if (request->value != nullptr) {
    g_byte_array_unref(request->value);
  }
  g_free(request);
}

static GByteArray* copy_byte_array(const GByteArray* source) {
  auto* bytes = g_byte_array_new();
  if (source != nullptr && source->len > 0) {
    g_byte_array_append(bytes, source->data, source->len);
  }
  return bytes;
}

static gboolean merge_byte_array_value(GByteArray* target,
                                       guint offset,
                                       const GByteArray* incoming) {
  if (target == nullptr || incoming == nullptr || offset > target->len) {
    return FALSE;
  }

  const guint required_length = offset + incoming->len;
  if (required_length > target->len) {
    g_byte_array_set_size(target, required_length);
  }

  if (incoming->len > 0) {
    memcpy(target->data + offset, incoming->data, incoming->len);
  }
  return TRUE;
}

static GByteArray* byte_array_from_fl_value(FlValue* value) {
  auto* bytes = g_byte_array_new();
  if (value == nullptr) {
    return bytes;
  }

  if (fl_value_get_type(value) == FL_VALUE_TYPE_LIST) {
    const size_t length = fl_value_get_length(value);
    for (size_t index = 0; index < length; index++) {
      FlValue* item = fl_value_get_list_value(value, index);
      if (fl_value_get_type(item) == FL_VALUE_TYPE_INT) {
        const guint8 byte = static_cast<guint8>(fl_value_get_int(item));
        g_byte_array_append(bytes, &byte, 1);
      }
    }
  }

  return bytes;
}

static GByteArray* byte_array_from_variant(GVariant* value) {
  auto* bytes = g_byte_array_new();
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE("ay"))) {
    return bytes;
  }

  gsize length = 0;
  const guint8* data = static_cast<const guint8*>(
      g_variant_get_fixed_array(value, &length, sizeof(guint8)));
  if (data != nullptr && length > 0) {
    g_byte_array_append(bytes, data, length);
  }
  return bytes;
}

static FlValue* fl_value_from_byte_array(const GByteArray* value) {
  g_autoptr(FlValue) bytes = fl_value_new_list();
  if (value == nullptr) {
    return fl_value_ref(bytes);
  }

  for (guint index = 0; index < value->len; index++) {
    fl_value_append_take(bytes, fl_value_new_int(value->data[index]));
  }
  return fl_value_ref(bytes);
}

static GVariant* byte_array_variant_from_bytes(const GByteArray* value) {
  if (value == nullptr || value->len == 0) {
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, nullptr, 0,
                                     sizeof(guint8));
  }

  return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value->data, value->len,
                                   sizeof(guint8));
}

static GVariant* string_array_variant_from_ptr_array(GPtrArray* values) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
  if (values != nullptr) {
    for (guint index = 0; index < values->len; index++) {
      const auto* value =
          static_cast<const gchar*>(g_ptr_array_index(values, index));
      if (value != nullptr) {
        g_variant_builder_add(&builder, "s", value);
      }
    }
  }
  return g_variant_builder_end(&builder);
}

static gchar* server_characteristic_key(const gchar* service_uuid,
                                        const gchar* characteristic_uuid) {
  const std::string normalized_service = normalize_uuid(service_uuid);
  const std::string normalized_characteristic = normalize_uuid(characteristic_uuid);
  return g_strdup_printf("%s|%s", normalized_service.c_str(),
                         normalized_characteristic.c_str());
}

static gboolean ptr_array_contains_string(GPtrArray* values,
                                          const gchar* expected) {
  if (values == nullptr || expected == nullptr) {
    return FALSE;
  }

  for (guint index = 0; index < values->len; index++) {
    const auto* value =
        static_cast<const gchar*>(g_ptr_array_index(values, index));
    if (g_strcmp0(value, expected) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static GDBusNodeInfo* object_manager_node_info() {
  static GDBusNodeInfo* node_info = nullptr;
  if (node_info == nullptr) {
    static constexpr char kXml[] =
        "<node>"
        "  <interface name='org.freedesktop.DBus.ObjectManager'>"
        "    <method name='GetManagedObjects'>"
        "      <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>"
        "    </method>"
        "  </interface>"
        "</node>";
    g_autoptr(GError) error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(kXml, &error);
  }
  return node_info;
}

static GDBusNodeInfo* gatt_service_node_info() {
  static GDBusNodeInfo* node_info = nullptr;
  if (node_info == nullptr) {
    static constexpr char kXml[] =
        "<node>"
        "  <interface name='org.bluez.GattService1'>"
        "    <property name='UUID' type='s' access='read'/>"
        "    <property name='Primary' type='b' access='read'/>"
        "    <property name='Includes' type='ao' access='read'/>"
        "  </interface>"
        "</node>";
    g_autoptr(GError) error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(kXml, &error);
  }
  return node_info;
}

static GDBusNodeInfo* gatt_characteristic_node_info() {
  static GDBusNodeInfo* node_info = nullptr;
  if (node_info == nullptr) {
    static constexpr char kXml[] =
        "<node>"
        "  <interface name='org.bluez.GattCharacteristic1'>"
        "    <method name='ReadValue'>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "      <arg name='value' type='ay' direction='out'/>"
        "    </method>"
        "    <method name='WriteValue'>"
        "      <arg name='value' type='ay' direction='in'/>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "    </method>"
        "    <method name='StartNotify'/>"
        "    <method name='StopNotify'/>"
        "    <property name='UUID' type='s' access='read'/>"
        "    <property name='Service' type='o' access='read'/>"
        "    <property name='Value' type='ay' access='read'/>"
        "    <property name='Notifying' type='b' access='read'/>"
        "    <property name='Flags' type='as' access='read'/>"
        "  </interface>"
        "</node>";
    g_autoptr(GError) error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(kXml, &error);
  }
  return node_info;
}

static GDBusNodeInfo* gatt_descriptor_node_info() {
  static GDBusNodeInfo* node_info = nullptr;
  if (node_info == nullptr) {
    static constexpr char kXml[] =
        "<node>"
        "  <interface name='org.bluez.GattDescriptor1'>"
        "    <method name='ReadValue'>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "      <arg name='value' type='ay' direction='out'/>"
        "    </method>"
        "    <method name='WriteValue'>"
        "      <arg name='value' type='ay' direction='in'/>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "    </method>"
        "    <property name='UUID' type='s' access='read'/>"
        "    <property name='Characteristic' type='o' access='read'/>"
        "    <property name='Value' type='ay' access='read'/>"
        "    <property name='Flags' type='as' access='read'/>"
        "  </interface>"
        "</node>";
    g_autoptr(GError) error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(kXml, &error);
  }
  return node_info;
}

static GDBusNodeInfo* advertisement_node_info() {
  static GDBusNodeInfo* node_info = nullptr;
  if (node_info == nullptr) {
    static constexpr char kXml[] =
        "<node>"
        "  <interface name='org.bluez.LEAdvertisement1'>"
        "    <method name='Release'/>"
        "    <property name='Type' type='s' access='read'/>"
        "    <property name='ServiceUUIDs' type='as' access='read'/>"
        "    <property name='ManufacturerData' type='a{qv}' access='read'/>"
        "    <property name='ServiceData' type='a{sv}' access='read'/>"
        "    <property name='LocalName' type='s' access='read'/>"
        "    <property name='Includes' type='as' access='read'/>"
        "  </interface>"
        "</node>";
    g_autoptr(GError) error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(kXml, &error);
  }
  return node_info;
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

        if (g_strcmp0(interface_name, kGattCharacteristicInterface) == 0 &&
            changed_properties_contains(changed_properties, "Value")) {
          const gchar* object_path =
              g_dbus_proxy_get_object_path(interface_proxy);
          g_autofree gchar* device_id =
              device_id_for_object_path(plugin, object_path);
          g_autofree gchar* service_uuid =
              service_uuid_for_characteristic_path(plugin, object_path);
          g_autofree gchar* characteristic_uuid =
              proxy_property_string_dup(interface_proxy, "UUID");
          if (device_id == nullptr || service_uuid == nullptr ||
              characteristic_uuid == nullptr) {
            return;
          }

          g_autoptr(GVariant) value =
              g_variant_lookup_value(changed_properties, "Value", nullptr);
          if (value == nullptr) {
            value = proxy_property(interface_proxy, "Value");
          }

          g_autoptr(FlValue) event = fl_value_new_map();
          fl_value_set_string_take(
              event, "type", fl_value_new_string("characteristicValueChanged"));
          fl_value_set_string_take(event, "deviceId",
                                   fl_value_new_string(device_id));
          fl_value_set_string_take(
              event, "serviceUuid",
              fl_value_new_string(normalize_uuid(service_uuid).c_str()));
          fl_value_set_string_take(
              event, "characteristicUuid",
              fl_value_new_string(normalize_uuid(characteristic_uuid).c_str()));
          fl_value_set_string_take(event, "value",
                                   bytes_value_from_variant(value));
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

static GPtrArray* server_characteristic_flags(FlValue* properties_value,
                                              FlValue* permissions_value) {
  auto* flags = g_ptr_array_new_with_free_func(g_free);
  auto has_permission = [permissions_value](const gchar* expected) {
    if (permissions_value == nullptr ||
        fl_value_get_type(permissions_value) != FL_VALUE_TYPE_LIST) {
      return FALSE;
    }
    const size_t count = fl_value_get_length(permissions_value);
    for (size_t index = 0; index < count; index++) {
      FlValue* value = fl_value_get_list_value(permissions_value, index);
      if (fl_value_get_type(value) == FL_VALUE_TYPE_STRING &&
          g_strcmp0(fl_value_get_string(value), expected) == 0) {
        return TRUE;
      }
    }
    return FALSE;
  };
  auto append_unique = [flags](const gchar* flag) {
    if (!ptr_array_contains_string(flags, flag)) {
      g_ptr_array_add(flags, g_strdup(flag));
    }
  };

  if (properties_value != nullptr &&
      fl_value_get_type(properties_value) == FL_VALUE_TYPE_LIST) {
    const size_t count = fl_value_get_length(properties_value);
    for (size_t index = 0; index < count; index++) {
      FlValue* value = fl_value_get_list_value(properties_value, index);
      if (fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
        continue;
      }

      const gchar* property = fl_value_get_string(value);
      if (g_strcmp0(property, "read") == 0) {
        append_unique(has_permission("readEncrypted") ? "encrypt-read" : "read");
      } else if (g_strcmp0(property, "write") == 0) {
        append_unique(has_permission("writeEncrypted") ? "encrypt-write" : "write");
      } else if (g_strcmp0(property, "writeWithoutResponse") == 0) {
        append_unique("write-without-response");
      } else if (g_strcmp0(property, "notify") == 0) {
        append_unique("notify");
      } else if (g_strcmp0(property, "indicate") == 0) {
        append_unique("indicate");
      }
    }
  }

  return flags;
}

static GPtrArray* server_descriptor_flags(FlValue* permissions_value) {
  auto* flags = g_ptr_array_new_with_free_func(g_free);
  auto append_unique = [flags](const gchar* flag) {
    if (!ptr_array_contains_string(flags, flag)) {
      g_ptr_array_add(flags, g_strdup(flag));
    }
  };

  if (permissions_value != nullptr &&
      fl_value_get_type(permissions_value) == FL_VALUE_TYPE_LIST) {
    const size_t count = fl_value_get_length(permissions_value);
    for (size_t index = 0; index < count; index++) {
      FlValue* value = fl_value_get_list_value(permissions_value, index);
      if (fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
        continue;
      }
      const gchar* permission = fl_value_get_string(value);
      if (g_strcmp0(permission, "read") == 0) {
        append_unique("read");
      } else if (g_strcmp0(permission, "write") == 0) {
        append_unique("write");
      } else if (g_strcmp0(permission, "readEncrypted") == 0) {
        append_unique("encrypt-read");
      } else if (g_strcmp0(permission, "writeEncrypted") == 0) {
        append_unique("encrypt-write");
      }
    }
  }

  return flags;
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

static gboolean ensure_server_connection(OmniBlePlugin* self, GError** error) {
  if (!ensure_object_manager(self, error)) {
    return FALSE;
  }

  if (self->server_connection != nullptr) {
    return TRUE;
  }

  self->server_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, error);
  return self->server_connection != nullptr;
}

static GDBusProxy* gatt_manager_proxy(OmniBlePlugin* self) {
  return first_proxy_for_interface(self, kGattManagerInterface);
}

static GDBusProxy* advertising_manager_proxy(OmniBlePlugin* self) {
  return first_proxy_for_interface(self, kAdvertisingManagerInterface);
}

static OmniBleServerCharacteristic* server_characteristic_for_key(
    OmniBlePlugin* self,
    const gchar* service_uuid,
    const gchar* characteristic_uuid) {
  if (self->server_characteristics == nullptr) {
    return nullptr;
  }

  g_autofree gchar* key =
      server_characteristic_key(service_uuid, characteristic_uuid);
  return static_cast<OmniBleServerCharacteristic*>(
      g_hash_table_lookup(self->server_characteristics, key));
}

static OmniBleServerCharacteristic* server_characteristic_for_path(
    OmniBlePlugin* self,
    const gchar* object_path) {
  if (self->server_services == nullptr || object_path == nullptr) {
    return nullptr;
  }

  for (guint service_index = 0; service_index < self->server_services->len;
       service_index++) {
    auto* service = static_cast<OmniBleServerService*>(
        g_ptr_array_index(self->server_services, service_index));
    for (guint characteristic_index = 0;
         characteristic_index < service->characteristics->len;
         characteristic_index++) {
      auto* characteristic = static_cast<OmniBleServerCharacteristic*>(
          g_ptr_array_index(service->characteristics, characteristic_index));
      if (g_strcmp0(characteristic->path, object_path) == 0) {
        return characteristic;
      }
    }
  }

  return nullptr;
}

static OmniBleServerDescriptor* server_descriptor_for_path(OmniBlePlugin* self,
                                                           const gchar* object_path) {
  if (self->server_services == nullptr || object_path == nullptr) {
    return nullptr;
  }

  for (guint service_index = 0; service_index < self->server_services->len;
       service_index++) {
    auto* service = static_cast<OmniBleServerService*>(
        g_ptr_array_index(self->server_services, service_index));
    for (guint characteristic_index = 0;
         characteristic_index < service->characteristics->len;
         characteristic_index++) {
      auto* characteristic = static_cast<OmniBleServerCharacteristic*>(
          g_ptr_array_index(service->characteristics, characteristic_index));
      for (guint descriptor_index = 0;
           descriptor_index < characteristic->descriptors->len;
           descriptor_index++) {
        auto* descriptor = static_cast<OmniBleServerDescriptor*>(
            g_ptr_array_index(characteristic->descriptors, descriptor_index));
        if (g_strcmp0(descriptor->path, object_path) == 0) {
          return descriptor;
        }
      }
    }
  }

  return nullptr;
}

static PendingServerReadRequest* pending_server_read_request_for_id(
    OmniBlePlugin* self,
    const gchar* request_id) {
  if (self->pending_server_read_requests == nullptr || request_id == nullptr) {
    return nullptr;
  }
  return static_cast<PendingServerReadRequest*>(
      g_hash_table_lookup(self->pending_server_read_requests, request_id));
}

static PendingServerWriteRequest* pending_server_write_request_for_id(
    OmniBlePlugin* self,
    const gchar* request_id) {
  if (self->pending_server_write_requests == nullptr || request_id == nullptr) {
    return nullptr;
  }
  return static_cast<PendingServerWriteRequest*>(
      g_hash_table_lookup(self->pending_server_write_requests, request_id));
}

static gchar* next_request_id(OmniBlePlugin* self, const gchar* prefix) {
  return g_strdup_printf("%s-%" G_GUINT64_FORMAT, prefix, self->next_request_id++);
}

static void emit_properties_changed(OmniBlePlugin* self,
                                    const gchar* object_path,
                                    const gchar* interface_name,
                                    GVariant* changed_properties) {
  if (self->server_connection == nullptr || object_path == nullptr ||
      interface_name == nullptr || changed_properties == nullptr) {
    return;
  }

  g_dbus_connection_emit_signal(
      self->server_connection, nullptr, object_path, kPropertiesInterface,
      "PropertiesChanged",
      g_variant_new("(s@a{sv}@as)", interface_name, changed_properties,
                    g_variant_new_strv(nullptr, 0)),
      nullptr);
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

static GDBusProxy* service_proxy_for_uuid(OmniBlePlugin* self,
                                          const gchar* device_path,
                                          const gchar* service_uuid) {
  if (self->object_manager == nullptr || device_path == nullptr ||
      service_uuid == nullptr) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattServiceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* candidate = G_DBUS_PROXY(interface_);
    const gchar* candidate_path = g_dbus_proxy_get_object_path(candidate);
    if (!object_path_is_child_of(candidate_path, device_path) ||
        g_strcmp0(candidate_path, device_path) == 0) {
      continue;
    }

    g_autofree gchar* uuid = proxy_property_string_dup(candidate, "UUID");
    if (normalize_uuid(uuid) == normalize_uuid(service_uuid)) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);
  return proxy;
}

static GDBusProxy* characteristic_proxy_for_uuid(OmniBlePlugin* self,
                                                 const gchar* device_path,
                                                 const gchar* service_uuid,
                                                 const gchar* characteristic_uuid) {
  g_autoptr(GDBusProxy) service_proxy =
      service_proxy_for_uuid(self, device_path, service_uuid);
  if (service_proxy == nullptr) {
    return nullptr;
  }

  const gchar* service_path = g_dbus_proxy_get_object_path(service_proxy);
  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattCharacteristicInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* candidate = G_DBUS_PROXY(interface_);
    const gchar* candidate_path = g_dbus_proxy_get_object_path(candidate);
    if (!object_path_is_child_of(candidate_path, service_path) ||
        g_strcmp0(candidate_path, service_path) == 0) {
      continue;
    }

    g_autofree gchar* uuid = proxy_property_string_dup(candidate, "UUID");
    if (normalize_uuid(uuid) == normalize_uuid(characteristic_uuid)) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);
  return proxy;
}

static GDBusProxy* descriptor_proxy_for_uuid(OmniBlePlugin* self,
                                             const gchar* device_path,
                                             const gchar* service_uuid,
                                             const gchar* characteristic_uuid,
                                             const gchar* descriptor_uuid) {
  g_autoptr(GDBusProxy) characteristic_proxy = characteristic_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid);
  if (characteristic_proxy == nullptr) {
    return nullptr;
  }

  const gchar* characteristic_path =
      g_dbus_proxy_get_object_path(characteristic_proxy);
  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  GDBusProxy* proxy = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattDescriptorInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* candidate = G_DBUS_PROXY(interface_);
    const gchar* candidate_path = g_dbus_proxy_get_object_path(candidate);
    if (!object_path_is_child_of(candidate_path, characteristic_path) ||
        g_strcmp0(candidate_path, characteristic_path) == 0) {
      continue;
    }

    g_autofree gchar* uuid = proxy_property_string_dup(candidate, "UUID");
    if (normalize_uuid(uuid) == normalize_uuid(descriptor_uuid)) {
      proxy = G_DBUS_PROXY(g_object_ref(interface_));
      break;
    }
  }

  g_list_free_full(objects, g_object_unref);
  return proxy;
}

static GVariant* empty_dict_variant() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  return g_variant_builder_end(&builder);
}

static GVariant* byte_array_variant_from_fl_value(FlValue* value) {
  g_autoptr(GByteArray) bytes = g_byte_array_new();
  if (value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_LIST) {
    const size_t length = fl_value_get_length(value);
    for (size_t index = 0; index < length; index++) {
      FlValue* item = fl_value_get_list_value(value, index);
      if (fl_value_get_type(item) == FL_VALUE_TYPE_INT) {
        guint8 byte = static_cast<guint8>(fl_value_get_int(item));
        g_byte_array_append(bytes, &byte, 1);
      }
    }
  }
  return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes->data, bytes->len,
                                   sizeof(guint8));
}

static GVariant* write_options_variant(const gchar* write_type) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  if (write_type != nullptr && strlen(write_type) > 0) {
    g_variant_builder_add(&builder, "{sv}", "type",
                          g_variant_new_string(write_type));
  }
  return g_variant_builder_end(&builder);
}

static gchar* device_id_for_object_path(OmniBlePlugin* self,
                                        const gchar* object_path) {
  if (self->object_manager == nullptr || object_path == nullptr) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  gchar* device_id = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kDeviceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* device_proxy = G_DBUS_PROXY(interface_);
    const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
    if (!object_path_is_child_of(object_path, device_path)) {
      continue;
    }

    device_id = proxy_property_string_dup(device_proxy, "Address");
    break;
  }

  g_list_free_full(objects, g_object_unref);
  return device_id;
}

static gchar* service_uuid_for_characteristic_path(OmniBlePlugin* self,
                                                   const gchar* characteristic_path) {
  if (self->object_manager == nullptr || characteristic_path == nullptr) {
    return nullptr;
  }

  GList* objects = g_dbus_object_manager_get_objects(self->object_manager);
  gchar* service_uuid = nullptr;
  for (GList* element = objects; element != nullptr; element = element->next) {
    GDBusObject* object = G_DBUS_OBJECT(element->data);
    g_autoptr(GDBusInterface) interface_ =
        g_dbus_object_get_interface(object, kGattServiceInterface);
    if (interface_ == nullptr) {
      continue;
    }

    GDBusProxy* service_proxy = G_DBUS_PROXY(interface_);
    const gchar* service_path = g_dbus_proxy_get_object_path(service_proxy);
    if (!object_path_is_child_of(characteristic_path, service_path) ||
        g_strcmp0(characteristic_path, service_path) == 0) {
      continue;
    }

    service_uuid = proxy_property_string_dup(service_proxy, "UUID");
    break;
  }

  g_list_free_full(objects, g_object_unref);
  return service_uuid;
}

static guint lookup_uint_variant_option(GVariant* options,
                                        const gchar* key,
                                        guint default_value) {
  if (options == nullptr || !g_variant_is_of_type(options, G_VARIANT_TYPE("a{sv}"))) {
    return default_value;
  }

  guint32 value = 0;
  if (g_variant_lookup(options, key, "u", &value)) {
    return value;
  }

  guint16 short_value = 0;
  if (g_variant_lookup(options, key, "q", &short_value)) {
    return short_value;
  }

  return default_value;
}

static gboolean lookup_bool_variant_option(GVariant* options,
                                           const gchar* key,
                                           gboolean default_value) {
  if (options == nullptr || !g_variant_is_of_type(options, G_VARIANT_TYPE("a{sv}"))) {
    return default_value;
  }

  gboolean value = FALSE;
  if (g_variant_lookup(options, key, "b", &value)) {
    return value;
  }
  return default_value;
}

static gchar* lookup_string_variant_option(GVariant* options, const gchar* key) {
  if (options == nullptr || !g_variant_is_of_type(options, G_VARIANT_TYPE("a{sv}"))) {
    return nullptr;
  }

  g_autoptr(GVariant) value = g_variant_lookup_value(options, key, nullptr);
  if (value == nullptr) {
    return nullptr;
  }
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) &&
      !g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
    return nullptr;
  }
  return g_variant_dup_string(value, nullptr);
}

static void emit_server_read_request(OmniBlePlugin* self,
                                     const gchar* request_id,
                                     const gchar* device_id,
                                     OmniBleServerCharacteristic* characteristic,
                                     guint offset) {
  if (!self->event_listening || characteristic == nullptr) {
    return;
  }

  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type", fl_value_new_string("readRequest"));
  fl_value_set_string_take(event, "requestId", fl_value_new_string(request_id));
  if (device_id != nullptr && strlen(device_id) > 0) {
    fl_value_set_string_take(event, "deviceId", fl_value_new_string(device_id));
  }
  fl_value_set_string_take(event, "serviceUuid",
                           fl_value_new_string(characteristic->service_uuid));
  fl_value_set_string_take(event, "characteristicUuid",
                           fl_value_new_string(characteristic->uuid));
  fl_value_set_string_take(event, "offset", fl_value_new_int(offset));
  send_event(self, event);
}

static void emit_server_write_request(OmniBlePlugin* self,
                                      const gchar* request_id,
                                      const gchar* device_id,
                                      OmniBleServerCharacteristic* characteristic,
                                      guint offset,
                                      const GByteArray* value,
                                      gboolean response_needed,
                                      gboolean prepared_write) {
  if (!self->event_listening || characteristic == nullptr) {
    return;
  }

  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type", fl_value_new_string("writeRequest"));
  fl_value_set_string_take(event, "requestId", fl_value_new_string(request_id));
  if (device_id != nullptr && strlen(device_id) > 0) {
    fl_value_set_string_take(event, "deviceId", fl_value_new_string(device_id));
  }
  fl_value_set_string_take(event, "serviceUuid",
                           fl_value_new_string(characteristic->service_uuid));
  fl_value_set_string_take(event, "characteristicUuid",
                           fl_value_new_string(characteristic->uuid));
  fl_value_set_string_take(event, "offset", fl_value_new_int(offset));
  fl_value_set_string_take(event, "preparedWrite",
                           fl_value_new_bool(prepared_write));
  fl_value_set_string_take(event, "responseNeeded",
                           fl_value_new_bool(response_needed));
  fl_value_set_string_take(event, "value", fl_value_from_byte_array(value));
  send_event(self, event);
}

static void emit_server_subscription_changed(OmniBlePlugin* self,
                                             OmniBleServerCharacteristic* characteristic,
                                             gboolean subscribed) {
  if (!self->event_listening || characteristic == nullptr) {
    return;
  }

  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type",
                           fl_value_new_string("subscriptionChanged"));
  fl_value_set_string_take(event, "serviceUuid",
                           fl_value_new_string(characteristic->service_uuid));
  fl_value_set_string_take(event, "characteristicUuid",
                           fl_value_new_string(characteristic->uuid));
  fl_value_set_string_take(event, "subscribed",
                           fl_value_new_bool(subscribed));
  send_event(self, event);
}

static void emit_notification_queue_ready(OmniBlePlugin* self, gint status) {
  if (!self->event_listening) {
    return;
  }

  g_autoptr(FlValue) event = fl_value_new_map();
  fl_value_set_string_take(event, "type",
                           fl_value_new_string("notificationQueueReady"));
  fl_value_set_string_take(event, "status", fl_value_new_int(status));
  send_event(self, event);
}

static GVariant* empty_object_path_array_variant() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));
  return g_variant_builder_end(&builder);
}

static GVariant* manufacturer_data_variant(const GByteArray* value) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{qv}"));
  if (value != nullptr && value->len >= 2) {
    const guint16 company_id = static_cast<guint16>(value->data[0]) |
        (static_cast<guint16>(value->data[1]) << 8);
    auto* payload = g_byte_array_new();
    if (value->len > 2) {
      g_byte_array_append(payload, value->data + 2, value->len - 2);
    }
    g_variant_builder_add(&builder, "{qv}", company_id,
                          byte_array_variant_from_bytes(payload));
    g_byte_array_unref(payload);
  }
  return g_variant_builder_end(&builder);
}

static GVariant* service_data_variant(GHashTable* values) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  if (values != nullptr) {
    GHashTableIter iter;
    gpointer key = nullptr;
    gpointer value = nullptr;
    g_hash_table_iter_init(&iter, values);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      g_variant_builder_add(
          &builder, "{sv}", static_cast<const gchar*>(key),
          byte_array_variant_from_bytes(static_cast<GByteArray*>(value)));
    }
  }
  return g_variant_builder_end(&builder);
}

static GVariant* service_properties_variant(OmniBleServerService* service) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "UUID",
                        g_variant_new_string(service->uuid));
  g_variant_builder_add(&builder, "{sv}", "Primary",
                        g_variant_new_boolean(service->primary));
  g_variant_builder_add(&builder, "{sv}", "Includes",
                        empty_object_path_array_variant());
  return g_variant_builder_end(&builder);
}

static GVariant* characteristic_properties_variant(
    OmniBleServerCharacteristic* characteristic) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "UUID",
                        g_variant_new_string(characteristic->uuid));
  g_variant_builder_add(&builder, "{sv}", "Service",
                        g_variant_new_object_path(characteristic->service_path));
  g_variant_builder_add(&builder, "{sv}", "Value",
                        byte_array_variant_from_bytes(characteristic->value));
  g_variant_builder_add(&builder, "{sv}", "Notifying",
                        g_variant_new_boolean(characteristic->notifying));
  g_variant_builder_add(&builder, "{sv}", "Flags",
                        string_array_variant_from_ptr_array(
                            characteristic->flags));
  return g_variant_builder_end(&builder);
}

static GVariant* descriptor_properties_variant(
    OmniBleServerDescriptor* descriptor) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "UUID",
                        g_variant_new_string(descriptor->uuid));
  g_variant_builder_add(
      &builder, "{sv}", "Characteristic",
      g_variant_new_object_path(descriptor->characteristic_path));
  g_variant_builder_add(&builder, "{sv}", "Value",
                        byte_array_variant_from_bytes(descriptor->value));
  g_variant_builder_add(&builder, "{sv}", "Flags",
                        string_array_variant_from_ptr_array(descriptor->flags));
  return g_variant_builder_end(&builder);
}

static GVariant* advertisement_properties_variant(OmniBlePlugin* self) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(
      &builder, "{sv}", "Type",
      g_variant_new_string(self->advertisement_connectable ? "peripheral"
                                                           : "broadcast"));
  g_variant_builder_add(
      &builder, "{sv}", "ServiceUUIDs",
      string_array_variant_from_ptr_array(self->advertisement_service_uuids));
  if (self->advertisement_manufacturer_data != nullptr &&
      self->advertisement_manufacturer_data->len >= 2) {
    g_variant_builder_add(
        &builder, "{sv}", "ManufacturerData",
        manufacturer_data_variant(self->advertisement_manufacturer_data));
  }
  if (self->advertisement_service_data != nullptr &&
      g_hash_table_size(self->advertisement_service_data) > 0) {
    g_variant_builder_add(&builder, "{sv}", "ServiceData",
                          service_data_variant(self->advertisement_service_data));
  }
  if (self->advertisement_local_name != nullptr &&
      strlen(self->advertisement_local_name) > 0) {
    g_variant_builder_add(&builder, "{sv}", "LocalName",
                          g_variant_new_string(self->advertisement_local_name));
  }
  if (self->advertisement_include_tx_power) {
    auto* includes = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(includes, g_strdup("tx-power"));
    g_variant_builder_add(&builder, "{sv}", "Includes",
                          string_array_variant_from_ptr_array(includes));
    g_ptr_array_unref(includes);
  }
  return g_variant_builder_end(&builder);
}

static GVariant* managed_objects_variant(OmniBlePlugin* self) {
  GVariantBuilder objects_builder;
  g_variant_builder_init(&objects_builder, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

  if (self->server_services != nullptr) {
    for (guint service_index = 0; service_index < self->server_services->len;
         service_index++) {
      auto* service = static_cast<OmniBleServerService*>(
          g_ptr_array_index(self->server_services, service_index));
      GVariantBuilder service_interfaces;
      g_variant_builder_init(&service_interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      g_variant_builder_add(&service_interfaces, "{sa{sv}}",
                            kGattServiceInterface,
                            service_properties_variant(service));
      g_autoptr(GVariant) service_interfaces_value =
          g_variant_builder_end(&service_interfaces);
      g_variant_builder_add(&objects_builder, "{oa{sa{sv}}}", service->path,
                            service_interfaces_value);

      for (guint characteristic_index = 0;
           characteristic_index < service->characteristics->len;
           characteristic_index++) {
        auto* characteristic = static_cast<OmniBleServerCharacteristic*>(
            g_ptr_array_index(service->characteristics, characteristic_index));
        GVariantBuilder characteristic_interfaces;
        g_variant_builder_init(&characteristic_interfaces,
                               G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&characteristic_interfaces, "{sa{sv}}",
                              kGattCharacteristicInterface,
                              characteristic_properties_variant(characteristic));
        g_autoptr(GVariant) characteristic_interfaces_value =
            g_variant_builder_end(&characteristic_interfaces);
        g_variant_builder_add(&objects_builder, "{oa{sa{sv}}}",
                              characteristic->path,
                              characteristic_interfaces_value);

        for (guint descriptor_index = 0;
             descriptor_index < characteristic->descriptors->len;
             descriptor_index++) {
          auto* descriptor = static_cast<OmniBleServerDescriptor*>(
              g_ptr_array_index(characteristic->descriptors, descriptor_index));
          GVariantBuilder descriptor_interfaces;
          g_variant_builder_init(&descriptor_interfaces,
                                 G_VARIANT_TYPE("a{sa{sv}}"));
          g_variant_builder_add(&descriptor_interfaces, "{sa{sv}}",
                                kGattDescriptorInterface,
                                descriptor_properties_variant(descriptor));
          g_autoptr(GVariant) descriptor_interfaces_value =
              g_variant_builder_end(&descriptor_interfaces);
          g_variant_builder_add(&objects_builder, "{oa{sa{sv}}}",
                                descriptor->path,
                                descriptor_interfaces_value);
        }
      }
    }
  }

  return g_variant_builder_end(&objects_builder);
}

static GVariant* server_service_property_get(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  if (self->server_services == nullptr) {
    return nullptr;
  }

  for (guint index = 0; index < self->server_services->len; index++) {
    auto* service = static_cast<OmniBleServerService*>(
        g_ptr_array_index(self->server_services, index));
    if (g_strcmp0(service->path, object_path) != 0) {
      continue;
    }

    if (g_strcmp0(property_name, "UUID") == 0) {
      return g_variant_new_string(service->uuid);
    }
    if (g_strcmp0(property_name, "Primary") == 0) {
      return g_variant_new_boolean(service->primary);
    }
    if (g_strcmp0(property_name, "Includes") == 0) {
      return empty_object_path_array_variant();
    }
  }

  return nullptr;
}

static GVariant* server_characteristic_property_get(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  auto* characteristic = server_characteristic_for_path(self, object_path);
  if (characteristic == nullptr) {
    return nullptr;
  }

  if (g_strcmp0(property_name, "UUID") == 0) {
    return g_variant_new_string(characteristic->uuid);
  }
  if (g_strcmp0(property_name, "Service") == 0) {
    return g_variant_new_object_path(characteristic->service_path);
  }
  if (g_strcmp0(property_name, "Value") == 0) {
    return byte_array_variant_from_bytes(characteristic->value);
  }
  if (g_strcmp0(property_name, "Notifying") == 0) {
    return g_variant_new_boolean(characteristic->notifying);
  }
  if (g_strcmp0(property_name, "Flags") == 0) {
    return string_array_variant_from_ptr_array(characteristic->flags);
  }

  return nullptr;
}

static GVariant* server_descriptor_property_get(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  auto* descriptor = server_descriptor_for_path(self, object_path);
  if (descriptor == nullptr) {
    return nullptr;
  }

  if (g_strcmp0(property_name, "UUID") == 0) {
    return g_variant_new_string(descriptor->uuid);
  }
  if (g_strcmp0(property_name, "Characteristic") == 0) {
    return g_variant_new_object_path(descriptor->characteristic_path);
  }
  if (g_strcmp0(property_name, "Value") == 0) {
    return byte_array_variant_from_bytes(descriptor->value);
  }
  if (g_strcmp0(property_name, "Flags") == 0) {
    return string_array_variant_from_ptr_array(descriptor->flags);
  }

  return nullptr;
}

static GVariant* advertisement_property_get(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  if (g_strcmp0(object_path, kAdvertisementPath) != 0) {
    return nullptr;
  }

  if (g_strcmp0(property_name, "Type") == 0) {
    return g_variant_new_string(self->advertisement_connectable ? "peripheral"
                                                                : "broadcast");
  }
  if (g_strcmp0(property_name, "ServiceUUIDs") == 0) {
    return string_array_variant_from_ptr_array(self->advertisement_service_uuids);
  }
  if (g_strcmp0(property_name, "ManufacturerData") == 0) {
    return manufacturer_data_variant(self->advertisement_manufacturer_data);
  }
  if (g_strcmp0(property_name, "ServiceData") == 0) {
    return service_data_variant(self->advertisement_service_data);
  }
  if (g_strcmp0(property_name, "LocalName") == 0) {
    return g_variant_new_string(self->advertisement_local_name != nullptr
                                    ? self->advertisement_local_name
                                    : "");
  }
  if (g_strcmp0(property_name, "Includes") == 0) {
    auto* includes = g_ptr_array_new_with_free_func(g_free);
    if (self->advertisement_include_tx_power) {
      g_ptr_array_add(includes, g_strdup("tx-power"));
    }
    g_autoptr(GPtrArray) owned_includes = includes;
    return string_array_variant_from_ptr_array(owned_includes);
  }

  return nullptr;
}

static void server_object_manager_method_call(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  if (g_strcmp0(method_name, "GetManagedObjects") != 0) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod",
        "Unknown ObjectManager method.");
    return;
  }

  g_dbus_method_invocation_return_value(
      invocation,
      g_variant_new("(@a{oa{sa{sv}}})", managed_objects_variant(self)));
}

static void server_characteristic_method_call(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  auto* characteristic = server_characteristic_for_path(self, object_path);
  if (characteristic == nullptr) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.Failed",
        "Characteristic was not found.");
    return;
  }

  if (g_strcmp0(method_name, "ReadValue") == 0) {
    if (!ptr_array_contains_string(characteristic->flags, "read") &&
        !ptr_array_contains_string(characteristic->flags, "encrypt-read")) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.NotPermitted",
          "Characteristic does not support reading.");
      return;
    }

    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
    auto* request = g_new0(PendingServerReadRequest, 1);
    request->request_id = next_request_id(self, "read");
    request->invocation = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
    request->characteristic = characteristic;
    request->offset = lookup_uint_variant_option(options, "offset", 0);
    g_autofree gchar* device_path = lookup_string_variant_option(options, "device");
    request->device_id = device_id_for_object_path(self, device_path);
    g_hash_table_insert(self->pending_server_read_requests,
                        g_strdup(request->request_id), request);
    emit_server_read_request(self, request->request_id, request->device_id,
                             characteristic, request->offset);
    return;
  }

  if (g_strcmp0(method_name, "WriteValue") == 0) {
    if (!ptr_array_contains_string(characteristic->flags, "write") &&
        !ptr_array_contains_string(characteristic->flags, "encrypt-write") &&
        !ptr_array_contains_string(characteristic->flags,
                                   "write-without-response")) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.NotPermitted",
          "Characteristic does not support writing.");
      return;
    }

    g_autoptr(GVariant) value = g_variant_get_child_value(parameters, 0);
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 1);
    auto* request = g_new0(PendingServerWriteRequest, 1);
    request->request_id = next_request_id(self, "write");
    request->invocation = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
    request->characteristic = characteristic;
    request->offset = lookup_uint_variant_option(options, "offset", 0);
    request->value = byte_array_from_variant(value);
    request->prepared_write =
        lookup_bool_variant_option(options, "prepare-authorize", FALSE);
    g_autofree gchar* type = lookup_string_variant_option(options, "type");
    request->response_needed = g_strcmp0(type, "command") != 0;
    g_autofree gchar* device_path = lookup_string_variant_option(options, "device");
    request->device_id = device_id_for_object_path(self, device_path);
    g_hash_table_insert(self->pending_server_write_requests,
                        g_strdup(request->request_id), request);
    emit_server_write_request(self, request->request_id, request->device_id,
                              characteristic, request->offset, request->value,
                              request->response_needed,
                              request->prepared_write);
    return;
  }

  if (g_strcmp0(method_name, "StartNotify") == 0) {
    if (!characteristic->notifying) {
      characteristic->notifying = TRUE;
      GVariantBuilder changed_builder;
      g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&changed_builder, "{sv}", "Notifying",
                            g_variant_new_boolean(TRUE));
      g_autoptr(GVariant) changed = g_variant_builder_end(&changed_builder);
      emit_properties_changed(self, characteristic->path,
                              kGattCharacteristicInterface, changed);
      emit_server_subscription_changed(self, characteristic, TRUE);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  if (g_strcmp0(method_name, "StopNotify") == 0) {
    if (characteristic->notifying) {
      characteristic->notifying = FALSE;
      GVariantBuilder changed_builder;
      g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&changed_builder, "{sv}", "Notifying",
                            g_variant_new_boolean(FALSE));
      g_autoptr(GVariant) changed = g_variant_builder_end(&changed_builder);
      emit_properties_changed(self, characteristic->path,
                              kGattCharacteristicInterface, changed);
      emit_server_subscription_changed(self, characteristic, FALSE);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod",
      "Unknown characteristic method.");
}

static void server_descriptor_method_call(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  auto* descriptor = server_descriptor_for_path(self, object_path);
  if (descriptor == nullptr) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.Failed", "Descriptor was not found.");
    return;
  }

  if (g_strcmp0(method_name, "ReadValue") == 0) {
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
    const guint offset = lookup_uint_variant_option(options, "offset", 0);
    if (offset > descriptor->value->len) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.InvalidOffset",
          "Descriptor read offset is out of range.");
      return;
    }
    auto* sliced = g_byte_array_new();
    if (offset < descriptor->value->len) {
      g_byte_array_append(sliced, descriptor->value->data + offset,
                          descriptor->value->len - offset);
    }
    g_autoptr(GByteArray) owned_sliced = sliced;
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@ay)", byte_array_variant_from_bytes(owned_sliced)));
    return;
  }

  if (g_strcmp0(method_name, "WriteValue") == 0) {
    g_autoptr(GVariant) value = g_variant_get_child_value(parameters, 0);
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 1);
    const guint offset = lookup_uint_variant_option(options, "offset", 0);
    g_autoptr(GByteArray) incoming = byte_array_from_variant(value);
    if (!merge_byte_array_value(descriptor->value, offset, incoming)) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.InvalidOffset",
          "Descriptor write offset is out of range.");
      return;
    }

    GVariantBuilder changed_builder;
    g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed_builder, "{sv}", "Value",
                          byte_array_variant_from_bytes(descriptor->value));
    g_autoptr(GVariant) changed = g_variant_builder_end(&changed_builder);
    emit_properties_changed(self, descriptor->path, kGattDescriptorInterface,
                            changed);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod",
      "Unknown descriptor method.");
}

static void advertisement_method_call(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data) {
  auto* self = static_cast<OmniBlePlugin*>(user_data);
  if (g_strcmp0(method_name, "Release") == 0) {
    self->advertisement_registered = FALSE;
    self->is_advertising = FALSE;
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }

  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod",
      "Unknown advertisement method.");
}

static const GDBusInterfaceVTable kObjectManagerVTable = {
    server_object_manager_method_call, nullptr, nullptr};
static const GDBusInterfaceVTable kGattCharacteristicVTable = {
    server_characteristic_method_call, server_characteristic_property_get,
    nullptr};
static const GDBusInterfaceVTable kGattServiceVTable = {
    nullptr, server_service_property_get, nullptr};
static const GDBusInterfaceVTable kGattDescriptorVTable = {
    server_descriptor_method_call, server_descriptor_property_get, nullptr};
static const GDBusInterfaceVTable kAdvertisementVTable = {
    advertisement_method_call, advertisement_property_get, nullptr};

static void reset_advertisement_state(OmniBlePlugin* self) {
  g_clear_pointer(&self->advertisement_local_name, g_free);
  self->advertisement_connectable = TRUE;
  self->advertisement_include_tx_power = FALSE;

  if (self->advertisement_service_uuids != nullptr) {
    g_ptr_array_set_size(self->advertisement_service_uuids, 0);
  }
  if (self->advertisement_service_data != nullptr) {
    g_hash_table_remove_all(self->advertisement_service_data);
  }
  g_clear_pointer(&self->advertisement_manufacturer_data, g_byte_array_unref);
}

static void fail_pending_server_requests(OmniBlePlugin* self, const gchar* message) {
  if (self->pending_server_read_requests != nullptr) {
    GHashTableIter iter;
    gpointer key = nullptr;
    gpointer value = nullptr;
    g_hash_table_iter_init(&iter, self->pending_server_read_requests);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      auto* request = static_cast<PendingServerReadRequest*>(value);
      if (request->invocation != nullptr) {
        g_dbus_method_invocation_return_dbus_error(
            request->invocation, "org.bluez.Error.Failed",
            message != nullptr ? message : "GATT server was reset.");
      }
    }
    g_hash_table_remove_all(self->pending_server_read_requests);
  }

  if (self->pending_server_write_requests != nullptr) {
    GHashTableIter iter;
    gpointer key = nullptr;
    gpointer value = nullptr;
    g_hash_table_iter_init(&iter, self->pending_server_write_requests);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      auto* request = static_cast<PendingServerWriteRequest*>(value);
      if (request->invocation != nullptr) {
        g_dbus_method_invocation_return_dbus_error(
            request->invocation, "org.bluez.Error.Failed",
            message != nullptr ? message : "GATT server was reset.");
      }
    }
    g_hash_table_remove_all(self->pending_server_write_requests);
  }
}

static void unregister_gatt_application(OmniBlePlugin* self) {
  if (!self->app_registered) {
    return;
  }

  g_autoptr(GDBusProxy) manager = gatt_manager_proxy(self);
  if (manager != nullptr) {
    g_autoptr(GError) error = nullptr;
    g_dbus_proxy_call_sync(manager, "UnregisterApplication",
                           g_variant_new("(o)", kApplicationPath),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  }
  self->app_registered = FALSE;
}

static void unregister_advertisement(OmniBlePlugin* self) {
  if (!self->advertisement_registered) {
    return;
  }

  g_autoptr(GDBusProxy) manager = advertising_manager_proxy(self);
  if (manager != nullptr) {
    g_autoptr(GError) error = nullptr;
    g_dbus_proxy_call_sync(manager, "UnregisterAdvertisement",
                           g_variant_new("(o)", kAdvertisementPath),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  }
  self->advertisement_registered = FALSE;
  self->is_advertising = FALSE;
}

static void unregister_server_objects(OmniBlePlugin* self) {
  if (self->server_connection == nullptr) {
    return;
  }

  if (self->advertisement_registration_id != 0) {
    g_dbus_connection_unregister_object(self->server_connection,
                                        self->advertisement_registration_id);
    self->advertisement_registration_id = 0;
  }

  if (self->server_services != nullptr) {
    for (guint service_index = 0; service_index < self->server_services->len;
         service_index++) {
      auto* service = static_cast<OmniBleServerService*>(
          g_ptr_array_index(self->server_services, service_index));
      for (guint characteristic_index = 0;
           characteristic_index < service->characteristics->len;
           characteristic_index++) {
        auto* characteristic = static_cast<OmniBleServerCharacteristic*>(
            g_ptr_array_index(service->characteristics, characteristic_index));
        for (guint descriptor_index = 0;
             descriptor_index < characteristic->descriptors->len;
             descriptor_index++) {
          auto* descriptor = static_cast<OmniBleServerDescriptor*>(
              g_ptr_array_index(characteristic->descriptors, descriptor_index));
          if (descriptor->registration_id != 0) {
            g_dbus_connection_unregister_object(self->server_connection,
                                                descriptor->registration_id);
            descriptor->registration_id = 0;
          }
        }
        if (characteristic->registration_id != 0) {
          g_dbus_connection_unregister_object(self->server_connection,
                                              characteristic->registration_id);
          characteristic->registration_id = 0;
        }
      }
      if (service->registration_id != 0) {
        g_dbus_connection_unregister_object(self->server_connection,
                                            service->registration_id);
        service->registration_id = 0;
      }
    }
  }

  if (self->app_registration_id != 0) {
    g_dbus_connection_unregister_object(self->server_connection,
                                        self->app_registration_id);
    self->app_registration_id = 0;
  }
}

static void clear_server_database_state(OmniBlePlugin* self) {
  unregister_advertisement(self);
  unregister_gatt_application(self);
  fail_pending_server_requests(self, "GATT server was reset.");
  unregister_server_objects(self);

  if (self->server_services != nullptr) {
    g_ptr_array_set_size(self->server_services, 0);
  }
  if (self->server_characteristics != nullptr) {
    g_hash_table_remove_all(self->server_characteristics);
  }
  self->next_request_id = 1;
}

static gboolean register_server_objects(OmniBlePlugin* self, GError** error) {
  if (!ensure_server_connection(self, error)) {
    return FALSE;
  }

  self->app_registration_id = g_dbus_connection_register_object(
      self->server_connection, kApplicationPath,
      object_manager_node_info()->interfaces[0], &kObjectManagerVTable, self,
      nullptr, error);
  if (self->app_registration_id == 0) {
    return FALSE;
  }

  for (guint service_index = 0; service_index < self->server_services->len;
       service_index++) {
    auto* service = static_cast<OmniBleServerService*>(
        g_ptr_array_index(self->server_services, service_index));
    service->registration_id = g_dbus_connection_register_object(
        self->server_connection, service->path,
        gatt_service_node_info()->interfaces[0], &kGattServiceVTable, self,
        nullptr, error);
    if (service->registration_id == 0) {
      return FALSE;
    }

    for (guint characteristic_index = 0;
         characteristic_index < service->characteristics->len;
         characteristic_index++) {
      auto* characteristic = static_cast<OmniBleServerCharacteristic*>(
          g_ptr_array_index(service->characteristics, characteristic_index));
      characteristic->registration_id = g_dbus_connection_register_object(
          self->server_connection, characteristic->path,
          gatt_characteristic_node_info()->interfaces[0],
          &kGattCharacteristicVTable, self, nullptr, error);
      if (characteristic->registration_id == 0) {
        return FALSE;
      }

      for (guint descriptor_index = 0;
           descriptor_index < characteristic->descriptors->len;
           descriptor_index++) {
        auto* descriptor = static_cast<OmniBleServerDescriptor*>(
            g_ptr_array_index(characteristic->descriptors, descriptor_index));
        descriptor->registration_id = g_dbus_connection_register_object(
            self->server_connection, descriptor->path,
            gatt_descriptor_node_info()->interfaces[0],
            &kGattDescriptorVTable, self, nullptr, error);
        if (descriptor->registration_id == 0) {
          return FALSE;
        }
      }
    }
  }

  g_autoptr(GDBusProxy) manager = gatt_manager_proxy(self);
  if (manager == nullptr) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "BlueZ GATT manager is unavailable.");
    return FALSE;
  }

  GVariantBuilder options_builder;
  g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = g_variant_builder_end(&options_builder);
  if (g_dbus_proxy_call_sync(manager, "RegisterApplication",
                             g_variant_new("(o@a{sv})", kApplicationPath,
                                           options),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, error) ==
      nullptr) {
    return FALSE;
  }

  self->app_registered = TRUE;
  return TRUE;
}

static gboolean build_server_database(OmniBlePlugin* self,
                                      FlValue* arguments,
                                      GError** error) {
  if (arguments == nullptr || fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A valid GATT database payload is required.");
    return FALSE;
  }

  FlValue* services_value = fl_value_lookup_string(arguments, "services");
  if (services_value == nullptr || fl_value_get_type(services_value) != FL_VALUE_TYPE_LIST) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A valid services list is required.");
    return FALSE;
  }

  const size_t service_count = fl_value_get_length(services_value);
  for (size_t service_index = 0; service_index < service_count; service_index++) {
    FlValue* service_value = fl_value_get_list_value(services_value, service_index);
    if (fl_value_get_type(service_value) != FL_VALUE_TYPE_MAP) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Each published service must be a map payload.");
      return FALSE;
    }

    const gchar* service_uuid_raw = lookup_string_argument(service_value, "uuid");
    if (service_uuid_raw == nullptr) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Each published service must include a UUID.");
      return FALSE;
    }

    auto* service = g_new0(OmniBleServerService, 1);
    service->uuid = g_strdup(normalize_uuid(service_uuid_raw).c_str());
    service->path = g_strdup_printf("%s/service%zu", kApplicationPath,
                                    service_index);
    FlValue* primary_value = fl_value_lookup_string(service_value, "primary");
    service->primary = primary_value != nullptr &&
            fl_value_get_type(primary_value) == FL_VALUE_TYPE_BOOL
        ? fl_value_get_bool(primary_value)
        : TRUE;
    service->characteristics =
        g_ptr_array_new_with_free_func(destroy_server_characteristic);

    FlValue* characteristics_value =
        fl_value_lookup_string(service_value, "characteristics");
    if (characteristics_value != nullptr &&
        fl_value_get_type(characteristics_value) == FL_VALUE_TYPE_LIST) {
      const size_t characteristic_count = fl_value_get_length(characteristics_value);
      for (size_t characteristic_index = 0;
           characteristic_index < characteristic_count; characteristic_index++) {
        FlValue* characteristic_value =
            fl_value_get_list_value(characteristics_value, characteristic_index);
        if (fl_value_get_type(characteristic_value) != FL_VALUE_TYPE_MAP) {
          g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Each characteristic must be a map payload.");
          destroy_server_service(service);
          return FALSE;
        }

        const gchar* characteristic_uuid_raw =
            lookup_string_argument(characteristic_value, "uuid");
        if (characteristic_uuid_raw == nullptr) {
          g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Each characteristic must include a UUID.");
          destroy_server_service(service);
          return FALSE;
        }

        auto* characteristic = g_new0(OmniBleServerCharacteristic, 1);
        characteristic->uuid =
            g_strdup(normalize_uuid(characteristic_uuid_raw).c_str());
        characteristic->path =
            g_strdup_printf("%s/char%zu", service->path, characteristic_index);
        characteristic->service_path = g_strdup(service->path);
        characteristic->service_uuid = g_strdup(service->uuid);
        characteristic->value = byte_array_from_fl_value(
            fl_value_lookup_string(characteristic_value, "initialValue"));
        characteristic->flags = server_characteristic_flags(
            fl_value_lookup_string(characteristic_value, "properties"),
            fl_value_lookup_string(characteristic_value, "permissions"));
        characteristic->descriptors =
            g_ptr_array_new_with_free_func(destroy_server_descriptor);

        FlValue* descriptors_value =
            fl_value_lookup_string(characteristic_value, "descriptors");
        if (descriptors_value != nullptr &&
            fl_value_get_type(descriptors_value) == FL_VALUE_TYPE_LIST) {
          const size_t descriptor_count = fl_value_get_length(descriptors_value);
          for (size_t descriptor_index = 0; descriptor_index < descriptor_count;
               descriptor_index++) {
            FlValue* descriptor_value =
                fl_value_get_list_value(descriptors_value, descriptor_index);
            if (fl_value_get_type(descriptor_value) != FL_VALUE_TYPE_MAP) {
              g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                  "Each descriptor must be a map payload.");
              destroy_server_service(service);
              return FALSE;
            }

            const gchar* descriptor_uuid_raw =
                lookup_string_argument(descriptor_value, "uuid");
            if (descriptor_uuid_raw == nullptr) {
              g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                  "Each descriptor must include a UUID.");
              destroy_server_service(service);
              return FALSE;
            }

            const std::string normalized_descriptor_uuid =
                normalize_uuid(descriptor_uuid_raw);
            if (normalized_descriptor_uuid ==
                "00002902-0000-1000-8000-00805f9b34fb") {
              continue;
            }

            auto* descriptor = g_new0(OmniBleServerDescriptor, 1);
            descriptor->uuid = g_strdup(normalized_descriptor_uuid.c_str());
            descriptor->path = g_strdup_printf("%s/desc%zu",
                                               characteristic->path,
                                               descriptor_index);
            descriptor->characteristic_path = g_strdup(characteristic->path);
            descriptor->value = byte_array_from_fl_value(
                fl_value_lookup_string(descriptor_value, "initialValue"));
            descriptor->flags = server_descriptor_flags(
                fl_value_lookup_string(descriptor_value, "permissions"));
            g_ptr_array_add(characteristic->descriptors, descriptor);
          }
        }

        g_ptr_array_add(service->characteristics, characteristic);
        g_autofree gchar* characteristic_key =
            server_characteristic_key(service->uuid, characteristic->uuid);
        g_hash_table_insert(self->server_characteristics,
                            g_strdup(characteristic_key), characteristic);
      }
    }

    g_ptr_array_add(self->server_services, service);
  }

  return TRUE;
}

static FlMethodResponse* publish_gatt_database(OmniBlePlugin* self,
                                               FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_server_connection(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  g_autoptr(GDBusProxy) adapter = adapter_proxy(self);
  if (adapter == nullptr || !proxy_property_bool(adapter, "Powered", FALSE)) {
    g_autoptr(FlValue) details = fl_value_new_map();
    fl_value_set_string_take(details, "state",
                             fl_value_new_string("poweredOff"));
    return error_response(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before publishing GATT services.",
        details);
  }

  g_autoptr(GDBusProxy) manager = gatt_manager_proxy(self);
  if (manager == nullptr) {
    return error_response(
        "unsupported",
        "BlueZ GATT server support is not available on this Linux device.");
  }

  clear_server_database_state(self);
  if (!build_server_database(self, arguments, &error)) {
    clear_server_database_state(self);
    return error_response("invalid-argument",
                          error != nullptr ? error->message
                                           : "Invalid GATT database payload.");
  }

  if (!register_server_objects(self, &error)) {
    clear_server_database_state(self);
    return error_response("publish-gatt-database-failed",
                          error != nullptr ? error->message
                                           : "Failed to publish the GATT database.");
  }

  return success_response();
}

static FlMethodResponse* clear_gatt_database_response(OmniBlePlugin* self) {
  clear_server_database_state(self);
  return success_response();
}

static FlMethodResponse* start_advertising(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_server_connection(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  g_autoptr(GDBusProxy) adapter = adapter_proxy(self);
  if (adapter == nullptr || !proxy_property_bool(adapter, "Powered", FALSE)) {
    g_autoptr(FlValue) details = fl_value_new_map();
    fl_value_set_string_take(details, "state",
                             fl_value_new_string("poweredOff"));
    return error_response("adapter-unavailable",
                          "Bluetooth adapter must be powered on before advertising.",
                          details);
  }

  g_autoptr(GDBusProxy) manager = advertising_manager_proxy(self);
  if (manager == nullptr) {
    return error_response(
        "unsupported",
        "BlueZ advertising support is not available on this Linux device.");
  }

  unregister_advertisement(self);
  if (self->advertisement_registration_id != 0 && self->server_connection != nullptr) {
    g_dbus_connection_unregister_object(self->server_connection,
                                        self->advertisement_registration_id);
    self->advertisement_registration_id = 0;
  }
  reset_advertisement_state(self);

  const gboolean connectable =
      arguments != nullptr && fl_value_get_type(arguments) == FL_VALUE_TYPE_MAP &&
              fl_value_lookup_string(arguments, "connectable") != nullptr
          ? fl_value_get_bool(fl_value_lookup_string(arguments, "connectable"))
          : TRUE;
  self->advertisement_connectable = connectable;
  if (connectable && !self->app_registered) {
    return error_response(
        "unavailable",
        "Publish a GATT database before starting connectable advertising on Linux.");
  }

  if (arguments != nullptr && fl_value_get_type(arguments) == FL_VALUE_TYPE_MAP) {
    if (FlValue* local_name = fl_value_lookup_string(arguments, "localName");
        local_name != nullptr &&
        fl_value_get_type(local_name) == FL_VALUE_TYPE_STRING) {
      self->advertisement_local_name = g_strdup(fl_value_get_string(local_name));
    }
    if (FlValue* include_tx_power =
            fl_value_lookup_string(arguments, "includeTxPowerLevel");
        include_tx_power != nullptr &&
        fl_value_get_type(include_tx_power) == FL_VALUE_TYPE_BOOL) {
      self->advertisement_include_tx_power = fl_value_get_bool(include_tx_power);
    }
    if (FlValue* service_uuids =
            fl_value_lookup_string(arguments, "serviceUuids");
        service_uuids != nullptr &&
        fl_value_get_type(service_uuids) == FL_VALUE_TYPE_LIST) {
      const size_t count = fl_value_get_length(service_uuids);
      for (size_t index = 0; index < count; index++) {
        FlValue* value = fl_value_get_list_value(service_uuids, index);
        if (fl_value_get_type(value) == FL_VALUE_TYPE_STRING) {
          g_ptr_array_add(self->advertisement_service_uuids,
                          g_strdup(normalize_uuid(fl_value_get_string(value)).c_str()));
        }
      }
    }

    if (self->advertisement_service_uuids->len == 0 && self->server_services != nullptr) {
      for (guint index = 0; index < self->server_services->len; index++) {
        auto* service = static_cast<OmniBleServerService*>(
            g_ptr_array_index(self->server_services, index));
        g_ptr_array_add(self->advertisement_service_uuids, g_strdup(service->uuid));
      }
    }

    if (FlValue* service_data =
            fl_value_lookup_string(arguments, "serviceData");
        service_data != nullptr &&
        fl_value_get_type(service_data) == FL_VALUE_TYPE_MAP) {
      const size_t count = fl_value_get_length(service_data);
      for (size_t index = 0; index < count; index++) {
        FlValue* key = fl_value_get_map_key(service_data, index);
        FlValue* value = fl_value_get_map_value(service_data, index);
        if (fl_value_get_type(key) != FL_VALUE_TYPE_STRING) {
          continue;
        }
        g_hash_table_insert(
            self->advertisement_service_data,
            g_strdup(normalize_uuid(fl_value_get_string(key)).c_str()),
            byte_array_from_fl_value(value));
      }
    }

    if (FlValue* manufacturer_data =
            fl_value_lookup_string(arguments, "manufacturerData");
        manufacturer_data != nullptr) {
      self->advertisement_manufacturer_data =
          byte_array_from_fl_value(manufacturer_data);
    }
  }

  self->advertisement_registration_id = g_dbus_connection_register_object(
      self->server_connection, kAdvertisementPath,
      advertisement_node_info()->interfaces[0], &kAdvertisementVTable, self,
      nullptr, &error);
  if (self->advertisement_registration_id == 0) {
    reset_advertisement_state(self);
    return error_response("advertise-failed",
                          error != nullptr ? error->message
                                           : "Failed to export advertisement.");
  }

  GVariantBuilder options_builder;
  g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) options = g_variant_builder_end(&options_builder);
  if (g_dbus_proxy_call_sync(manager, "RegisterAdvertisement",
                             g_variant_new("(o@a{sv})", kAdvertisementPath,
                                           options),
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error) ==
      nullptr) {
    g_dbus_connection_unregister_object(self->server_connection,
                                        self->advertisement_registration_id);
    self->advertisement_registration_id = 0;
    reset_advertisement_state(self);
    return error_response("advertise-failed",
                          error != nullptr ? error->message
                                           : "Failed to register advertisement.");
  }

  self->advertisement_registered = TRUE;
  self->is_advertising = TRUE;
  return success_response();
}

static FlMethodResponse* stop_advertising_response(OmniBlePlugin* self) {
  unregister_advertisement(self);
  if (self->advertisement_registration_id != 0 && self->server_connection != nullptr) {
    g_dbus_connection_unregister_object(self->server_connection,
                                        self->advertisement_registration_id);
    self->advertisement_registration_id = 0;
  }
  reset_advertisement_state(self);
  return success_response();
}

static FlMethodResponse* notify_characteristic_value(OmniBlePlugin* self,
                                                     FlValue* arguments) {
  if (arguments == nullptr || fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
    return error_response(
        "invalid-argument",
        "A characteristic address and value are required to notify subscribers.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  if (device_id != nullptr && strlen(device_id) > 0) {
    return error_response(
        "unsupported",
        "BlueZ notifications are broadcast to current subscribers and do not support per-device targeting.");
  }

  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  if (service_uuid == nullptr || characteristic_uuid == nullptr) {
    return error_response(
        "invalid-argument",
        "`serviceUuid` and `characteristicUuid` are required to notify subscribers.");
  }

  auto* characteristic = server_characteristic_for_key(
      self, service_uuid, characteristic_uuid);
  if (characteristic == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth characteristic was not found. Publish a GATT database first.");
  }

  if (!ptr_array_contains_string(characteristic->flags, "notify") &&
      !ptr_array_contains_string(characteristic->flags, "indicate")) {
    return error_response(
        "unsupported",
        "Bluetooth characteristic does not support notifications or indications.");
  }

  g_autoptr(GByteArray) value = byte_array_from_fl_value(
      fl_value_lookup_string(arguments, "value"));
  g_byte_array_set_size(characteristic->value, 0);
  if (value->len > 0) {
    g_byte_array_append(characteristic->value, value->data, value->len);
  }

  if (characteristic->notifying) {
    GVariantBuilder changed_builder;
    g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed_builder, "{sv}", "Value",
                          byte_array_variant_from_bytes(characteristic->value));
    g_autoptr(GVariant) changed = g_variant_builder_end(&changed_builder);
    emit_properties_changed(self, characteristic->path,
                            kGattCharacteristicInterface, changed);
    emit_notification_queue_ready(self, 0);
  }

  return success_response();
}

static FlMethodResponse* respond_to_read_request(OmniBlePlugin* self,
                                                 FlValue* arguments) {
  const gchar* request_id = lookup_string_argument(arguments, "requestId");
  if (request_id == nullptr) {
    return error_response("invalid-argument",
                          "`requestId` is required to answer a read request.");
  }

  auto* request = pending_server_read_request_for_id(self, request_id);
  if (request == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth read request was not found or has already been answered.");
  }

  g_autoptr(GByteArray) value = byte_array_from_fl_value(
      fl_value_lookup_string(arguments, "value"));
  g_byte_array_set_size(request->characteristic->value, 0);
  if (value->len > 0) {
    g_byte_array_append(request->characteristic->value, value->data, value->len);
  }

  if (request->offset > value->len) {
    g_dbus_method_invocation_return_dbus_error(
        request->invocation, "org.bluez.Error.InvalidOffset",
        "The provided value is shorter than the pending read offset.");
    g_hash_table_remove(self->pending_server_read_requests, request_id);
    return error_response(
        "invalid-argument",
        "The provided value is shorter than the pending read offset.");
  }

  auto* sliced = g_byte_array_new();
  if (request->offset < value->len) {
    g_byte_array_append(sliced, value->data + request->offset,
                        value->len - request->offset);
  }
  g_autoptr(GByteArray) owned_sliced = sliced;
  g_dbus_method_invocation_return_value(
      request->invocation,
      g_variant_new("(@ay)", byte_array_variant_from_bytes(owned_sliced)));
  g_hash_table_remove(self->pending_server_read_requests, request_id);
  return success_response();
}

static FlMethodResponse* respond_to_write_request(OmniBlePlugin* self,
                                                  FlValue* arguments) {
  const gchar* request_id = lookup_string_argument(arguments, "requestId");
  if (request_id == nullptr) {
    return error_response("invalid-argument",
                          "`requestId` is required to answer a write request.");
  }

  auto* request = pending_server_write_request_for_id(self, request_id);
  if (request == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth write request was not found or has already been answered.");
  }

  const gboolean accept =
      arguments != nullptr && fl_value_get_type(arguments) == FL_VALUE_TYPE_MAP &&
              fl_value_lookup_string(arguments, "accept") != nullptr
          ? fl_value_get_bool(fl_value_lookup_string(arguments, "accept"))
          : TRUE;
  if (!accept) {
    g_dbus_method_invocation_return_dbus_error(
        request->invocation, "org.bluez.Error.NotPermitted",
        "The write request was rejected.");
    g_hash_table_remove(self->pending_server_write_requests, request_id);
    return success_response();
  }

  if (!merge_byte_array_value(request->characteristic->value, request->offset,
                              request->value)) {
    g_dbus_method_invocation_return_dbus_error(
        request->invocation, "org.bluez.Error.InvalidOffset",
        "The pending write request used an unsupported offset.");
    g_hash_table_remove(self->pending_server_write_requests, request_id);
    return error_response(
        "invalid-argument",
        "The pending write request used an unsupported offset.");
  }

  GVariantBuilder changed_builder;
  g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&changed_builder, "{sv}", "Value",
                        byte_array_variant_from_bytes(request->characteristic->value));
  g_autoptr(GVariant) changed = g_variant_builder_end(&changed_builder);
  emit_properties_changed(self, request->characteristic->path,
                          kGattCharacteristicInterface, changed);

  g_dbus_method_invocation_return_value(request->invocation, nullptr);
  g_hash_table_remove(self->pending_server_write_requests, request_id);
  return success_response();
}

static FlMethodResponse* read_characteristic(OmniBlePlugin* self,
                                             FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  if (device_id == nullptr || service_uuid == nullptr ||
      characteristic_uuid == nullptr) {
    return error_response(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, and `characteristicUuid` are required to read a characteristic.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before reading a characteristic.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  g_autoptr(GDBusProxy) characteristic_proxy = characteristic_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid);
  if (characteristic_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before reading.");
  }

  g_autoptr(GVariant) options = empty_dict_variant();
  GVariant* call_children[] = {options};
  g_autoptr(GVariant) parameters = g_variant_new_tuple(call_children, 1);
  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(
      characteristic_proxy, "ReadValue", parameters, G_DBUS_CALL_FLAGS_NONE, -1,
      nullptr, &error);
  if (response == nullptr) {
    return error_response("read-failed",
                          error != nullptr ? error->message
                                           : "Bluetooth read failed on Linux.");
  }

  g_autoptr(GVariant) value = g_variant_get_child_value(response, 0);
  return success_response(bytes_value_from_variant(value));
}

static FlMethodResponse* read_descriptor(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  const gchar* descriptor_uuid =
      lookup_string_argument(arguments, "descriptorUuid");
  if (device_id == nullptr || service_uuid == nullptr ||
      characteristic_uuid == nullptr || descriptor_uuid == nullptr) {
    return error_response(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `descriptorUuid` are required to read a descriptor.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before reading a descriptor.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  g_autoptr(GDBusProxy) descriptor_proxy = descriptor_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid, descriptor_uuid);
  if (descriptor_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before reading.");
  }

  g_autoptr(GVariant) options = empty_dict_variant();
  GVariant* call_children[] = {options};
  g_autoptr(GVariant) parameters = g_variant_new_tuple(call_children, 1);
  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(
      descriptor_proxy, "ReadValue", parameters, G_DBUS_CALL_FLAGS_NONE, -1,
      nullptr, &error);
  if (response == nullptr) {
    return error_response("read-failed",
                          error != nullptr ? error->message
                                           : "Bluetooth descriptor read failed on Linux.");
  }

  g_autoptr(GVariant) value = g_variant_get_child_value(response, 0);
  return success_response(bytes_value_from_variant(value));
}

static FlMethodResponse* write_characteristic(OmniBlePlugin* self,
                                              FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  FlValue* value = arguments != nullptr ? fl_value_lookup_string(arguments, "value")
                                        : nullptr;
  if (device_id == nullptr || service_uuid == nullptr ||
      characteristic_uuid == nullptr || value == nullptr) {
    return error_response(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `value` are required to write a characteristic.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before writing a characteristic.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  g_autoptr(GDBusProxy) characteristic_proxy = characteristic_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid);
  if (characteristic_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before writing.");
  }

  const gchar* write_type = lookup_string_argument(arguments, "writeType");
  g_autoptr(GVariant) bytes = byte_array_variant_from_fl_value(value);
  g_autoptr(GVariant) options = write_options_variant(
      g_strcmp0(write_type, "withoutResponse") == 0 ? "command" : "request");
  GVariant* call_children[] = {bytes, options};
  g_autoptr(GVariant) parameters = g_variant_new_tuple(call_children, 2);
  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(
      characteristic_proxy, "WriteValue", parameters, G_DBUS_CALL_FLAGS_NONE,
      -1, nullptr, &error);
  if (response == nullptr) {
    return error_response("write-failed",
                          error != nullptr ? error->message
                                           : "Bluetooth write failed on Linux.");
  }

  return success_response();
}

static FlMethodResponse* write_descriptor(OmniBlePlugin* self, FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  const gchar* descriptor_uuid =
      lookup_string_argument(arguments, "descriptorUuid");
  FlValue* value = arguments != nullptr ? fl_value_lookup_string(arguments, "value")
                                        : nullptr;
  if (device_id == nullptr || service_uuid == nullptr ||
      characteristic_uuid == nullptr || descriptor_uuid == nullptr ||
      value == nullptr) {
    return error_response(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, `descriptorUuid`, and `value` are required to write a descriptor.");
  }

  if (normalize_uuid(descriptor_uuid) == "00002902-0000-1000-8000-00805f9b34fb") {
    return error_response(
        "unsupported",
        "Use setNotification() to update the client characteristic configuration descriptor.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before writing a descriptor.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  g_autoptr(GDBusProxy) descriptor_proxy = descriptor_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid, descriptor_uuid);
  if (descriptor_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before writing.");
  }

  g_autoptr(GVariant) bytes = byte_array_variant_from_fl_value(value);
  g_autoptr(GVariant) options = empty_dict_variant();
  GVariant* call_children[] = {bytes, options};
  g_autoptr(GVariant) parameters = g_variant_new_tuple(call_children, 2);
  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(
      descriptor_proxy, "WriteValue", parameters, G_DBUS_CALL_FLAGS_NONE, -1,
      nullptr, &error);
  if (response == nullptr) {
    return error_response("write-failed",
                          error != nullptr ? error->message
                                           : "Bluetooth descriptor write failed on Linux.");
  }

  return success_response();
}

static FlMethodResponse* set_notification(OmniBlePlugin* self,
                                          FlValue* arguments) {
  g_autoptr(GError) error = nullptr;
  if (!ensure_object_manager(self, &error)) {
    return error_response("unavailable",
                          error != nullptr ? error->message
                                           : "BlueZ is unavailable.");
  }

  const gchar* device_id = lookup_string_argument(arguments, "deviceId");
  const gchar* service_uuid = lookup_string_argument(arguments, "serviceUuid");
  const gchar* characteristic_uuid =
      lookup_string_argument(arguments, "characteristicUuid");
  FlValue* enabled_value =
      arguments != nullptr ? fl_value_lookup_string(arguments, "enabled")
                           : nullptr;
  if (device_id == nullptr || service_uuid == nullptr ||
      characteristic_uuid == nullptr || enabled_value == nullptr ||
      fl_value_get_type(enabled_value) != FL_VALUE_TYPE_BOOL) {
    return error_response(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `enabled` are required to update notifications.");
  }

  g_autoptr(GDBusProxy) device_proxy = device_proxy_for_id(self, device_id);
  if (device_proxy == nullptr || !proxy_property_bool(device_proxy, "Connected", FALSE)) {
    return error_response(
        "not-connected",
        "Bluetooth device must be connected before updating notifications.");
  }

  const gchar* device_path = g_dbus_proxy_get_object_path(device_proxy);
  g_autoptr(GDBusProxy) characteristic_proxy = characteristic_proxy_for_uuid(
      self, device_path, service_uuid, characteristic_uuid);
  if (characteristic_proxy == nullptr) {
    return error_response(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before enabling notifications.");
  }

  g_autoptr(FlValue) properties = gatt_properties_from_flags(characteristic_proxy);
  gboolean supports_notify = FALSE;
  gboolean supports_indicate = FALSE;
  const size_t property_count = fl_value_get_length(properties);
  for (size_t index = 0; index < property_count; index++) {
    const gchar* property =
        fl_value_get_string(fl_value_get_list_value(properties, index));
    if (g_strcmp0(property, "notify") == 0) {
      supports_notify = TRUE;
    } else if (g_strcmp0(property, "indicate") == 0) {
      supports_indicate = TRUE;
    }
  }

  if (!supports_notify && !supports_indicate) {
    return error_response(
        "unsupported",
        "Bluetooth characteristic does not support notifications or indications.");
  }

  const gboolean enabled = fl_value_get_bool(enabled_value);
  const gchar* method = enabled ? "StartNotify" : "StopNotify";
  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(
      characteristic_proxy, method, g_variant_new("()"), G_DBUS_CALL_FLAGS_NONE,
      -1, nullptr, &error);
  if (response == nullptr) {
    return error_response("set-notification-failed",
                          error != nullptr ? error->message
                                           : "Bluetooth notifications could not be updated on Linux.");
  }

  return success_response();
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
  } else if (strcmp(method, "readCharacteristic") == 0) {
    response = read_characteristic(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "readDescriptor") == 0) {
    response = read_descriptor(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "writeCharacteristic") == 0) {
    response = write_characteristic(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "writeDescriptor") == 0) {
    response = write_descriptor(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "setNotification") == 0) {
    response = set_notification(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "publishGattDatabase") == 0) {
    response = publish_gatt_database(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "clearGattDatabase") == 0) {
    response = clear_gatt_database_response(self);
  } else if (strcmp(method, "startAdvertising") == 0) {
    response = start_advertising(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "stopAdvertising") == 0) {
    response = stop_advertising_response(self);
  } else if (strcmp(method, "notifyCharacteristicValue") == 0) {
    response = notify_characteristic_value(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "respondToReadRequest") == 0) {
    response = respond_to_read_request(self, fl_method_call_get_args(method_call));
  } else if (strcmp(method, "respondToWriteRequest") == 0) {
    response = respond_to_write_request(self, fl_method_call_get_args(method_call));
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
  fl_value_append_take(features, fl_value_new_string("gattClient"));
  fl_value_append_take(features, fl_value_new_string("peripheral"));
  fl_value_append_take(features, fl_value_new_string("advertising"));
  fl_value_append_take(features, fl_value_new_string("gattServer"));
  fl_value_append_take(features, fl_value_new_string("notifications"));

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

  clear_server_database_state(self);
  reset_advertisement_state(self);

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

  if (self->server_connection != nullptr) {
    g_object_unref(self->server_connection);
    self->server_connection = nullptr;
  }

  if (self->server_services != nullptr) {
    g_ptr_array_unref(self->server_services);
    self->server_services = nullptr;
  }

  if (self->server_characteristics != nullptr) {
    g_hash_table_unref(self->server_characteristics);
    self->server_characteristics = nullptr;
  }

  if (self->pending_server_read_requests != nullptr) {
    g_hash_table_unref(self->pending_server_read_requests);
    self->pending_server_read_requests = nullptr;
  }

  if (self->pending_server_write_requests != nullptr) {
    g_hash_table_unref(self->pending_server_write_requests);
    self->pending_server_write_requests = nullptr;
  }

  if (self->advertisement_service_uuids != nullptr) {
    g_ptr_array_unref(self->advertisement_service_uuids);
    self->advertisement_service_uuids = nullptr;
  }

  if (self->advertisement_service_data != nullptr) {
    g_hash_table_unref(self->advertisement_service_data);
    self->advertisement_service_data = nullptr;
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
  self->server_services =
      g_ptr_array_new_with_free_func(destroy_server_service);
  self->server_characteristics =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
  self->pending_server_read_requests =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                            destroy_pending_read_request);
  self->pending_server_write_requests =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                            destroy_pending_write_request);
  self->advertisement_service_uuids =
      g_ptr_array_new_with_free_func(g_free);
  self->advertisement_service_data =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                            destroy_byte_array);
  self->next_request_id = 1;
  self->advertisement_connectable = TRUE;
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
