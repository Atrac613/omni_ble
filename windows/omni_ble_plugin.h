#ifndef FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_
#define FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>

namespace omni_ble {

struct CachedGattCharacteristic {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattCharacteristic characteristic{nullptr};
  winrt::event_token value_changed_token_{};
  bool value_changed_active = false;
  std::unordered_map<
      std::string,
      winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
          GattDescriptor>
      descriptors;
};

struct CachedGattService {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattDeviceService service{nullptr};
  std::unordered_map<std::string, CachedGattCharacteristic> characteristics;
};

struct ConnectionContext {
  winrt::Windows::Devices::Bluetooth::BluetoothLEDevice device{nullptr};
  winrt::event_token connection_status_token_{};
  bool connection_status_active = false;
  std::string state = "disconnected";
  std::unordered_map<std::string, CachedGattService> services;
};

struct LocalGattDescriptorContext {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattLocalDescriptor descriptor{nullptr};
  std::string uuid;
};

struct LocalGattCharacteristicContext {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattLocalCharacteristic characteristic{nullptr};
  std::string service_uuid;
  std::string characteristic_uuid;
  std::vector<uint8_t> value;
  winrt::event_token read_requested_token_{};
  bool read_requested_active = false;
  winrt::event_token write_requested_token_{};
  bool write_requested_active = false;
  winrt::event_token subscribed_clients_changed_token_{};
  bool subscribed_clients_changed_active = false;
  std::unordered_map<std::string, LocalGattDescriptorContext> descriptors;
  std::set<std::string> subscribed_devices;
};

struct LocalGattServiceContext {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattServiceProvider provider{nullptr};
  bool primary = true;
  std::string uuid;
  std::unordered_map<std::string, LocalGattCharacteristicContext> characteristics;
};

struct PendingServerReadRequest {
  winrt::Windows::Foundation::Deferral deferral{nullptr};
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattReadRequest request{nullptr};
  std::string characteristic_key;
  std::string device_id;
  std::string service_uuid;
  std::string characteristic_uuid;
  uint32_t offset = 0;
};

struct PendingServerWriteRequest {
  winrt::Windows::Foundation::Deferral deferral{nullptr};
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattWriteRequest request{nullptr};
  std::string characteristic_key;
  std::string device_id;
  std::string service_uuid;
  std::string characteristic_uuid;
  uint32_t offset = 0;
  std::vector<uint8_t> value;
  bool response_needed = true;
  bool prepared_write = false;
};

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

 private:
  using EventSink = flutter::EventSink<flutter::EncodableValue>;

