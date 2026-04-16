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
using BluetoothLEAdvertisementFilter = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementFilter;
using BluetoothLEAdvertisementReceivedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using BluetoothLEAdvertisementWatcher = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using BluetoothLEAdvertisementWatcherStoppedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcherStoppedEventArgs;
using BluetoothLEScanningMode = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using GattCharacteristicProperties = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using GattCommunicationStatus = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using GattDescriptor = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDescriptor;
using Radio = winrt::Windows::Devices::Radios::Radio;
using RadioState = winrt::Windows::Devices::Radios::RadioState;
using DataReader = winrt::Windows::Storage::Streams::DataReader;
using IBuffer = winrt::Windows::Storage::Streams::IBuffer;

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
  RefreshRadioSubscription();
}

OmniBlePlugin::~OmniBlePlugin() {
  StopScan(nullptr);
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

bool OmniBlePlugin::RefreshGattCache(ConnectionContext* connection,
                                     std::string& error_code,
                                     std::string& error_message) {
  if (connection == nullptr || !connection->device) {
    error_code = "not-connected";
    error_message =
        "Bluetooth device must be connected before discovering services.";
    return false;
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
  } else {
    result->NotImplemented();
  }
}

}  // namespace omni_ble
