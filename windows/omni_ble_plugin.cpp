// This must be included before many other Windows headers.
#include <windows.h>

#include "omni_ble_plugin.h"

#include <VersionHelpers.h>
#include <objbase.h>

#include <flutter/event_stream_handler_functions.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

namespace omni_ble {

namespace {

using BluetoothAdapter = winrt::Windows::Devices::Bluetooth::BluetoothAdapter;
using BluetoothCacheMode = winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using BluetoothConnectionStatus = winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using BluetoothError = winrt::Windows::Devices::Bluetooth::BluetoothError;
using BluetoothLEDevice = winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using BluetoothLEAdvertisement = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisement;
using BluetoothLEAdvertisementDataSection =
    winrt::Windows::Devices::Bluetooth::Advertisement::
        BluetoothLEAdvertisementDataSection;
using BluetoothLEAdvertisementFilter = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementFilter;
using BluetoothLEAdvertisementPublisher =
    winrt::Windows::Devices::Bluetooth::Advertisement::
        BluetoothLEAdvertisementPublisher;
using BluetoothLEAdvertisementReceivedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using BluetoothLEAdvertisementWatcher = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using BluetoothLEAdvertisementWatcherStoppedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcherStoppedEventArgs;
using BluetoothLEScanningMode = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using GattClientCharacteristicConfigurationDescriptorValue =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattClientCharacteristicConfigurationDescriptorValue;
using GattCharacteristicProperties = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using GattCommunicationStatus = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using GattDescriptor = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDescriptor;
using GattLocalCharacteristic =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattLocalCharacteristic;
using GattLocalCharacteristicParameters =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattLocalCharacteristicParameters;
using GattLocalDescriptor =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattLocalDescriptor;
using GattLocalDescriptorParameters =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattLocalDescriptorParameters;
using GattProtectionLevel =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattProtectionLevel;
using GattReadRequest = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattReadRequest;
using GattReadRequestedEventArgs = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattReadRequestedEventArgs;
using GattServiceProvider = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattServiceProvider;
using GattServiceProviderAdvertisingParameters =
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattServiceProviderAdvertisingParameters;
using GattSubscribedClient = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattSubscribedClient;
using GattWriteRequest = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattWriteRequest;
using GattWriteRequestedEventArgs = winrt::Windows::Devices::Bluetooth::
    GenericAttributeProfile::GattWriteRequestedEventArgs;
using GattWriteOption = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteOption;
using Radio = winrt::Windows::Devices::Radios::Radio;
using RadioState = winrt::Windows::Devices::Radios::RadioState;
using DataReader = winrt::Windows::Storage::Streams::DataReader;
using DataWriter = winrt::Windows::Storage::Streams::DataWriter;
using IBuffer = winrt::Windows::Storage::Streams::IBuffer;
using Deferral = winrt::Windows::Foundation::Deferral;

struct ParsedCharacteristicAddress {
  std::string device_id;
  std::string service_uuid;
  std::string characteristic_uuid;
};

struct ParsedDescriptorAddress {
  std::string device_id;
  std::string service_uuid;
  std::string characteristic_uuid;
  std::string descriptor_uuid;
};

std::vector<uint8_t> BufferToBytes(const IBuffer& buffer) {
  std::vector<uint8_t> bytes;
  if (!buffer) {
    return bytes;
  }

  bytes.resize(buffer.Length());
  if (bytes.empty()) {
    return bytes;
  }

  auto reader = DataReader::FromBuffer(buffer);
  reader.ReadBytes(bytes);
  return bytes;
}

flutter::EncodableList BytesToEncodableList(const std::vector<uint8_t>& bytes) {
  flutter::EncodableList list;
  for (const auto byte : bytes) {
    list.emplace_back(static_cast<int>(byte));
  }
  return list;
}

IBuffer BytesToBuffer(const std::vector<uint8_t>& bytes) {
  DataWriter writer;
  writer.WriteBytes(bytes);
  return writer.DetachBuffer();
}

std::optional<Radio> TryGetBluetoothRadio() {
  try {
    auto adapter = BluetoothAdapter::GetDefaultAsync().get();
    if (!adapter) {
      return std::nullopt;
    }

    auto radio = adapter.GetRadioAsync().get();
    if (!radio) {
      return std::nullopt;
    }

    return radio;
  } catch (...) {
    return std::nullopt;
  }
}

const flutter::EncodableMap* ArgumentMap(
    const flutter::EncodableValue* arguments) {
  if (arguments == nullptr ||
      !std::holds_alternative<flutter::EncodableMap>(*arguments)) {
    return nullptr;
  }
  return &std::get<flutter::EncodableMap>(*arguments);
}

std::optional<std::string> GetStringArgument(
    const flutter::EncodableValue* arguments,
    const char* key) {
  const auto* map = ArgumentMap(arguments);
  if (map == nullptr) {
    return std::nullopt;
  }
  const auto iterator = map->find(flutter::EncodableValue(key));
  if (iterator == map->end() ||
      !std::holds_alternative<std::string>(iterator->second)) {
    return std::nullopt;
  }
  return std::get<std::string>(iterator->second);
}

std::optional<std::vector<uint8_t>> GetByteArgument(
    const flutter::EncodableValue* arguments,
    const char* key) {
  const auto* map = ArgumentMap(arguments);
  if (map == nullptr) {
    return std::nullopt;
  }
  const auto iterator = map->find(flutter::EncodableValue(key));
  if (iterator == map->end()) {
    return std::nullopt;
  }
  if (std::holds_alternative<std::vector<uint8_t>>(iterator->second)) {
    return std::get<std::vector<uint8_t>>(iterator->second);
  }
  if (std::holds_alternative<flutter::EncodableList>(iterator->second)) {
    std::vector<uint8_t> bytes;
    for (const auto& value :
         std::get<flutter::EncodableList>(iterator->second)) {
      if (std::holds_alternative<int32_t>(value)) {
        bytes.push_back(static_cast<uint8_t>(std::get<int32_t>(value)));
      } else if (std::holds_alternative<int64_t>(value)) {
        bytes.push_back(static_cast<uint8_t>(std::get<int64_t>(value)));
      }
    }
    return bytes;
  }
  return std::nullopt;
}

std::optional<bool> GetBoolArgument(const flutter::EncodableValue* arguments,
                                    const char* key) {
  const auto* map = ArgumentMap(arguments);
  if (map == nullptr) {
    return std::nullopt;
  }
  const auto iterator = map->find(flutter::EncodableValue(key));
  if (iterator == map->end() || !std::holds_alternative<bool>(iterator->second)) {
    return std::nullopt;
  }
  return std::get<bool>(iterator->second);
}

const flutter::EncodableValue* FindMapValue(const flutter::EncodableMap& map,
                                            const char* key) {
  const auto iterator = map.find(flutter::EncodableValue(key));
  if (iterator == map.end()) {
    return nullptr;
  }
  return &iterator->second;
}

const flutter::EncodableMap* AsEncodableMap(
    const flutter::EncodableValue& value) {
  if (!std::holds_alternative<flutter::EncodableMap>(value)) {
    return nullptr;
  }
  return &std::get<flutter::EncodableMap>(value);
}

const flutter::EncodableList* AsEncodableList(
    const flutter::EncodableValue& value) {
  if (!std::holds_alternative<flutter::EncodableList>(value)) {
    return nullptr;
  }
  return &std::get<flutter::EncodableList>(value);
}

std::optional<std::string> GetStringFromMap(const flutter::EncodableMap& map,
                                            const char* key) {
  const auto* value = FindMapValue(map, key);
  if (value == nullptr || !std::holds_alternative<std::string>(*value)) {
    return std::nullopt;
  }
  return std::get<std::string>(*value);
}

std::optional<bool> GetBoolFromMap(const flutter::EncodableMap& map,
                                   const char* key) {
  const auto* value = FindMapValue(map, key);
  if (value == nullptr || !std::holds_alternative<bool>(*value)) {
    return std::nullopt;
  }
  return std::get<bool>(*value);
}

std::vector<uint8_t> BytesFromValue(const flutter::EncodableValue* value) {
  if (value == nullptr) {
    return {};
  }
  if (std::holds_alternative<std::vector<uint8_t>>(*value)) {
    return std::get<std::vector<uint8_t>>(*value);
  }
  if (!std::holds_alternative<flutter::EncodableList>(*value)) {
    return {};
  }

  std::vector<uint8_t> bytes;
  for (const auto& item : std::get<flutter::EncodableList>(*value)) {
    if (std::holds_alternative<int32_t>(item)) {
      bytes.push_back(static_cast<uint8_t>(std::get<int32_t>(item)));
    } else if (std::holds_alternative<int64_t>(item)) {
      bytes.push_back(static_cast<uint8_t>(std::get<int64_t>(item)));
    }
  }
  return bytes;
}

std::vector<std::string> StringListFromValue(
    const flutter::EncodableValue* value) {
  if (value == nullptr || !std::holds_alternative<flutter::EncodableList>(*value)) {
    return {};
  }

  std::vector<std::string> strings;
  for (const auto& item : std::get<flutter::EncodableList>(*value)) {
    if (std::holds_alternative<std::string>(item)) {
      strings.push_back(std::get<std::string>(item));
    }
  }
  return strings;
}

bool ContainsString(const std::vector<std::string>& values,
                    const char* expected) {
  return std::any_of(values.begin(), values.end(),
                     [expected](const auto& value) { return value == expected; });
}

GattCharacteristicProperties PeripheralCharacteristicPropertiesFromValue(
    const flutter::EncodableValue* value) {
  const auto properties = StringListFromValue(value);
  auto result = GattCharacteristicProperties::None;
  if (ContainsString(properties, "read")) {
    result |= GattCharacteristicProperties::Read;
  }
  if (ContainsString(properties, "write")) {
    result |= GattCharacteristicProperties::Write;
  }
  if (ContainsString(properties, "writeWithoutResponse")) {
    result |= GattCharacteristicProperties::WriteWithoutResponse;
  }
  if (ContainsString(properties, "notify")) {
    result |= GattCharacteristicProperties::Notify;
  }
  if (ContainsString(properties, "indicate")) {
    result |= GattCharacteristicProperties::Indicate;
  }
  return result;
}

GattProtectionLevel ProtectionLevelFromValue(
    const flutter::EncodableValue* value,
    bool read_operation) {
  const auto permissions = StringListFromValue(value);
  if (ContainsString(permissions,
                     read_operation ? "readEncrypted" : "writeEncrypted")) {
    return GattProtectionLevel::EncryptionRequired;
  }
  return GattProtectionLevel::Plain;
}

bool IsCccdUuid(const std::string& uuid) {
  return NormalizeUuidValue(uuid) == "00002902-0000-1000-8000-00805f9b34fb";
}

std::string ServerCharacteristicKey(const std::string& service_uuid,
                                    const std::string& characteristic_uuid) {
  return NormalizeUuidValue(service_uuid) + "|" +
         NormalizeUuidValue(characteristic_uuid);
}

std::optional<std::vector<uint8_t>> MergeServerValue(
    const std::vector<uint8_t>& existing_value,
    uint32_t offset,
    const std::vector<uint8_t>& incoming_value) {
  if (offset > existing_value.size()) {
    return std::nullopt;
  }

  auto next_value = existing_value;
  if (offset == next_value.size()) {
    next_value.insert(next_value.end(), incoming_value.begin(),
                      incoming_value.end());
    return next_value;
  }

  const auto required_size =
      static_cast<size_t>(offset) + incoming_value.size();
  if (required_size > next_value.size()) {
    next_value.resize(required_size);
  }
  std::copy(incoming_value.begin(), incoming_value.end(),
            next_value.begin() + offset);
  return next_value;
}

std::string NormalizeUuidValue(const std::string& uuid) {
  std::string normalized = uuid;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  if (normalized.size() == 4) {
    return "0000" + normalized + "-0000-1000-8000-00805f9b34fb";
  }
  if (normalized.size() == 8) {
    return normalized + "-0000-1000-8000-00805f9b34fb";
  }
  return normalized;
}

ParsedCharacteristicAddress ParseCharacteristicAddress(
    const flutter::EncodableValue* arguments) {
  ParsedCharacteristicAddress address;
  address.device_id = GetStringArgument(arguments, "deviceId").value_or("");
  address.service_uuid =
      NormalizeUuidValue(GetStringArgument(arguments, "serviceUuid").value_or(""));
  address.characteristic_uuid = NormalizeUuidValue(
      GetStringArgument(arguments, "characteristicUuid").value_or(""));
  return address;
}

ParsedDescriptorAddress ParseDescriptorAddress(
    const flutter::EncodableValue* arguments) {
  ParsedDescriptorAddress address;
  address.device_id = GetStringArgument(arguments, "deviceId").value_or("");
  address.service_uuid =
      NormalizeUuidValue(GetStringArgument(arguments, "serviceUuid").value_or(""));
  address.characteristic_uuid = NormalizeUuidValue(
      GetStringArgument(arguments, "characteristicUuid").value_or(""));
  address.descriptor_uuid = NormalizeUuidValue(
      GetStringArgument(arguments, "descriptorUuid").value_or(""));
  return address;
}

bool IsValidCharacteristicAddress(const ParsedCharacteristicAddress& address) {
  return !address.device_id.empty() && !address.service_uuid.empty() &&
         !address.characteristic_uuid.empty();
}

bool IsValidDescriptorAddress(const ParsedDescriptorAddress& address) {
  return !address.device_id.empty() && !address.service_uuid.empty() &&
         !address.characteristic_uuid.empty() && !address.descriptor_uuid.empty();
}

std::string GattStatusErrorCode(const GattCommunicationStatus status) {
  switch (status) {
    case GattCommunicationStatus::AccessDenied:
      return "permission-denied";
    case GattCommunicationStatus::Unreachable:
      return "unavailable";
    case GattCommunicationStatus::ProtocolError:
      return "communication-error";
    case GattCommunicationStatus::Success:
      return "";
    default:
      return "gatt-failed";
  }
}

std::string GattStatusMessage(const char* operation,
                              const GattCommunicationStatus status) {
  switch (status) {
    case GattCommunicationStatus::AccessDenied:
      return std::string("Bluetooth access was denied while trying to ") +
             operation + ".";
    case GattCommunicationStatus::Unreachable:
      return std::string("Bluetooth device was unreachable while trying to ") +
             operation + ".";
    case GattCommunicationStatus::ProtocolError:
      return std::string("Bluetooth GATT protocol error while trying to ") +
             operation + ".";
    case GattCommunicationStatus::Success:
      return "";
    default:
      return std::string("Bluetooth operation failed while trying to ") +
             operation + ".";
  }
}

flutter::EncodableList CharacteristicPropertiesPayload(
    const GattCharacteristicProperties properties) {
  flutter::EncodableList payload;
  if ((properties & GattCharacteristicProperties::Read) !=
      GattCharacteristicProperties::None) {
    payload.emplace_back("read");
  }
  if ((properties & GattCharacteristicProperties::Write) !=
      GattCharacteristicProperties::None) {
    payload.emplace_back("write");
  }
  if ((properties & GattCharacteristicProperties::WriteWithoutResponse) !=
      GattCharacteristicProperties::None) {
    payload.emplace_back("writeWithoutResponse");
  }
  if ((properties & GattCharacteristicProperties::Notify) !=
      GattCharacteristicProperties::None) {
    payload.emplace_back("notify");
  }
  if ((properties & GattCharacteristicProperties::Indicate) !=
      GattCharacteristicProperties::None) {
    payload.emplace_back("indicate");
  }
  return payload;
}

}  // namespace

// static
void OmniBlePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto method_channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "omni_ble/methods",
          &flutter::StandardMethodCodec::GetInstance());
  auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), "omni_ble/events",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<OmniBlePlugin>();

  method_channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  event_channel->SetStreamHandler(
      std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
          [plugin_pointer = plugin.get()](
              const flutter::EncodableValue* arguments,
              std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                  events)
              -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
            plugin_pointer->OnStreamListen(std::move(events));
            return nullptr;
          },
          [plugin_pointer = plugin.get()](
              const flutter::EncodableValue* arguments)
              -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
            plugin_pointer->OnStreamCancel();
            return nullptr;
          }));

  registrar->AddPlugin(std::move(plugin));
}

