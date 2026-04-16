// This must be included before many other Windows headers.
#include <windows.h>

#include "omni_ble_plugin.h"

#include <VersionHelpers.h>
#include <objbase.h>

#include <flutter/event_stream_handler_functions.h>
#include <flutter/standard_method_codec.h>

#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Storage.Streams.h>

namespace omni_ble {

namespace {

using BluetoothAdapter = winrt::Windows::Devices::Bluetooth::BluetoothAdapter;
using BluetoothError = winrt::Windows::Devices::Bluetooth::BluetoothError;
using BluetoothLEAdvertisement = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisement;
using BluetoothLEAdvertisementFilter = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementFilter;
using BluetoothLEAdvertisementReceivedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using BluetoothLEAdvertisementWatcher = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using BluetoothLEAdvertisementWatcherStoppedEventArgs = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcherStoppedEventArgs;
using BluetoothLEScanningMode = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
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

void OmniBlePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("getCapabilities") == 0) {
    result->Success(flutter::EncodableValue(CapabilitiesPayload()));
  } else if (method_call.method_name().compare("checkPermissions") == 0 ||
             method_call.method_name().compare("requestPermissions") == 0) {
    result->Success(flutter::EncodableValue(
        PermissionStatusPayload(method_call.arguments())));
  } else if (method_call.method_name().compare("startScan") == 0) {
    StartScan(method_call.arguments(), std::move(result));
  } else if (method_call.method_name().compare("stopScan") == 0) {
    StopScan(std::move(result));
  } else {
    result->NotImplemented();
  }
}

}  // namespace omni_ble
