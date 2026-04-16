import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'omni_ble_method_channel.dart';
import 'src/omni_ble_models.dart';

abstract class OmniBlePlatform extends PlatformInterface {
  /// Constructs a OmniBlePlatform.
  OmniBlePlatform() : super(token: _token);

  static final Object _token = Object();

  static OmniBlePlatform _instance = MethodChannelOmniBle();

  /// The default instance of [OmniBlePlatform] to use.
  ///
  /// Defaults to [MethodChannelOmniBle].
  static OmniBlePlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [OmniBlePlatform] when
  /// they register themselves.
  static set instance(OmniBlePlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Stream<OmniBleEvent> observeEvents();

  Future<OmniBleCapabilities> getCapabilities();

  Future<OmniBlePermissionStatus> checkPermissions(
    Set<OmniBlePermission> permissions,
  );

  Future<OmniBlePermissionStatus> requestPermissions(
    Set<OmniBlePermission> permissions,
  );

  Future<void> startScan(OmniBleScanConfig config);

  Future<void> stopScan();

  Future<void> connect(String deviceId, {OmniBleConnectionConfig config});

  Future<void> disconnect(String deviceId);

  Future<List<OmniBleGattService>> discoverServices(String deviceId);

  Future<int> readRssi(String deviceId);

  Future<int> requestMtu(String deviceId, {int mtu});

  Future<void> requestConnectionPriority(
    String deviceId,
    OmniBleConnectionPriority priority,
  );

  Future<void> setPreferredPhy(
    String deviceId, {
    OmniBlePhy txPhy,
    OmniBlePhy rxPhy,
    OmniBlePhyCoding coding,
  });

  Future<Uint8List> readCharacteristic(OmniBleCharacteristicAddress address);

  Future<Uint8List> readDescriptor(OmniBleDescriptorAddress address);

  Future<void> writeCharacteristic(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    OmniBleWriteType type,
  });

  Future<void> writeDescriptor(
    OmniBleDescriptorAddress address,
    Uint8List value,
  );

  Future<void> setNotification(
    OmniBleCharacteristicAddress address, {
    required bool enabled,
  });

  Future<void> publishGattDatabase(OmniBleGattDatabase database);

  Future<void> clearGattDatabase();

  Future<void> startAdvertising(OmniBleAdvertisement advertisement);

  Future<void> stopAdvertising();

  Future<void> notifyCharacteristicValue(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    String? deviceId,
  });

  Future<void> respondToReadRequest(String requestId, Uint8List value);

  Future<void> respondToWriteRequest(String requestId, {bool accept});
}