OmniBlePlugin::OmniBlePlugin() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error&) {
  }
  advertising_publisher_ = BluetoothLEAdvertisementPublisher();
  RefreshRadioSubscription();
}

OmniBlePlugin::~OmniBlePlugin() {
  StopScan(nullptr);
  StopAdvertisingInternal();
  ClearPublishedGattDatabase();
  std::vector<std::string> connected_device_ids;
  connected_device_ids.reserve(connections_.size());
  for (const auto& entry : connections_) {
    connected_device_ids.push_back(entry.first);
  }
  for (const auto& device_id : connected_device_ids) {
    ClearConnection(device_id);
  }
  ClearRadioSubscription();
}

flutter::EncodableMap OmniBlePlugin::CapabilitiesPayload() const {
  std::ostringstream version_stream;
  version_stream << "Windows ";
  if (IsWindows10OrGreater()) {
    version_stream << "10+";
  } else if (IsWindows8OrGreater()) {
    version_stream << "8";
  } else if (IsWindows7OrGreater()) {
    version_stream << "7";
  }

  flutter::EncodableList features;
  features.emplace_back("central");
  features.emplace_back("scanning");
  features.emplace_back("gattClient");
  features.emplace_back("peripheral");
  features.emplace_back("advertising");
  features.emplace_back("gattServer");
  features.emplace_back("notifications");

  flutter::EncodableMap metadata;
  metadata[flutter::EncodableValue("adapterState")] =
      flutter::EncodableValue(CurrentAdapterState());

  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("platform")] =
      flutter::EncodableValue("windows");
  payload[flutter::EncodableValue("platformVersion")] =
      flutter::EncodableValue(version_stream.str());
  payload[flutter::EncodableValue("availableFeatures")] =
      flutter::EncodableValue(features);
  payload[flutter::EncodableValue("metadata")] =
      flutter::EncodableValue(metadata);
  return payload;
}

flutter::EncodableMap OmniBlePlugin::PermissionStatusPayload(
    const flutter::EncodableValue* arguments) const {
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

flutter::EncodableMap OmniBlePlugin::PermissionRationalePayload(
    const flutter::EncodableValue* arguments) const {
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
              flutter::EncodableValue(false);
        }
      }
    }
  }

  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("permissions")] =
      flutter::EncodableValue(permissions);
  return payload;
}

std::string OmniBlePlugin::CurrentAdapterState() const {
  const auto radio = TryGetBluetoothRadio();
  if (!radio.has_value()) {
    return "unavailable";
  }

  switch (radio->State()) {
    case RadioState::On:
      return "poweredOn";
    case RadioState::Off:
    case RadioState::Disabled:
      return "poweredOff";
    case RadioState::Unknown:
    default:
      return "unknown";
  }
}

void OmniBlePlugin::RefreshRadioSubscription() {
  ClearRadioSubscription();

  const auto radio = TryGetBluetoothRadio();
  if (!radio.has_value()) {
    return;
  }

  bluetooth_radio_ = *radio;
  radio_state_token_ = bluetooth_radio_.StateChanged([this](const auto& sender,
                                                            const auto&) {
    EmitAdapterState();
  });
  radio_subscription_active_ = true;
}

void OmniBlePlugin::ClearRadioSubscription() {
  if (radio_subscription_active_ && bluetooth_radio_) {
    bluetooth_radio_.StateChanged(radio_state_token_);
  }
  bluetooth_radio_ = nullptr;
  radio_subscription_active_ = false;
}

