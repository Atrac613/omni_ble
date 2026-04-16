import 'dart:typed_data';

import 'omni_ble_platform_interface.dart';
import 'src/omni_ble_models.dart';

export 'omni_ble_method_channel.dart';
export 'omni_ble_platform_interface.dart';
export 'src/omni_ble_exception.dart';
export 'src/omni_ble_models.dart';

const omniBle = OmniBle();

class OmniBle {
  const OmniBle();

  OmniBlePlatform get _platform => OmniBlePlatform.instance;

  OmniBleCentral get central => OmniBleCentral._(_platform);

  OmniBlePermissions get permissions => OmniBlePermissions._(_platform);

  OmniBlePeripheral get peripheral => OmniBlePeripheral._(_platform);

  Stream<OmniBleEvent> get events => _platform.observeEvents();

  Future<OmniBleCapabilities> getCapabilities() => _platform.getCapabilities();
}

class OmniBlePermissions {
  const OmniBlePermissions._(this._platform);

  final OmniBlePlatform _platform;

  Future<OmniBlePermissionStatus> check(Set<OmniBlePermission> permissions) {
    return _platform.checkPermissions(permissions);
  }

  Future<OmniBlePermissionStatus> request(Set<OmniBlePermission> permissions) {
    return _platform.requestPermissions(permissions);
  }

  Future<Map<OmniBlePermission, bool>> shouldShowRequestRationale(
    Set<OmniBlePermission> permissions,
  ) {
    return _platform.shouldShowRequestRationale(permissions);
  }

  Future<bool> openAppSettings() => _platform.openAppSettings();

  Future<bool> openBluetoothSettings() => _platform.openBluetoothSettings();
}

class OmniBleCentral {
  const OmniBleCentral._(this._platform);

  final OmniBlePlatform _platform;

  Future<void> startScan({
    OmniBleScanConfig config = const OmniBleScanConfig(),
  }) {
    return _platform.startScan(config);
  }

  Future<void> stopScan() => _platform.stopScan();

  Future<void> connect(
    String deviceId, {
    OmniBleConnectionConfig config = const OmniBleConnectionConfig(),
  }) {
    return _platform.connect(deviceId, config: config);
  }

  Future<void> disconnect(String deviceId) => _platform.disconnect(deviceId);

  Future<List<OmniBleGattService>> discoverServices(String deviceId) {
    return _platform.discoverServices(deviceId);
  }

  Future<int> readRssi(String deviceId) {
    return _platform.readRssi(deviceId);
  }

  Future<int> requestMtu(String deviceId, {int mtu = 512}) {
    return _platform.requestMtu(deviceId, mtu: mtu);
  }

  Future<void> requestConnectionPriority(
    String deviceId,
    OmniBleConnectionPriority priority,
  ) {
    return _platform.requestConnectionPriority(deviceId, priority);
  }

  Future<void> setPreferredPhy(
    String deviceId, {
    OmniBlePhy txPhy = OmniBlePhy.le1m,
    OmniBlePhy rxPhy = OmniBlePhy.le1m,
    OmniBlePhyCoding coding = OmniBlePhyCoding.unspecified,
  }) {
    return _platform.setPreferredPhy(
      deviceId,
      txPhy: txPhy,
      rxPhy: rxPhy,
      coding: coding,
    );
  }

  Future<Uint8List> readCharacteristic(OmniBleCharacteristicAddress address) {
    return _platform.readCharacteristic(address);
  }

  Future<Uint8List> readDescriptor(OmniBleDescriptorAddress address) {
    return _platform.readDescriptor(address);
  }

  Future<void> writeCharacteristic(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    OmniBleWriteType type = OmniBleWriteType.withResponse,
  }) {
    return _platform.writeCharacteristic(address, value, type: type);
  }

  Future<void> writeDescriptor(
    OmniBleDescriptorAddress address,
    Uint8List value,
  ) {
    return _platform.writeDescriptor(address, value);
  }

  Future<void> setNotification(
    OmniBleCharacteristicAddress address, {
    required bool enabled,
  }) {
    return _platform.setNotification(address, enabled: enabled);
  }
}

class OmniBlePeripheral {
  const OmniBlePeripheral._(this._platform);

  final OmniBlePlatform _platform;

  Future<void> publishGattDatabase(OmniBleGattDatabase database) {
    return _platform.publishGattDatabase(database);
  }

  Future<void> clearGattDatabase() => _platform.clearGattDatabase();

  Future<void> startAdvertising(OmniBleAdvertisement advertisement) {
    return _platform.startAdvertising(advertisement);
  }

  Future<void> stopAdvertising() => _platform.stopAdvertising();

  Future<void> notifyCharacteristicValue(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    String? deviceId,
  }) {
    return _platform.notifyCharacteristicValue(
      address,
      value,
      deviceId: deviceId,
    );
  }

  Future<void> respondToReadRequest(String requestId, Uint8List value) {
    return _platform.respondToReadRequest(requestId, value);
  }

  Future<void> respondToWriteRequest(String requestId, {bool accept = true}) {
    return _platform.respondToWriteRequest(requestId, accept: accept);
  }
}