  flutter::EncodableMap CapabilitiesPayload() const;
  flutter::EncodableMap PermissionStatusPayload(
      const flutter::EncodableValue* arguments) const;
  flutter::EncodableMap PermissionRationalePayload(
      const flutter::EncodableValue* arguments) const;
  std::string CurrentAdapterState() const;
  void RefreshRadioSubscription();
  void ClearRadioSubscription();
  void EmitAdapterState();
  void EmitScanError(int error_code, const std::string& message = "");
  void EmitConnectionState(const std::string& device_id,
                           const std::string& state);
  void EmitScanResult(
      const winrt::Windows::Devices::Bluetooth::Advertisement::
          BluetoothLEAdvertisementReceivedEventArgs& args);
  void StartScan(const flutter::EncodableValue* arguments,
                 std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                     result);
  void StopScan(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void Connect(const flutter::EncodableValue* arguments,
               std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                   result);
  void Disconnect(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void DiscoverServices(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void ReadCharacteristic(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void ReadDescriptor(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void WriteCharacteristic(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void WriteDescriptor(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void SetNotification(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void PublishGattDatabase(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void ClearGattDatabase(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void StartAdvertising(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void StopAdvertising(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void NotifyCharacteristicValue(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RespondToReadRequest(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RespondToWriteRequest(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void OnStreamListen(std::unique_ptr<EventSink>&& events);
  void OnStreamCancel();
  void HandleConnectionStatusChanged(
      const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device);
  void EmitCharacteristicValueChanged(const std::string& device_id,
                                      const std::string& service_uuid,
                                      const std::string& characteristic_uuid,
                                      const std::vector<uint8_t>& value);
  void EmitReadRequest(const std::string& request_id,
                       const std::string& device_id,
                       const std::string& service_uuid,
                       const std::string& characteristic_uuid,
                       uint32_t offset);
  void EmitWriteRequest(const std::string& request_id,
                        const std::string& device_id,
                        const std::string& service_uuid,
                        const std::string& characteristic_uuid,
                        uint32_t offset,
                        const std::vector<uint8_t>& value,
                        bool response_needed);
  void EmitSubscriptionChanged(const std::string& device_id,
                               const std::string& service_uuid,
                               const std::string& characteristic_uuid,
                               bool subscribed);
  void EmitNotificationQueueReady(const std::string& device_id, int status);
  ConnectionContext* FindConnection(const std::string& device_id);
  ConnectionContext* FindConnectedConnection(const std::string& device_id);
  CachedGattCharacteristic* FindCharacteristic(ConnectionContext* connection,
                                               const std::string& service_uuid,
                                               const std::string& characteristic_uuid);
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDescriptor*
  FindDescriptor(ConnectionContext* connection, const std::string& service_uuid,
                 const std::string& characteristic_uuid,
                 const std::string& descriptor_uuid);
  bool RefreshGattCache(ConnectionContext* connection, std::string& error_code,
                        std::string& error_message);
  void ClearConnection(const std::string& device_id);
  bool PeripheralRoleSupported() const;
  bool BuildLocalGattDatabase(const flutter::EncodableValue* arguments,
                              std::string& error_code,
                              std::string& error_message);
  void ClearPublishedGattDatabase();
  void StopAdvertisingInternal();
  void StartServiceAdvertising(const flutter::EncodableValue* arguments,
                               std::string& error_code,
                               std::string& error_message);
  LocalGattCharacteristicContext* FindServerCharacteristic(
      const std::string& service_uuid,
      const std::string& characteristic_uuid);
  std::string ResolveSessionDeviceId(
      const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
          GattSession& session) const;
  void HandleLocalCharacteristicReadRequested(
      const std::string& service_uuid,
      const std::string& characteristic_uuid,
      const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
          GattReadRequestedEventArgs& event_args);
  void HandleLocalCharacteristicWriteRequested(
      const std::string& service_uuid,
      const std::string& characteristic_uuid,
      const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
          GattWriteRequestedEventArgs& event_args);
  void HandleLocalCharacteristicSubscribedClientsChanged(
      const std::string& service_uuid,
      const std::string& characteristic_uuid);
  std::string NextRequestId(const char* prefix);
  flutter::EncodableList ServicesPayload(
      const ConnectionContext& connection) const;
  flutter::EncodableMap ServicePayload(const CachedGattService& service) const;
  flutter::EncodableMap CharacteristicPayload(
      const CachedGattCharacteristic& characteristic) const;
  flutter::EncodableMap DescriptorPayload(
      const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
          GattDescriptor& descriptor) const;

  static std::string NormalizeUuid(const std::string& uuid);
  static std::string GuidToString(const winrt::guid& value);
  static winrt::guid ParseGuid(const std::string& value);
  static std::string FormatBluetoothAddress(uint64_t address);
  static uint64_t ParseBluetoothAddress(const std::string& address);

  std::unique_ptr<EventSink> event_sink_;
  winrt::Windows::Devices::Bluetooth::Advertisement::
      BluetoothLEAdvertisementWatcher watcher_{nullptr};
  winrt::Windows::Devices::Bluetooth::Advertisement::
      BluetoothLEAdvertisementPublisher advertising_publisher_{nullptr};
  winrt::event_token watcher_received_token_{};
  winrt::event_token watcher_stopped_token_{};
  bool watcher_subscription_active_ = false;
  bool is_scanning_ = false;
  bool is_advertising_ = false;
  bool allow_duplicates_ = false;
  std::vector<std::string> service_filters_;
  std::set<std::string> seen_device_ids_;
  winrt::Windows::Devices::Radios::Radio bluetooth_radio_{nullptr};
  winrt::event_token radio_state_token_{};
  bool radio_subscription_active_ = false;
  std::unordered_map<std::string, ConnectionContext> connections_;
  std::unordered_map<std::string, LocalGattServiceContext> local_services_;
  std::unordered_map<std::string, PendingServerReadRequest>
      pending_server_read_requests_;
  std::unordered_map<std::string, PendingServerWriteRequest>
      pending_server_write_requests_;
  uint64_t next_request_id_ = 1;
};

}  // namespace omni_ble

#endif  // FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_