void OmniBlePlugin::EmitAdapterState() {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("adapterStateChanged");
  event[flutter::EncodableValue("state")] =
      flutter::EncodableValue(CurrentAdapterState());
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitScanError(int error_code, const std::string& message) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("scanError");
  event[flutter::EncodableValue("code")] =
      flutter::EncodableValue(error_code);
  if (!message.empty()) {
    event[flutter::EncodableValue("message")] =
        flutter::EncodableValue(message);
  }
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitConnectionState(const std::string& device_id,
                                        const std::string& state) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("connectionStateChanged");
  event[flutter::EncodableValue("deviceId")] =
      flutter::EncodableValue(device_id);
  event[flutter::EncodableValue("state")] = flutter::EncodableValue(state);
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitCharacteristicValueChanged(
    const std::string& device_id,
    const std::string& service_uuid,
    const std::string& characteristic_uuid,
    const std::vector<uint8_t>& value) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("characteristicValueChanged");
  event[flutter::EncodableValue("deviceId")] =
      flutter::EncodableValue(device_id);
  event[flutter::EncodableValue("serviceUuid")] =
      flutter::EncodableValue(service_uuid);
  event[flutter::EncodableValue("characteristicUuid")] =
      flutter::EncodableValue(characteristic_uuid);
  event[flutter::EncodableValue("value")] =
      flutter::EncodableValue(BytesToEncodableList(value));
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitReadRequest(const std::string& request_id,
                                    const std::string& device_id,
                                    const std::string& service_uuid,
                                    const std::string& characteristic_uuid,
                                    uint32_t offset) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("readRequest");
  event[flutter::EncodableValue("requestId")] =
      flutter::EncodableValue(request_id);
  if (!device_id.empty()) {
    event[flutter::EncodableValue("deviceId")] =
        flutter::EncodableValue(device_id);
  }
  event[flutter::EncodableValue("serviceUuid")] =
      flutter::EncodableValue(service_uuid);
  event[flutter::EncodableValue("characteristicUuid")] =
      flutter::EncodableValue(characteristic_uuid);
  event[flutter::EncodableValue("offset")] =
      flutter::EncodableValue(static_cast<int64_t>(offset));
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitWriteRequest(const std::string& request_id,
                                     const std::string& device_id,
                                     const std::string& service_uuid,
                                     const std::string& characteristic_uuid,
                                     uint32_t offset,
                                     const std::vector<uint8_t>& value,
                                     bool response_needed) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("writeRequest");
  event[flutter::EncodableValue("requestId")] =
      flutter::EncodableValue(request_id);
  if (!device_id.empty()) {
    event[flutter::EncodableValue("deviceId")] =
        flutter::EncodableValue(device_id);
  }
  event[flutter::EncodableValue("serviceUuid")] =
      flutter::EncodableValue(service_uuid);
  event[flutter::EncodableValue("characteristicUuid")] =
      flutter::EncodableValue(characteristic_uuid);
  event[flutter::EncodableValue("offset")] =
      flutter::EncodableValue(static_cast<int64_t>(offset));
  event[flutter::EncodableValue("preparedWrite")] =
      flutter::EncodableValue(false);
  event[flutter::EncodableValue("responseNeeded")] =
      flutter::EncodableValue(response_needed);
  event[flutter::EncodableValue("value")] =
      flutter::EncodableValue(BytesToEncodableList(value));
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitSubscriptionChanged(const std::string& device_id,
                                            const std::string& service_uuid,
                                            const std::string& characteristic_uuid,
                                            bool subscribed) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("subscriptionChanged");
  if (!device_id.empty()) {
    event[flutter::EncodableValue("deviceId")] =
        flutter::EncodableValue(device_id);
  }
  event[flutter::EncodableValue("serviceUuid")] =
      flutter::EncodableValue(service_uuid);
  event[flutter::EncodableValue("characteristicUuid")] =
      flutter::EncodableValue(characteristic_uuid);
  event[flutter::EncodableValue("subscribed")] =
      flutter::EncodableValue(subscribed);
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitNotificationQueueReady(const std::string& device_id,
                                               int status) {
  if (!event_sink_) {
    return;
  }

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("notificationQueueReady");
  if (!device_id.empty()) {
    event[flutter::EncodableValue("deviceId")] =
        flutter::EncodableValue(device_id);
  }
  event[flutter::EncodableValue("status")] =
      flutter::EncodableValue(status);
  event_sink_->Success(flutter::EncodableValue(event));
}

void OmniBlePlugin::EmitScanResult(
    const BluetoothLEAdvertisementReceivedEventArgs& args) {
  const auto device_id = FormatBluetoothAddress(args.BluetoothAddress());
  if (device_id.empty()) {
    return;
  }
  if (!allow_duplicates_ &&
      seen_device_ids_.find(device_id) != seen_device_ids_.end()) {
    return;
  }
  seen_device_ids_.insert(device_id);

  flutter::EncodableMap result;
  result[flutter::EncodableValue("deviceId")] =
      flutter::EncodableValue(device_id);
  const auto local_name = winrt::to_string(args.Advertisement().LocalName());
  if (!local_name.empty()) {
    result[flutter::EncodableValue("name")] =
        flutter::EncodableValue(local_name);
  }
  result[flutter::EncodableValue("rssi")] =
      flutter::EncodableValue(static_cast<int>(args.RawSignalStrengthInDBm()));

  flutter::EncodableList service_uuids;
  for (const auto& uuid : args.Advertisement().ServiceUuids()) {
    service_uuids.emplace_back(GuidToString(uuid));
  }
  result[flutter::EncodableValue("serviceUuids")] =
      flutter::EncodableValue(service_uuids);

  flutter::EncodableMap service_data;
  result[flutter::EncodableValue("serviceData")] =
      flutter::EncodableValue(service_data);

  const auto manufacturer_sections = args.Advertisement().ManufacturerData();
  if (manufacturer_sections.Size() > 0) {
    const auto manufacturer = manufacturer_sections.GetAt(0);
    flutter::EncodableList manufacturer_data;
    manufacturer_data.emplace_back(
        static_cast<int>(manufacturer.CompanyId() & 0xFF));
    manufacturer_data.emplace_back(
        static_cast<int>((manufacturer.CompanyId() >> 8) & 0xFF));
    for (auto byte : BufferToBytes(manufacturer.Data())) {
      manufacturer_data.emplace_back(static_cast<int>(byte));
    }
    result[flutter::EncodableValue("manufacturerData")] =
        flutter::EncodableValue(manufacturer_data);
  }

  result[flutter::EncodableValue("connectable")] =
      flutter::EncodableValue(true);

  flutter::EncodableMap event;
  event[flutter::EncodableValue("type")] =
      flutter::EncodableValue("scanResult");
  event[flutter::EncodableValue("result")] =
      flutter::EncodableValue(result);
  if (event_sink_) {
    event_sink_->Success(flutter::EncodableValue(event));
  }
}

void OmniBlePlugin::StartScan(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (CurrentAdapterState() != "poweredOn") {
    flutter::EncodableMap details;
    details[flutter::EncodableValue("state")] =
        flutter::EncodableValue(CurrentAdapterState());
    result->Error("adapter-unavailable",
                  "Bluetooth adapter must be powered on before scanning.",
                  flutter::EncodableValue(details));
    return;
  }

  allow_duplicates_ = false;
  service_filters_.clear();
  seen_device_ids_.clear();

  if (arguments != nullptr &&
      std::holds_alternative<flutter::EncodableMap>(*arguments)) {
    const auto& argument_map = std::get<flutter::EncodableMap>(*arguments);
    const auto allow_duplicates_it =
        argument_map.find(flutter::EncodableValue("allowDuplicates"));
    if (allow_duplicates_it != argument_map.end() &&
        std::holds_alternative<bool>(allow_duplicates_it->second)) {
      allow_duplicates_ = std::get<bool>(allow_duplicates_it->second);
    }

    const auto service_uuids_it =
        argument_map.find(flutter::EncodableValue("serviceUuids"));
    if (service_uuids_it != argument_map.end() &&
        std::holds_alternative<flutter::EncodableList>(
            service_uuids_it->second)) {
      for (const auto& uuid_value :
           std::get<flutter::EncodableList>(service_uuids_it->second)) {
        if (const auto* uuid = std::get_if<std::string>(&uuid_value)) {
          service_filters_.push_back(NormalizeUuid(*uuid));
        }
      }
    }
  }

  StopScan(nullptr);

  watcher_ = BluetoothLEAdvertisementWatcher();
  watcher_.ScanningMode(BluetoothLEScanningMode::Active);

  if (!service_filters_.empty()) {
    BluetoothLEAdvertisement advertisement;
    for (const auto& uuid : service_filters_) {
      const auto parsed = ParseGuid(uuid);
      if (parsed != winrt::guid{}) {
        advertisement.ServiceUuids().Append(parsed);
      }
    }
    BluetoothLEAdvertisementFilter filter;
    filter.Advertisement(advertisement);
    watcher_.AdvertisementFilter(filter);
  }

  watcher_received_token_ = watcher_.Received(
      [this](const auto& sender, const BluetoothLEAdvertisementReceivedEventArgs& args) {
        EmitScanResult(args);
      });
  watcher_stopped_token_ = watcher_.Stopped(
      [this](const auto& sender,
             const BluetoothLEAdvertisementWatcherStoppedEventArgs& args) {
        if (args.Error() != BluetoothError::Success) {
          EmitScanError(static_cast<int>(args.Error()),
                        "Windows advertisement watcher stopped with an error.");
        }
      });
  watcher_subscription_active_ = true;

  try {
    watcher_.Start();
    is_scanning_ = true;
    EmitAdapterState();
    result->Success();
  } catch (const winrt::hresult_error& error) {
    StopScan(nullptr);
    result->Error("scan-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::StopScan(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  try {
    if (watcher_) {
      if (watcher_subscription_active_) {
        watcher_.Received(watcher_received_token_);
        watcher_.Stopped(watcher_stopped_token_);
        watcher_subscription_active_ = false;
      }
      if (watcher_.Status() ==
              winrt::Windows::Devices::Bluetooth::Advertisement::
                  BluetoothLEAdvertisementWatcherStatus::Started ||
          watcher_.Status() ==
              winrt::Windows::Devices::Bluetooth::Advertisement::
                  BluetoothLEAdvertisementWatcherStatus::Stopping) {
        watcher_.Stop();
      }
    }
  } catch (...) {
  }

  watcher_ = nullptr;
  is_scanning_ = false;
  seen_device_ids_.clear();

  if (result) {
    result->Success();
  }
}

void OmniBlePlugin::Connect(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (CurrentAdapterState() != "poweredOn") {
    flutter::EncodableMap details;
    details[flutter::EncodableValue("state")] =
        flutter::EncodableValue(CurrentAdapterState());
    result->Error("adapter-unavailable",
                  "Bluetooth adapter must be powered on before connecting.",
                  flutter::EncodableValue(details));
    return;
  }

  const auto device_id = GetStringArgument(arguments, "deviceId");
  if (!device_id.has_value() || device_id->empty()) {
    result->Error("invalid-argument", "`deviceId` is required to connect.");
    return;
  }

  auto* existing_connection = FindConnection(*device_id);
  if (existing_connection != nullptr &&
      existing_connection->state == "connected" && existing_connection->device) {
    result->Success();
    return;
  }
  if (existing_connection != nullptr &&
      existing_connection->state == "connecting") {
    result->Error("busy",
                  "Bluetooth connection is already in progress for this device.");
    return;
  }
  if (existing_connection != nullptr) {
    ClearConnection(*device_id);
  }

  EmitConnectionState(*device_id, "connecting");

  try {
    const auto bluetooth_address = ParseBluetoothAddress(*device_id);
    if (bluetooth_address == 0) {
      result->Error(
          "invalid-argument",
          "`deviceId` must be a 48-bit Bluetooth address such as `AA:BB:CC:DD:EE:FF`.");
      EmitConnectionState(*device_id, "disconnected");
      return;
    }

    const auto device =
        BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address).get();
    if (!device) {
      const auto message =
          "Bluetooth device `" + *device_id +
          "` is not available. Scan first or reconnect to a known device.";
      result->Error(
          "unavailable", message);
      EmitConnectionState(*device_id, "disconnected");
      return;
    }

    ConnectionContext context;
    context.device = device;
    context.state = "connecting";
    context.connection_status_token_ = device.ConnectionStatusChanged(
        [this](const BluetoothLEDevice& sender, const auto&) {
          HandleConnectionStatusChanged(sender);
        });
    context.connection_status_active = true;
    connections_.insert_or_assign(*device_id, std::move(context));

    auto* connection = FindConnection(*device_id);
    std::string error_code;
    std::string error_message;
    if (connection == nullptr ||
        !RefreshGattCache(connection, error_code, error_message)) {
      ClearConnection(*device_id);
      result->Error("connection-failed",
                    error_message.empty()
                        ? "Bluetooth connection could not be established."
                        : error_message);
      EmitConnectionState(*device_id, "disconnected");
      return;
    }

    connection->state = "connected";
    result->Success();
    EmitConnectionState(*device_id, "connected");
  } catch (const winrt::hresult_error& error) {
    ClearConnection(*device_id);
    result->Error("connection-failed", winrt::to_string(error.message()));
    EmitConnectionState(*device_id, "disconnected");
  }
}

void OmniBlePlugin::Disconnect(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto device_id = GetStringArgument(arguments, "deviceId");
  if (!device_id.has_value() || device_id->empty()) {
    result->Error("invalid-argument", "`deviceId` is required to disconnect.");
    return;
  }

  auto* connection = FindConnection(*device_id);
  if (connection == nullptr || !connection->device ||
      connection->state == "disconnected") {
    ClearConnection(*device_id);
    result->Success();
    return;
  }

  if (connection->state == "disconnecting") {
    result->Error(
        "busy",
        "Bluetooth disconnection is already in progress for this device.");
    return;
  }

  connection->state = "disconnecting";
  EmitConnectionState(*device_id, "disconnecting");
  ClearConnection(*device_id);
  EmitConnectionState(*device_id, "disconnected");
  result->Success();
}

void OmniBlePlugin::DiscoverServices(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto device_id = GetStringArgument(arguments, "deviceId");
  if (!device_id.has_value() || device_id->empty()) {
    result->Error("invalid-argument",
                  "`deviceId` is required to discover services.");
    return;
  }

  auto* connection = FindConnectedConnection(*device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before discovering services.");
    return;
  }

  std::string error_code;
  std::string error_message;
  if (!RefreshGattCache(connection, error_code, error_message)) {
    result->Error(error_code.empty() ? "discovery-failed" : error_code,
                  error_message.empty()
                      ? "Bluetooth service discovery failed."
                      : error_message);
    return;
  }

  result->Success(flutter::EncodableValue(ServicesPayload(*connection)));
}

void OmniBlePlugin::ReadCharacteristic(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto address = ParseCharacteristicAddress(arguments);
  if (!IsValidCharacteristicAddress(address)) {
    result->Error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, and `characteristicUuid` are required to read a characteristic.");
    return;
  }

  auto* connection = FindConnectedConnection(address.device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before reading a characteristic.");
    return;
  }

  auto* characteristic =
      FindCharacteristic(connection, address.service_uuid,
                         address.characteristic_uuid);
  if (characteristic == nullptr) {
    result->Error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before reading.");
    return;
  }

  const auto properties =
      characteristic->characteristic.CharacteristicProperties();
  if ((properties & GattCharacteristicProperties::Read) ==
      GattCharacteristicProperties::None) {
    result->Error("unsupported",
                  "Bluetooth characteristic does not support reading.");
    return;
  }

  try {
    const auto read_result =
        characteristic->characteristic
            .ReadValueAsync(BluetoothCacheMode::Uncached)
            .get();
    if (read_result.Status() != GattCommunicationStatus::Success) {
      result->Error(
          GattStatusErrorCode(read_result.Status()).empty()
              ? "read-failed"
              : GattStatusErrorCode(read_result.Status()),
          GattStatusMessage("read the characteristic", read_result.Status()));
      return;
    }

    result->Success(
        flutter::EncodableValue(BytesToEncodableList(BufferToBytes(
            read_result.Value()))));
  } catch (const winrt::hresult_error& error) {
    result->Error("read-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::ReadDescriptor(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto address = ParseDescriptorAddress(arguments);
  if (!IsValidDescriptorAddress(address)) {
    result->Error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `descriptorUuid` are required to read a descriptor.");
    return;
  }

  auto* connection = FindConnectedConnection(address.device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before reading a descriptor.");
    return;
  }

  auto* descriptor = FindDescriptor(connection, address.service_uuid,
                                    address.characteristic_uuid,
                                    address.descriptor_uuid);
  if (descriptor == nullptr) {
    result->Error(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before reading.");
    return;
  }

  try {
    const auto read_result =
        descriptor->ReadValueAsync(BluetoothCacheMode::Uncached).get();
    if (read_result.Status() != GattCommunicationStatus::Success) {
      result->Error(
          GattStatusErrorCode(read_result.Status()).empty()
              ? "read-failed"
              : GattStatusErrorCode(read_result.Status()),
          GattStatusMessage("read the descriptor", read_result.Status()));
      return;
    }

    result->Success(
        flutter::EncodableValue(BytesToEncodableList(BufferToBytes(
            read_result.Value()))));
  } catch (const winrt::hresult_error& error) {
    result->Error("read-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::WriteCharacteristic(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto address = ParseCharacteristicAddress(arguments);
  const auto value = GetByteArgument(arguments, "value");
  if (!IsValidCharacteristicAddress(address) || !value.has_value()) {
    result->Error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `value` are required to write a characteristic.");
    return;
  }

  auto* connection = FindConnectedConnection(address.device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before writing a characteristic.");
    return;
  }

  auto* characteristic =
      FindCharacteristic(connection, address.service_uuid,
                         address.characteristic_uuid);
  if (characteristic == nullptr) {
    result->Error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before writing.");
    return;
  }

  const auto write_type =
      GetStringArgument(arguments, "writeType").value_or("withResponse");
  const auto properties =
      characteristic->characteristic.CharacteristicProperties();
  auto write_option = GattWriteOption::WriteWithResponse;
  if (write_type == "withoutResponse") {
    write_option = GattWriteOption::WriteWithoutResponse;
    if ((properties & GattCharacteristicProperties::WriteWithoutResponse) ==
        GattCharacteristicProperties::None) {
      result->Error(
          "unsupported",
          "Bluetooth characteristic does not support write without response.");
      return;
    }
  } else if ((properties & GattCharacteristicProperties::Write) ==
             GattCharacteristicProperties::None) {
    result->Error("unsupported",
                  "Bluetooth characteristic does not support writing.");
    return;
  }

  try {
    const auto status = characteristic->characteristic
                            .WriteValueAsync(BytesToBuffer(*value), write_option)
                            .get();
    if (status != GattCommunicationStatus::Success) {
      result->Error(
          GattStatusErrorCode(status).empty() ? "write-failed"
                                              : GattStatusErrorCode(status),
          GattStatusMessage("write the characteristic", status));
      return;
    }

    result->Success();
  } catch (const winrt::hresult_error& error) {
    result->Error("write-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::WriteDescriptor(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto address = ParseDescriptorAddress(arguments);
  const auto value = GetByteArgument(arguments, "value");
  if (!IsValidDescriptorAddress(address) || !value.has_value()) {
    result->Error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, `descriptorUuid`, and `value` are required to write a descriptor.");
    return;
  }

  if (address.descriptor_uuid == "00002902-0000-1000-8000-00805f9b34fb") {
    result->Error(
        "unsupported",
        "Use setNotification() to update the client characteristic configuration descriptor.");
    return;
  }

  auto* connection = FindConnectedConnection(address.device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before writing a descriptor.");
    return;
  }

  auto* descriptor = FindDescriptor(connection, address.service_uuid,
                                    address.characteristic_uuid,
                                    address.descriptor_uuid);
  if (descriptor == nullptr) {
    result->Error(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before writing.");
    return;
  }

  try {
    const auto status =
        descriptor->WriteValueAsync(BytesToBuffer(*value)).get();
    if (status != GattCommunicationStatus::Success) {
      result->Error(
          GattStatusErrorCode(status).empty() ? "write-failed"
                                              : GattStatusErrorCode(status),
          GattStatusMessage("write the descriptor", status));
      return;
    }

    result->Success();
  } catch (const winrt::hresult_error& error) {
    result->Error("write-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::SetNotification(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto address = ParseCharacteristicAddress(arguments);
  if (!IsValidCharacteristicAddress(address) ||
      !GetBoolArgument(arguments, "enabled").has_value()) {
    result->Error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `enabled` are required to update notifications.");
    return;
  }

  auto* connection = FindConnectedConnection(address.device_id);
  if (connection == nullptr) {
    result->Error(
        "not-connected",
        "Bluetooth device must be connected before updating notifications.");
    return;
  }

  auto* characteristic =
      FindCharacteristic(connection, address.service_uuid,
                         address.characteristic_uuid);
  if (characteristic == nullptr) {
    result->Error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before enabling notifications.");
    return;
  }

  const auto enabled = GetBoolArgument(arguments, "enabled").value_or(false);
  const auto properties =
      characteristic->characteristic.CharacteristicProperties();
  const auto supports_notify =
      (properties & GattCharacteristicProperties::Notify) !=
      GattCharacteristicProperties::None;
  const auto supports_indicate =
      (properties & GattCharacteristicProperties::Indicate) !=
      GattCharacteristicProperties::None;
  if (enabled && !supports_notify && !supports_indicate) {
    result->Error(
        "unsupported",
        "Bluetooth characteristic does not support notifications or indications.");
    return;
  }

  auto descriptor_value =
      GattClientCharacteristicConfigurationDescriptorValue::None;
  if (enabled) {
    descriptor_value = supports_notify
                           ? GattClientCharacteristicConfigurationDescriptorValue::Notify
                           : GattClientCharacteristicConfigurationDescriptorValue::Indicate;
  }

  try {
    const auto status =
        characteristic->characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                descriptor_value)
            .get();
    if (status != GattCommunicationStatus::Success) {
      result->Error(
          GattStatusErrorCode(status).empty() ? "set-notification-failed"
                                              : GattStatusErrorCode(status),
          GattStatusMessage("update notifications", status));
      return;
    }

    if (!enabled) {
      if (characteristic->value_changed_active) {
        try {
          characteristic->characteristic.ValueChanged(
              characteristic->value_changed_token_);
        } catch (...) {
        }
        characteristic->value_changed_active = false;
      }
      result->Success();
      return;
    }

    if (!characteristic->value_changed_active) {
      characteristic->value_changed_token_ =
          characteristic->characteristic.ValueChanged(
              [this, device_id = address.device_id,
               service_uuid = address.service_uuid,
               characteristic_uuid = address.characteristic_uuid](
                  const auto& sender, const auto& args) {
                EmitCharacteristicValueChanged(
                    device_id, service_uuid, characteristic_uuid,
                    BufferToBytes(args.CharacteristicValue()));
              });
      characteristic->value_changed_active = true;
    }

    result->Success();
  } catch (const winrt::hresult_error& error) {
    result->Error("set-notification-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::PublishGattDatabase(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (!PeripheralRoleSupported()) {
    result->Error(
        "unsupported",
        "Bluetooth peripheral role is not available on this Windows device.");
    return;
  }

  if (CurrentAdapterState() != "poweredOn") {
    flutter::EncodableMap details;
    details[flutter::EncodableValue("state")] =
        flutter::EncodableValue(CurrentAdapterState());
    result->Error("adapter-unavailable",
                  "Bluetooth adapter must be powered on before publishing GATT services.",
                  flutter::EncodableValue(details));
    return;
  }

  StopAdvertisingInternal();
  ClearPublishedGattDatabase();

  std::string error_code;
  std::string error_message;
  if (!BuildLocalGattDatabase(arguments, error_code, error_message)) {
    ClearPublishedGattDatabase();
    result->Error(error_code.empty() ? "publish-gatt-database-failed"
                                     : error_code,
                  error_message.empty()
                      ? "Bluetooth GATT database could not be published."
                      : error_message);
    return;
  }

  result->Success();
}

void OmniBlePlugin::ClearGattDatabase(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  StopAdvertisingInternal();
  ClearPublishedGattDatabase();
  result->Success();
}

void OmniBlePlugin::StartAdvertising(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (!PeripheralRoleSupported()) {
    result->Error(
        "unsupported",
        "Bluetooth peripheral role is not available on this Windows device.");
    return;
  }

  if (CurrentAdapterState() != "poweredOn") {
    flutter::EncodableMap details;
    details[flutter::EncodableValue("state")] =
        flutter::EncodableValue(CurrentAdapterState());
    result->Error("adapter-unavailable",
                  "Bluetooth adapter must be powered on before advertising.",
                  flutter::EncodableValue(details));
    return;
  }

  StopAdvertisingInternal();

  std::string error_code;
  std::string error_message;
  StartServiceAdvertising(arguments, error_code, error_message);
  if (!error_code.empty()) {
    result->Error(error_code, error_message);
    return;
  }

  result->Success();
}

void OmniBlePlugin::StopAdvertising(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  StopAdvertisingInternal();
  result->Success();
}

void OmniBlePlugin::NotifyCharacteristicValue(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto service_uuid = GetStringArgument(arguments, "serviceUuid");
  const auto characteristic_uuid =
      GetStringArgument(arguments, "characteristicUuid");
  const auto value = GetByteArgument(arguments, "value");
  if (!service_uuid.has_value() || service_uuid->empty() ||
      !characteristic_uuid.has_value() || characteristic_uuid->empty() ||
      !value.has_value()) {
    result->Error(
        "invalid-argument",
        "`serviceUuid`, `characteristicUuid`, and `value` are required to notify subscribers.");
    return;
  }

  auto* characteristic =
      FindServerCharacteristic(NormalizeUuid(*service_uuid),
                               NormalizeUuid(*characteristic_uuid));
  if (characteristic == nullptr || !characteristic->characteristic) {
    result->Error(
        "unavailable",
        "Bluetooth characteristic was not found. Publish a GATT database first.");
    return;
  }

  const auto properties =
      characteristic->characteristic.CharacteristicProperties();
  const auto supports_notify =
      (properties & GattCharacteristicProperties::Notify) !=
      GattCharacteristicProperties::None;
  const auto supports_indicate =
      (properties & GattCharacteristicProperties::Indicate) !=
      GattCharacteristicProperties::None;
  if (!supports_notify && !supports_indicate) {
    result->Error(
        "unsupported",
        "Bluetooth characteristic does not support notifications or indications.");
    return;
  }

  characteristic->value = *value;

  std::vector<GattSubscribedClient> targets;
  const auto target_device_id = GetStringArgument(arguments, "deviceId");
  try {
    for (const auto& client : characteristic->characteristic.SubscribedClients()) {
      const auto device_id = ResolveSessionDeviceId(client.Session());
      if (!target_device_id.has_value() || target_device_id->empty()) {
        targets.push_back(client);
        continue;
      }
      if (device_id == *target_device_id) {
        targets.push_back(client);
        break;
      }
    }
  } catch (const winrt::hresult_error& error) {
    result->Error("notify-failed", winrt::to_string(error.message()));
    return;
  }

  if (target_device_id.has_value() && !target_device_id->empty() &&
      targets.empty()) {
    result->Error(
        "unavailable",
        "The target central is not subscribed to this characteristic.");
    return;
  }

  if (targets.empty()) {
    result->Success();
    return;
  }

  try {
    for (const auto& client : targets) {
      characteristic->characteristic
          .NotifyValueAsync(BytesToBuffer(*value), client)
          .get();
      EmitNotificationQueueReady(
          ResolveSessionDeviceId(client.Session()),
          0);
    }
  } catch (const winrt::hresult_error& error) {
    result->Error("notify-failed", winrt::to_string(error.message()));
    return;
  }

  result->Success();
}

void OmniBlePlugin::RespondToReadRequest(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto request_id = GetStringArgument(arguments, "requestId");
  const auto value = GetByteArgument(arguments, "value");
  if (!request_id.has_value() || request_id->empty() || !value.has_value()) {
    result->Error(
        "invalid-argument",
        "`requestId` and `value` are required to answer a read request.");
    return;
  }

  const auto iterator = pending_server_read_requests_.find(*request_id);
  if (iterator == pending_server_read_requests_.end()) {
    result->Error(
        "unavailable",
        "Bluetooth read request was not found or has already been answered.");
    return;
  }

  auto pending_request = std::move(iterator->second);
  pending_server_read_requests_.erase(iterator);

  auto* characteristic = FindServerCharacteristic(
      pending_request.service_uuid, pending_request.characteristic_uuid);
  if (characteristic != nullptr) {
    characteristic->value = *value;
  }

  try {
    if (pending_request.offset > value->size()) {
      pending_request.request.RespondWithProtocolError(0x07);
      if (pending_request.deferral) {
        pending_request.deferral.Complete();
      }
      result->Error(
          "invalid-argument",
          "The provided value is shorter than the pending read offset.");
      return;
    }

    const std::vector<uint8_t> response_value(value->begin() +
                                                  pending_request.offset,
                                              value->end());
    pending_request.request.RespondWithValue(BytesToBuffer(response_value));
    if (pending_request.deferral) {
      pending_request.deferral.Complete();
    }
    result->Success();
  } catch (const winrt::hresult_error& error) {
    if (pending_request.deferral) {
      try {
        pending_request.deferral.Complete();
      } catch (...) {
      }
    }
    result->Error("respond-failed", winrt::to_string(error.message()));
  }
}

void OmniBlePlugin::RespondToWriteRequest(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto request_id = GetStringArgument(arguments, "requestId");
  if (!request_id.has_value() || request_id->empty()) {
    result->Error(
        "invalid-argument", "`requestId` is required to answer a write request.");
    return;
  }

  const auto iterator = pending_server_write_requests_.find(*request_id);
  if (iterator == pending_server_write_requests_.end()) {
    result->Error(
        "unavailable",
        "Bluetooth write request was not found or has already been answered.");
    return;
  }

  auto pending_request = std::move(iterator->second);
  pending_server_write_requests_.erase(iterator);

  const auto accept = GetBoolArgument(arguments, "accept").value_or(true);
  auto* characteristic = FindServerCharacteristic(
      pending_request.service_uuid, pending_request.characteristic_uuid);

  try {
    if (!accept) {
      if (pending_request.response_needed) {
        pending_request.request.RespondWithProtocolError(0x0E);
      }
      if (pending_request.deferral) {
        pending_request.deferral.Complete();
      }
      result->Success();
      return;
    }

    const auto current_value =
        characteristic != nullptr ? characteristic->value : std::vector<uint8_t>{};
    const auto merged_value = MergeServerValue(current_value,
                                               pending_request.offset,
                                               pending_request.value);
    if (!merged_value.has_value()) {
      if (pending_request.response_needed) {
        pending_request.request.RespondWithProtocolError(0x07);
      }
      if (pending_request.deferral) {
        pending_request.deferral.Complete();
      }
      result->Error(
          "invalid-argument",
          "The pending write request used an unsupported offset.");
      return;
    }

    if (characteristic != nullptr) {
      characteristic->value = *merged_value;
    }

    if (pending_request.response_needed) {
      pending_request.request.Respond();
    }
    if (pending_request.deferral) {
      pending_request.deferral.Complete();
    }
    result->Success();
  } catch (const winrt::hresult_error& error) {
    if (pending_request.deferral) {
      try {
        pending_request.deferral.Complete();
      } catch (...) {
      }
    }
    result->Error("respond-failed", winrt::to_string(error.message()));
  }
}

bool OmniBlePlugin::PeripheralRoleSupported() const {
  try {
    const auto adapter = BluetoothAdapter::GetDefaultAsync().get();
    return adapter && adapter.IsPeripheralRoleSupported();
  } catch (...) {
    return false;
  }
}

bool OmniBlePlugin::BuildLocalGattDatabase(
    const flutter::EncodableValue* arguments,
    std::string& error_code,
    std::string& error_message) {
  const auto* argument_map = ArgumentMap(arguments);
  if (argument_map == nullptr) {
    error_code = "invalid-argument";
    error_message = "A valid GATT database payload is required to publish services.";
    return false;
  }

  const auto* services_value = FindMapValue(*argument_map, "services");
  if (services_value == nullptr) {
    error_code = "invalid-argument";
    error_message = "A valid GATT database payload is required to publish services.";
    return false;
  }
  const auto* services = AsEncodableList(*services_value);
  if (services == nullptr) {
    error_code = "invalid-argument";
    error_message = "A valid services list is required to publish GATT services.";
    return false;
  }

  try {
    for (const auto& service_value : *services) {
      const auto* service_map = AsEncodableMap(service_value);
      if (service_map == nullptr) {
        error_code = "invalid-argument";
        error_message = "Each published service must be a map payload.";
        return false;
      }

      const auto service_uuid =
          NormalizeUuid(GetStringFromMap(*service_map, "uuid").value_or(""));
      const auto service_guid = ParseGuid(service_uuid);
      if (service_uuid.empty() || service_guid == winrt::guid{}) {
        error_code = "invalid-argument";
        error_message = "Each published service must include a valid UUID.";
        return false;
      }

      const auto provider_result =
          GattServiceProvider::CreateAsync(service_guid).get();
      if (provider_result.Error() != BluetoothError::Success) {
        error_code = "publish-gatt-database-failed";
        error_message = "Bluetooth service provider could not be created.";
        return false;
      }

      LocalGattServiceContext service_context;
      service_context.provider = provider_result.ServiceProvider();
      service_context.primary = GetBoolFromMap(*service_map, "primary").value_or(true);
      service_context.uuid = service_uuid;

      const auto* characteristics_value =
          FindMapValue(*service_map, "characteristics");
      const auto* characteristics = characteristics_value != nullptr
                                        ? AsEncodableList(*characteristics_value)
                                        : nullptr;
      if (characteristics != nullptr) {
        for (const auto& characteristic_value : *characteristics) {
          const auto* characteristic_map = AsEncodableMap(characteristic_value);
          if (characteristic_map == nullptr) {
            error_code = "invalid-argument";
            error_message =
                "Each published characteristic must be a map payload.";
            return false;
          }

          const auto characteristic_uuid = NormalizeUuid(
              GetStringFromMap(*characteristic_map, "uuid").value_or(""));
          const auto characteristic_guid = ParseGuid(characteristic_uuid);
          if (characteristic_uuid.empty() ||
              characteristic_guid == winrt::guid{}) {
            error_code = "invalid-argument";
            error_message =
                "Each published characteristic must include a valid UUID.";
            return false;
          }

          GattLocalCharacteristicParameters parameters;
          parameters.CharacteristicProperties(
              PeripheralCharacteristicPropertiesFromValue(
                  FindMapValue(*characteristic_map, "properties")));
          parameters.ReadProtectionLevel(ProtectionLevelFromValue(
              FindMapValue(*characteristic_map, "permissions"), true));
          parameters.WriteProtectionLevel(ProtectionLevelFromValue(
              FindMapValue(*characteristic_map, "permissions"), false));
          const auto initial_value =
              BytesFromValue(FindMapValue(*characteristic_map, "initialValue"));
          if (!initial_value.empty()) {
            parameters.StaticValue(BytesToBuffer(initial_value));
          }

          const auto characteristic_result =
              service_context.provider.Service()
                  .CreateCharacteristicAsync(characteristic_guid, parameters)
                  .get();
          if (characteristic_result.Error() != BluetoothError::Success) {
            error_code = "publish-gatt-database-failed";
            error_message = "Bluetooth characteristic could not be created.";
            return false;
          }

          LocalGattCharacteristicContext characteristic_context;
          characteristic_context.characteristic =
              characteristic_result.Characteristic();
          characteristic_context.service_uuid = service_uuid;
          characteristic_context.characteristic_uuid = characteristic_uuid;
          characteristic_context.value = initial_value;
          characteristic_context.read_requested_token_ =
              characteristic_context.characteristic.ReadRequested(
                  [this, service_uuid, characteristic_uuid](
                      const GattLocalCharacteristic&,
                      const GattReadRequestedEventArgs& event_args) {
                    HandleLocalCharacteristicReadRequested(
                        service_uuid, characteristic_uuid, event_args);
                  });
          characteristic_context.read_requested_active = true;
          characteristic_context.write_requested_token_ =
              characteristic_context.characteristic.WriteRequested(
                  [this, service_uuid, characteristic_uuid](
                      const GattLocalCharacteristic&,
                      const GattWriteRequestedEventArgs& event_args) {
                    HandleLocalCharacteristicWriteRequested(
                        service_uuid, characteristic_uuid, event_args);
                  });
          characteristic_context.write_requested_active = true;
          characteristic_context.subscribed_clients_changed_token_ =
              characteristic_context.characteristic.SubscribedClientsChanged(
                  [this, service_uuid, characteristic_uuid](
                      const GattLocalCharacteristic&, const auto&) {
                    HandleLocalCharacteristicSubscribedClientsChanged(
                        service_uuid, characteristic_uuid);
                  });
          characteristic_context.subscribed_clients_changed_active = true;

          const auto* descriptors_value =
              FindMapValue(*characteristic_map, "descriptors");
          const auto* descriptors = descriptors_value != nullptr
                                        ? AsEncodableList(*descriptors_value)
                                        : nullptr;
          if (descriptors != nullptr) {
            for (const auto& descriptor_value : *descriptors) {
              const auto* descriptor_map = AsEncodableMap(descriptor_value);
              if (descriptor_map == nullptr) {
                error_code = "invalid-argument";
                error_message =
                    "Each published descriptor must be a map payload.";
                return false;
              }

              const auto descriptor_uuid = NormalizeUuid(
                  GetStringFromMap(*descriptor_map, "uuid").value_or(""));
              if (descriptor_uuid.empty()) {
                error_code = "invalid-argument";
                error_message =
                    "Each published descriptor must include a valid UUID.";
                return false;
              }
              if (IsCccdUuid(descriptor_uuid)) {
                continue;
              }

              GattLocalDescriptorParameters descriptor_parameters;
              descriptor_parameters.ReadProtectionLevel(ProtectionLevelFromValue(
                  FindMapValue(*descriptor_map, "permissions"), true));
              descriptor_parameters.WriteProtectionLevel(ProtectionLevelFromValue(
                  FindMapValue(*descriptor_map, "permissions"), false));
              const auto descriptor_initial_value =
                  BytesFromValue(FindMapValue(*descriptor_map, "initialValue"));
              if (!descriptor_initial_value.empty()) {
                descriptor_parameters.StaticValue(
                    BytesToBuffer(descriptor_initial_value));
              }

              const auto descriptor_result =
                  characteristic_context.characteristic
                      .CreateDescriptorAsync(ParseGuid(descriptor_uuid),
                                             descriptor_parameters)
                      .get();
              if (descriptor_result.Error() != BluetoothError::Success) {
                error_code = "publish-gatt-database-failed";
                error_message = "Bluetooth descriptor could not be created.";
                return false;
              }

              LocalGattDescriptorContext descriptor_context;
              descriptor_context.descriptor = descriptor_result.Descriptor();
              descriptor_context.uuid = descriptor_uuid;
              characteristic_context.descriptors.insert_or_assign(
                  descriptor_uuid, std::move(descriptor_context));
            }
          }

          service_context.characteristics.insert_or_assign(
              characteristic_uuid, std::move(characteristic_context));
        }
      }

      local_services_.insert_or_assign(service_uuid, std::move(service_context));
    }
  } catch (const winrt::hresult_error& error) {
    error_code = "publish-gatt-database-failed";
    error_message = winrt::to_string(error.message());
    return false;
  }

  return true;
}

void OmniBlePlugin::ClearPublishedGattDatabase() {
  for (auto& pending_entry : pending_server_read_requests_) {
    try {
      if (pending_entry.second.request) {
        pending_entry.second.request.RespondWithProtocolError(0x06);
      }
      if (pending_entry.second.deferral) {
        pending_entry.second.deferral.Complete();
      }
    } catch (...) {
    }
  }
  pending_server_read_requests_.clear();

  for (auto& pending_entry : pending_server_write_requests_) {
    try {
      if (pending_entry.second.response_needed && pending_entry.second.request) {
        pending_entry.second.request.RespondWithProtocolError(0x06);
      }
      if (pending_entry.second.deferral) {
        pending_entry.second.deferral.Complete();
      }
    } catch (...) {
    }
  }
  pending_server_write_requests_.clear();

  for (auto& service_entry : local_services_) {
    for (auto& characteristic_entry : service_entry.second.characteristics) {
      auto& characteristic = characteristic_entry.second;
      if (characteristic.read_requested_active) {
        try {
          characteristic.characteristic.ReadRequested(
              characteristic.read_requested_token_);
        } catch (...) {
        }
        characteristic.read_requested_active = false;
      }
      if (characteristic.write_requested_active) {
        try {
          characteristic.characteristic.WriteRequested(
              characteristic.write_requested_token_);
        } catch (...) {
        }
        characteristic.write_requested_active = false;
      }
      if (characteristic.subscribed_clients_changed_active) {
        try {
          characteristic.characteristic.SubscribedClientsChanged(
              characteristic.subscribed_clients_changed_token_);
        } catch (...) {
        }
        characteristic.subscribed_clients_changed_active = false;
      }
      characteristic.descriptors.clear();
      characteristic.subscribed_devices.clear();
      characteristic.value.clear();
    }
    if (service_entry.second.provider) {
      try {
        service_entry.second.provider.StopAdvertising();
      } catch (...) {
      }
    }
  }
  local_services_.clear();
  next_request_id_ = 1;
}

void OmniBlePlugin::StopAdvertisingInternal() {
  for (auto& service_entry : local_services_) {
    if (!service_entry.second.provider) {
      continue;
    }
    try {
      service_entry.second.provider.StopAdvertising();
    } catch (...) {
    }
  }

  if (advertising_publisher_) {
    try {
      advertising_publisher_.Stop();
    } catch (...) {
    }
  }
  is_advertising_ = false;
}

void OmniBlePlugin::StartServiceAdvertising(
    const flutter::EncodableValue* arguments,
    std::string& error_code,
    std::string& error_message) {
  error_code.clear();
  error_message.clear();

  const auto connectable = GetBoolArgument(arguments, "connectable").value_or(true);
  if (connectable && local_services_.empty()) {
    error_code = "unavailable";
    error_message =
        "Publish a GATT database before starting connectable advertising on Windows.";
    return;
  }

  try {
    if (connectable) {
      GattServiceProviderAdvertisingParameters parameters;
      parameters.IsConnectable(true);
      parameters.IsDiscoverable(true);
      for (auto& service_entry : local_services_) {
        service_entry.second.provider.StartAdvertising(parameters);
      }
    }

    advertising_publisher_ = BluetoothLEAdvertisementPublisher();
    auto advertisement = advertising_publisher_.Advertisement();

    if (const auto* argument_map = ArgumentMap(arguments)) {
      if (const auto* service_uuids_value =
              FindMapValue(*argument_map, "serviceUuids")) {
        for (const auto& service_uuid : StringListFromValue(service_uuids_value)) {
          const auto parsed = ParseGuid(NormalizeUuid(service_uuid));
          if (parsed != winrt::guid{}) {
            advertisement.ServiceUuids().Append(parsed);
          }
        }
      }

      if (const auto* service_data_value = FindMapValue(*argument_map, "serviceData")) {
        const auto* service_data_map = service_data_value != nullptr
                                           ? AsEncodableMap(*service_data_value)
                                           : nullptr;
        if (service_data_map != nullptr) {
          for (const auto& entry : *service_data_map) {
            if (!std::holds_alternative<std::string>(entry.first)) {
              continue;
            }
            const auto raw_uuid = std::get<std::string>(entry.first);
            const auto normalized_uuid = NormalizeUuid(raw_uuid);
            const auto parsed = ParseGuid(normalized_uuid);
            if (parsed == winrt::guid{}) {
              continue;
            }

            BluetoothLEAdvertisementDataSection data_section;
            uint8_t data_type =
                static_cast<uint8_t>(winrt::Windows::Devices::Bluetooth::
                                         Advertisement::
                                             BluetoothLEAdvertisementDataTypes::
                                                 ServiceData128BitUuids());
            if (raw_uuid.size() == 4) {
              data_type =
                  static_cast<uint8_t>(winrt::Windows::Devices::Bluetooth::
                                           Advertisement::
                                               BluetoothLEAdvertisementDataTypes::
                                                   ServiceData16BitUuids());
            } else if (raw_uuid.size() == 8) {
              data_type =
                  static_cast<uint8_t>(winrt::Windows::Devices::Bluetooth::
                                           Advertisement::
                                               BluetoothLEAdvertisementDataTypes::
                                                   ServiceData32BitUuids());
            }
            data_section.DataType(data_type);
            DataWriter writer;
            if (raw_uuid.size() == 4) {
              const auto short_uuid =
                  static_cast<uint16_t>(std::stoul(raw_uuid, nullptr, 16));
              writer.WriteUInt16(short_uuid);
            } else if (raw_uuid.size() == 8) {
              const auto short_uuid =
                  static_cast<uint32_t>(std::stoul(raw_uuid, nullptr, 16));
              writer.WriteUInt32(short_uuid);
            } else {
              writer.WriteGuid(parsed);
            }
            writer.WriteBytes(BytesFromValue(&entry.second));
            data_section.Data(writer.DetachBuffer());
            advertisement.DataSections().Append(data_section);
          }
        }
      }

      if (const auto* manufacturer_data_value =
              FindMapValue(*argument_map, "manufacturerData")) {
        const auto manufacturer_data = BytesFromValue(manufacturer_data_value);
        if (manufacturer_data.size() >= 2) {
          winrt::Windows::Devices::Bluetooth::Advertisement::
              BluetoothLEManufacturerData data;
          const auto company_id =
              static_cast<uint16_t>(manufacturer_data[0]) |
              (static_cast<uint16_t>(manufacturer_data[1]) << 8);
          data.CompanyId(company_id);
          DataWriter writer;
          if (manufacturer_data.size() > 2) {
            writer.WriteBytes(std::vector<uint8_t>(manufacturer_data.begin() + 2,
                                                   manufacturer_data.end()));
          }
          data.Data(writer.DetachBuffer());
          advertisement.ManufacturerData().Append(data);
        }
      }
    }

    if (!connectable || advertisement.ServiceUuids().Size() > 0 ||
        advertisement.DataSections().Size() > 0 ||
        advertisement.ManufacturerData().Size() > 0) {
      advertising_publisher_.Start();
    }

    is_advertising_ = connectable || advertisement.ServiceUuids().Size() > 0 ||
                      advertisement.DataSections().Size() > 0 ||
                      advertisement.ManufacturerData().Size() > 0;
  } catch (const winrt::hresult_error& error) {
    StopAdvertisingInternal();
    error_code = "advertise-failed";
    error_message = winrt::to_string(error.message());
  }
}

LocalGattCharacteristicContext* OmniBlePlugin::FindServerCharacteristic(
    const std::string& service_uuid,
    const std::string& characteristic_uuid) {
  const auto service_iterator = local_services_.find(NormalizeUuid(service_uuid));
  if (service_iterator == local_services_.end()) {
    return nullptr;
  }

  const auto characteristic_iterator =
      service_iterator->second.characteristics.find(
          NormalizeUuid(characteristic_uuid));
  if (characteristic_iterator ==
      service_iterator->second.characteristics.end()) {
    return nullptr;
  }

  return &characteristic_iterator->second;
}

std::string OmniBlePlugin::ResolveSessionDeviceId(
    const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::
        GattSession& session) const {
  try {
    if (!session || session.DeviceId().Id().empty()) {
      return "";
    }
    const auto device = BluetoothLEDevice::FromIdAsync(session.DeviceId().Id()).get();
    if (!device) {
      return "";
    }
    return FormatBluetoothAddress(device.BluetoothAddress());
  } catch (...) {
    return "";
  }
}

void OmniBlePlugin::HandleLocalCharacteristicReadRequested(
    const std::string& service_uuid,
    const std::string& characteristic_uuid,
    const GattReadRequestedEventArgs& event_args) {
  try {
    const auto deferral = event_args.GetDeferral();
    const auto request = event_args.GetRequestAsync().get();
    if (!request) {
      if (deferral) {
        deferral.Complete();
      }
      return;
    }

    const auto request_id = NextRequestId("read");
    PendingServerReadRequest pending_request;
    pending_request.deferral = deferral;
    pending_request.request = request;
    pending_request.device_id = ResolveSessionDeviceId(event_args.Session());
    pending_request.service_uuid = service_uuid;
    pending_request.characteristic_uuid = characteristic_uuid;
    pending_request.characteristic_key =
        ServerCharacteristicKey(service_uuid, characteristic_uuid);
    pending_request.offset = request.Offset();
    pending_server_read_requests_.insert_or_assign(request_id,
                                                   std::move(pending_request));
    EmitReadRequest(request_id, ResolveSessionDeviceId(event_args.Session()),
                    service_uuid, characteristic_uuid, request.Offset());
  } catch (...) {
  }
}

void OmniBlePlugin::HandleLocalCharacteristicWriteRequested(
    const std::string& service_uuid,
    const std::string& characteristic_uuid,
    const GattWriteRequestedEventArgs& event_args) {
  try {
    const auto deferral = event_args.GetDeferral();
    const auto request = event_args.GetRequestAsync().get();
    if (!request) {
      if (deferral) {
        deferral.Complete();
      }
      return;
    }

    PendingServerWriteRequest pending_request;
    pending_request.deferral = deferral;
    pending_request.request = request;
    pending_request.device_id = ResolveSessionDeviceId(event_args.Session());
    pending_request.service_uuid = service_uuid;
    pending_request.characteristic_uuid = characteristic_uuid;
    pending_request.characteristic_key =
        ServerCharacteristicKey(service_uuid, characteristic_uuid);
    pending_request.offset = request.Offset();
    pending_request.value = BufferToBytes(request.Value());
    pending_request.response_needed =
        request.Option() != GattWriteOption::WriteWithoutResponse;

    const auto request_id = NextRequestId("write");
    pending_server_write_requests_.insert_or_assign(request_id,
                                                    pending_request);
    EmitWriteRequest(request_id, pending_request.device_id, service_uuid,
                     characteristic_uuid, pending_request.offset,
                     pending_request.value, pending_request.response_needed);
  } catch (...) {
  }
}

void OmniBlePlugin::HandleLocalCharacteristicSubscribedClientsChanged(
    const std::string& service_uuid,
    const std::string& characteristic_uuid) {
  auto* characteristic =
      FindServerCharacteristic(service_uuid, characteristic_uuid);
  if (characteristic == nullptr || !characteristic->characteristic) {
    return;
  }

  std::set<std::string> next_subscribers;
  try {
    for (const auto& client : characteristic->characteristic.SubscribedClients()) {
      const auto device_id = ResolveSessionDeviceId(client.Session());
      if (!device_id.empty()) {
        next_subscribers.insert(device_id);
      }
    }
  } catch (...) {
    return;
  }

  for (const auto& device_id : next_subscribers) {
    if (characteristic->subscribed_devices.find(device_id) ==
        characteristic->subscribed_devices.end()) {
      EmitSubscriptionChanged(device_id, service_uuid, characteristic_uuid,
                              true);
    }
  }

  for (const auto& device_id : characteristic->subscribed_devices) {
    if (next_subscribers.find(device_id) == next_subscribers.end()) {
      EmitSubscriptionChanged(device_id, service_uuid, characteristic_uuid,
                              false);
    }
  }

  characteristic->subscribed_devices = std::move(next_subscribers);
}

std::string OmniBlePlugin::NextRequestId(const char* prefix) {
  std::ostringstream stream;
  stream << prefix << "-" << next_request_id_++;
  return stream.str();
}

void OmniBlePlugin::HandleConnectionStatusChanged(
    const BluetoothLEDevice& device) {
  try {
    if (!device ||
        device.ConnectionStatus() != BluetoothConnectionStatus::Disconnected) {
      return;
    }

    const auto device_id = FormatBluetoothAddress(device.BluetoothAddress());
    if (device_id.empty()) {
      return;
    }

    ClearConnection(device_id);
    EmitConnectionState(device_id, "disconnected");
  } catch (...) {
  }
}

ConnectionContext* OmniBlePlugin::FindConnection(const std::string& device_id) {
  const auto iterator = connections_.find(device_id);
  if (iterator == connections_.end()) {
    return nullptr;
  }
  return &iterator->second;
}

ConnectionContext* OmniBlePlugin::FindConnectedConnection(
    const std::string& device_id) {
  auto* connection = FindConnection(device_id);
  if (connection == nullptr || !connection->device ||
      connection->state != "connected") {
    return nullptr;
  }
  return connection;
}

CachedGattCharacteristic* OmniBlePlugin::FindCharacteristic(
    ConnectionContext* connection,
    const std::string& service_uuid,
    const std::string& characteristic_uuid) {
  if (connection == nullptr) {
    return nullptr;
  }

  const auto service_iterator = connection->services.find(service_uuid);
  if (service_iterator == connection->services.end()) {
    return nullptr;
  }

  const auto characteristic_iterator =
      service_iterator->second.characteristics.find(characteristic_uuid);
  if (characteristic_iterator ==
      service_iterator->second.characteristics.end()) {
    return nullptr;
  }

  return &characteristic_iterator->second;
}

GattDescriptor* OmniBlePlugin::FindDescriptor(
    ConnectionContext* connection,
    const std::string& service_uuid,
    const std::string& characteristic_uuid,
    const std::string& descriptor_uuid) {
  auto* characteristic =
      FindCharacteristic(connection, service_uuid, characteristic_uuid);
  if (characteristic == nullptr) {
    return nullptr;
  }

  const auto descriptor_iterator =
      characteristic->descriptors.find(descriptor_uuid);
  if (descriptor_iterator == characteristic->descriptors.end()) {
    return nullptr;
  }

  return &descriptor_iterator->second;
}

bool OmniBlePlugin::RefreshGattCache(ConnectionContext* connection,
                                     std::string& error_code,
                                     std::string& error_message) {
  if (connection == nullptr || !connection->device) {
    error_code = "not-connected";
    error_message =
        "Bluetooth device must be connected before discovering services.";
    return false;
  }

  for (auto& service_entry : connection->services) {
    for (auto& characteristic_entry : service_entry.second.characteristics) {
      if (characteristic_entry.second.value_changed_active) {
        try {
          characteristic_entry.second.characteristic.ValueChanged(
              characteristic_entry.second.value_changed_token_);
        } catch (...) {
        }
        characteristic_entry.second.value_changed_active = false;
      }
    }
    if (service_entry.second.service) {
      try {
        service_entry.second.service.Close();
      } catch (...) {
      }
    }
  }
  connection->services.clear();

  try {
    const auto services_result =
        connection->device.GetGattServicesAsync(BluetoothCacheMode::Uncached)
            .get();
    if (services_result.Status() != GattCommunicationStatus::Success) {
      error_code = GattStatusErrorCode(services_result.Status());
      error_message =
          GattStatusMessage("discover services", services_result.Status());
      return false;
    }

    for (const auto& service : services_result.Services()) {
      CachedGattService cached_service;
      cached_service.service = service;

      try {
        const auto characteristics_result =
            service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
        if (characteristics_result.Status() ==
            GattCommunicationStatus::Success) {
          for (const auto& characteristic :
               characteristics_result.Characteristics()) {
            CachedGattCharacteristic cached_characteristic;
            cached_characteristic.characteristic = characteristic;

            try {
              const auto descriptors_result =
                  characteristic.GetDescriptorsAsync(BluetoothCacheMode::Uncached)
                      .get();
              if (descriptors_result.Status() ==
                  GattCommunicationStatus::Success) {
                for (const auto& descriptor :
                     descriptors_result.Descriptors()) {
                  cached_characteristic.descriptors.insert_or_assign(
                      GuidToString(descriptor.Uuid()), descriptor);
                }
              }
            } catch (...) {
            }

            cached_service.characteristics.insert_or_assign(
                GuidToString(characteristic.Uuid()),
                std::move(cached_characteristic));
          }
        }
      } catch (...) {
      }

      connection->services.insert_or_assign(GuidToString(service.Uuid()),
                                            std::move(cached_service));
    }

    return true;
  } catch (const winrt::hresult_error& error) {
    error_code = "discovery-failed";
    error_message = winrt::to_string(error.message());
    connection->services.clear();
    return false;
  }
}

void OmniBlePlugin::ClearConnection(const std::string& device_id) {
  auto iterator = connections_.find(device_id);
  if (iterator == connections_.end()) {
    return;
  }

  auto& connection = iterator->second;
  for (auto& service_entry : connection.services) {
    for (auto& characteristic_entry : service_entry.second.characteristics) {
      if (characteristic_entry.second.value_changed_active) {
        try {
          characteristic_entry.second.characteristic.ValueChanged(
              characteristic_entry.second.value_changed_token_);
        } catch (...) {
        }
        characteristic_entry.second.value_changed_active = false;
      }
    }
    if (service_entry.second.service) {
      try {
        service_entry.second.service.Close();
      } catch (...) {
      }
    }
  }
  connection.services.clear();

  if (connection.connection_status_active && connection.device) {
    try {
      connection.device.ConnectionStatusChanged(
          connection.connection_status_token_);
    } catch (...) {
    }
    connection.connection_status_active = false;
  }

  if (connection.device) {
    try {
      connection.device.Close();
    } catch (...) {
    }
  }

  connections_.erase(iterator);
}

flutter::EncodableList OmniBlePlugin::ServicesPayload(
    const ConnectionContext& connection) const {
  flutter::EncodableList payload;
  for (const auto& entry : connection.services) {
    payload.emplace_back(ServicePayload(entry.second));
  }
  return payload;
}

flutter::EncodableMap OmniBlePlugin::ServicePayload(
    const CachedGattService& service) const {
  flutter::EncodableList characteristics;
  for (const auto& entry : service.characteristics) {
    characteristics.emplace_back(CharacteristicPayload(entry.second));
  }

  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("uuid")] =
      flutter::EncodableValue(GuidToString(service.service.Uuid()));
  payload[flutter::EncodableValue("primary")] = flutter::EncodableValue(true);
  payload[flutter::EncodableValue("characteristics")] =
      flutter::EncodableValue(characteristics);
  return payload;
}

flutter::EncodableMap OmniBlePlugin::CharacteristicPayload(
    const CachedGattCharacteristic& characteristic) const {
  flutter::EncodableList descriptors;
  for (const auto& entry : characteristic.descriptors) {
    descriptors.emplace_back(DescriptorPayload(entry.second));
  }

  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("uuid")] =
      flutter::EncodableValue(GuidToString(characteristic.characteristic.Uuid()));
  payload[flutter::EncodableValue("properties")] =
      flutter::EncodableValue(CharacteristicPropertiesPayload(
          characteristic.characteristic.CharacteristicProperties()));
  payload[flutter::EncodableValue("permissions")] =
      flutter::EncodableValue(flutter::EncodableList());
  payload[flutter::EncodableValue("descriptors")] =
      flutter::EncodableValue(descriptors);
  payload[flutter::EncodableValue("initialValue")] = flutter::EncodableValue();
  return payload;
}

flutter::EncodableMap OmniBlePlugin::DescriptorPayload(
    const GattDescriptor& descriptor) const {
  flutter::EncodableMap payload;
  payload[flutter::EncodableValue("uuid")] =
      flutter::EncodableValue(GuidToString(descriptor.Uuid()));
  payload[flutter::EncodableValue("permissions")] =
      flutter::EncodableValue(flutter::EncodableList());
  payload[flutter::EncodableValue("initialValue")] = flutter::EncodableValue();
  return payload;
}

void OmniBlePlugin::OnStreamListen(std::unique_ptr<EventSink>&& events) {
  event_sink_ = std::move(events);
  RefreshRadioSubscription();
  EmitAdapterState();
}

void OmniBlePlugin::OnStreamCancel() { event_sink_ = nullptr; }

std::string OmniBlePlugin::NormalizeUuid(const std::string& uuid) {
  std::string normalized = uuid;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  if (normalized.size() == 4) {
    return "0000" + normalized + "-0000-1000-8000-00805f9b34fb";
  }
  if (normalized.size() == 8) {
    return normalized + "-0000-1000-8000-00805f9b34fb";
  }
  return normalized;
}

std::string OmniBlePlugin::GuidToString(const winrt::guid& value) {
  wchar_t buffer[39];
  const int length = StringFromGUID2(value, buffer, 39);
  if (length <= 1) {
    return "";
  }
  std::wstring wide(buffer, length - 1);
  if (!wide.empty() && wide.front() == L'{') {
    wide.erase(wide.begin());
  }
  if (!wide.empty() && wide.back() == L'}') {
    wide.pop_back();
  }
  return NormalizeUuid(winrt::to_string(winrt::hstring(wide)));
}

winrt::guid OmniBlePlugin::ParseGuid(const std::string& value) {
  std::wstring wide(value.begin(), value.end());
  GUID guid = GUID_NULL;
  if (IIDFromString(wide.c_str(), &guid) != S_OK) {
    return GUID_NULL;
  }
  return guid;
}

std::string OmniBlePlugin::FormatBluetoothAddress(uint64_t address) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::uppercase;
  for (int index = 5; index >= 0; --index) {
    stream << std::setw(2) << ((address >> (index * 8)) & 0xFF);
    if (index > 0) {
      stream << ":";
    }
  }
  return stream.str();
}

uint64_t OmniBlePlugin::ParseBluetoothAddress(const std::string& address) {
  std::string normalized;
  normalized.reserve(address.size());
  for (const auto character : address) {
    if (std::isxdigit(static_cast<unsigned char>(character))) {
      normalized.push_back(character);
    }
  }
  if (normalized.size() != 12) {
    return 0;
  }
  return std::stoull(normalized, nullptr, 16);
}

void OmniBlePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("getCapabilities") == 0) {
    result->Success(flutter::EncodableValue(CapabilitiesPayload()));
  } else if (method_call.method_name().compare("checkPermissions") == 0 ||
             method_call.method_name().compare("requestPermissions") == 0) {
    result->Success(flutter::EncodableValue(
        PermissionStatusPayload(method_call.arguments())));
  } else if (method_call.method_name().compare("shouldShowRequestRationale") ==
             0) {
    result->Success(flutter::EncodableValue(
        PermissionRationalePayload(method_call.arguments())));
  } else if (method_call.method_name().compare("openAppSettings") == 0 ||
             method_call.method_name().compare("openBluetoothSettings") == 0) {
    result->Success(flutter::EncodableValue(false));
  } else if (method_call.method_name().compare("startScan") == 0) {
    StartScan(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("stopScan") == 0) {
    StopScan(std::move(result));
  } else if (method_call.method_name().compare("connect") == 0) {
    Connect(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("disconnect") == 0) {
    Disconnect(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("discoverServices") == 0) {
    DiscoverServices(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("readCharacteristic") == 0) {
    ReadCharacteristic(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("readDescriptor") == 0) {
    ReadDescriptor(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("writeCharacteristic") == 0) {
    WriteCharacteristic(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("writeDescriptor") == 0) {
    WriteDescriptor(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("setNotification") == 0) {
    SetNotification(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("publishGattDatabase") == 0) {
    PublishGattDatabase(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("clearGattDatabase") == 0) {
    ClearGattDatabase(std::move(result));
  } else if (method_call.method_name().compare("startAdvertising") == 0) {
    StartAdvertising(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("stopAdvertising") == 0) {
    StopAdvertising(std::move(result));
  } else if (method_call.method_name().compare("notifyCharacteristicValue") ==
             0) {
    NotifyCharacteristicValue(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("respondToReadRequest") == 0) {
    RespondToReadRequest(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("respondToWriteRequest") == 0) {
    RespondToWriteRequest(method_call.arguments(), std::move(result));
  } else {
    result->NotImplemented();
  }
}

}  // namespace omni_ble
