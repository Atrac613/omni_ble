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
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>

namespace omni_ble {

struct CachedGattCharacteristic {
  winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
      GattCharacteristic characteristic{nullptr};
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
  void OnStreamListen(std::unique_ptr<EventSink>&& events);
  void OnStreamCancel();
  void HandleConnectionStatusChanged(
      const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice& device);
  ConnectionContext* FindConnection(const std::string& device_id);
  ConnectionContext* FindConnectedConnection(const std::string& device_id);
  bool RefreshGattCache(ConnectionContext* connection, std::string& error_code,
                        std::string& error_message);
  void ClearConnection(const std::string& device_id);
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
  winrt::event_token watcher_received_token_{};
  winrt::event_token watcher_stopped_token_{};
  bool watcher_subscription_active_ = false;
  bool is_scanning_ = false;
  bool allow_duplicates_ = false;
  std::vector<std::string> service_filters_;
  std::set<std::string> seen_device_ids_;
  winrt::Windows::Devices::Radios::Radio bluetooth_radio_{nullptr};
  winrt::event_token radio_state_token_{};
  bool radio_subscription_active_ = false;
  std::unordered_map<std::string, ConnectionContext> connections_;
};

}  // namespace omni_ble

#endif  // FLUTTER_PLUGIN_OMNI_BLE_PLUGIN_H_
