import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'omni_ble_platform_interface.dart';
import 'src/omni_ble_exception.dart';
import 'src/omni_ble_models.dart';

/// An implementation of [OmniBlePlatform] that uses method channels.
class MethodChannelOmniBle extends OmniBlePlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('omni_ble/methods');

  @visibleForTesting
  final eventChannel = const EventChannel('omni_ble/events');

  @override
  Stream<OmniBleEvent> observeEvents() async* {
    try {
      await for (final event in eventChannel.receiveBroadcastStream()) {
        final map = _eventMap(event);
        if (map != null) {
          yield OmniBleEvent.fromMap(map);
        }
      }
    } on PlatformException {
      return;
    } on MissingPluginException {
      return;
    }
  }

  @override
  Future<OmniBleCapabilities> getCapabilities() async {
    final response = await _invokeMap('getCapabilities');
    return OmniBleCapabilities.fromMap(response);
  }

  @override
  Future<OmniBlePermissionStatus> checkPermissions(
    Set<OmniBlePermission> permissions,
  ) async {
    final response = await _invokeMap('checkPermissions', {
      'permissions': permissions.map((permission) => permission.value).toList(),
    });
    return OmniBlePermissionStatus.fromMap(response);
  }

  @override
  Future<OmniBlePermissionStatus> requestPermissions(
    Set<OmniBlePermission> permissions,
  ) async {
    final response = await _invokeMap('requestPermissions', {
      'permissions': permissions.map((permission) => permission.value).toList(),
    });
    return OmniBlePermissionStatus.fromMap(response);
  }

  @override
  Future<Map<OmniBlePermission, bool>> shouldShowRequestRationale(
    Set<OmniBlePermission> permissions,
  ) async {
    final response = await _invokeMap('shouldShowRequestRationale', {
      'permissions': permissions.map((permission) => permission.value).toList(),
    });
    final rationaleMap = response['permissions'] is Map
        ? Map<Object?, Object?>.from(response['permissions'] as Map)
        : const <Object?, Object?>{};
    return rationaleMap.map(
      (key, value) => MapEntry(
        OmniBlePermission.values.firstWhere(
          (permission) => permission.value == key.toString(),
          orElse: () =>
              throw ArgumentError.value(key, 'key', 'Unknown BLE permission.'),
        ),
        value == true,
      ),
    );
  }

  @override
  Future<bool> openAppSettings() {
    return _invokeBool('openAppSettings');
  }

  @override
  Future<bool> openBluetoothSettings() {
    return _invokeBool('openBluetoothSettings');
  }

  @override
  Future<void> startScan(OmniBleScanConfig config) {
    return _invokeVoid('startScan', config.toMap());
  }

  @override
  Future<void> stopScan() {
    return _invokeVoid('stopScan');
  }

  @override
  Future<void> connect(
    String deviceId, {
    OmniBleConnectionConfig config = const OmniBleConnectionConfig(),
  }) {
    return _invokeVoid('connect', {'deviceId': deviceId, ...config.toMap()});
  }

  @override
  Future<void> disconnect(String deviceId) {
    return _invokeVoid('disconnect', {'deviceId': deviceId});
  }

  @override
  Future<List<OmniBleGattService>> discoverServices(String deviceId) async {
    final response = await _invokeList('discoverServices', {
      'deviceId': deviceId,
    });
    return response
        .whereType<Map>()
        .map((service) => Map<Object?, Object?>.from(service))
        .map(OmniBleGattService.fromMap)
        .toList(growable: false);
  }

  @override
  Future<int> readRssi(String deviceId) async {
    return _invokeInt('readRssi', {'deviceId': deviceId});
  }

  @override
  Future<int> requestMtu(String deviceId, {int mtu = 512}) async {
    return _invokeInt('requestMtu', {'deviceId': deviceId, 'mtu': mtu});
  }

  @override
  Future<void> requestConnectionPriority(
    String deviceId,
    OmniBleConnectionPriority priority,
  ) {
    return _invokeVoid('requestConnectionPriority', {
      'deviceId': deviceId,
      'priority': priority.value,
    });
  }

  @override
  Future<void> setPreferredPhy(
    String deviceId, {
    OmniBlePhy txPhy = OmniBlePhy.le1m,
    OmniBlePhy rxPhy = OmniBlePhy.le1m,
    OmniBlePhyCoding coding = OmniBlePhyCoding.unspecified,
  }) {
    return _invokeVoid('setPreferredPhy', {
      'deviceId': deviceId,
      'txPhy': txPhy.value,
      'rxPhy': rxPhy.value,
      'coding': coding.value,
    });
  }

  Future<int> _invokeInt(
    String method, [
    Map<String, Object?>? arguments,
  ]) async {
    try {
      final response = await methodChannel.invokeMethod<Object?>(
        method,
        arguments,
      );
      if (response is num) {
        return response.toInt();
      }
      throw OmniBleException(
        code: 'invalid-response',
        message: 'Expected a number from `$method`, but received $response.',
      );
    } on PlatformException catch (error) {
      throw OmniBleException(
        code: error.code,
        message: error.message ?? 'Native platform call failed.',
        details: error.details,
      );
    } on MissingPluginException {
      throw OmniBleException(
        code: 'unimplemented',
        message: 'The native method `$method` has not been implemented yet.',
      );
    }
  }

  Future<bool> _invokeBool(
    String method, [
    Map<String, Object?>? arguments,
  ]) async {
    try {
      final response = await methodChannel.invokeMethod<Object?>(
        method,
        arguments,
      );
      if (response is bool) {
        return response;
      }
      throw OmniBleException(
        code: 'invalid-response',
        message: 'Expected a bool from `$method`, but received $response.',
      );
    } on PlatformException catch (error) {
      throw OmniBleException(
        code: error.code,
        message: error.message ?? 'Native platform call failed.',
        details: error.details,
      );
    } on MissingPluginException {
      throw OmniBleException(
        code: 'unimplemented',
        message: 'The native method `$method` has not been implemented yet.',
      );
    }
  }

  @override
  Future<Uint8List> readCharacteristic(
    OmniBleCharacteristicAddress address,
  ) async {
    final response = await _invokeList('readCharacteristic', address.toMap());
    return _bytesFromDynamic(response);
  }

  @override
  Future<Uint8List> readDescriptor(OmniBleDescriptorAddress address) async {
    final response = await _invokeList('readDescriptor', address.toMap());
    return _bytesFromDynamic(response);
  }

  @override
  Future<void> writeCharacteristic(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    OmniBleWriteType type = OmniBleWriteType.withResponse,
  }) {
    return _invokeVoid('writeCharacteristic', {
      ...address.toMap(),
      'value': value,
      'writeType': type.value,
    });
  }

  @override
  Future<void> writeDescriptor(
    OmniBleDescriptorAddress address,
    Uint8List value,
  ) {
    return _invokeVoid('writeDescriptor', {...address.toMap(), 'value': value});
  }

  @override
  Future<void> setNotification(
    OmniBleCharacteristicAddress address, {
    required bool enabled,
  }) {
    return _invokeVoid('setNotification', {
      ...address.toMap(),
      'enabled': enabled,
    });
  }

  @override
  Future<void> publishGattDatabase(OmniBleGattDatabase database) {
    return _invokeVoid('publishGattDatabase', database.toMap());
  }

  @override
  Future<void> clearGattDatabase() {
    return _invokeVoid('clearGattDatabase');
  }

  @override
  Future<void> startAdvertising(OmniBleAdvertisement advertisement) {
    return _invokeVoid('startAdvertising', advertisement.toMap());
  }

  @override
  Future<void> stopAdvertising() {
    return _invokeVoid('stopAdvertising');
  }

  @override
  Future<void> notifyCharacteristicValue(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    String? deviceId,
  }) {
    return _invokeVoid('notifyCharacteristicValue', {
      ...address.toMap(),
      'value': value,
      'deviceId': deviceId,
    });
  }

  @override
  Future<void> respondToReadRequest(String requestId, Uint8List value) {
    return _invokeVoid('respondToReadRequest', {
      'requestId': requestId,
      'value': value,
    });
  }

  @override
  Future<void> respondToWriteRequest(String requestId, {bool accept = true}) {
    return _invokeVoid('respondToWriteRequest', {
      'requestId': requestId,
      'accept': accept,
    });
  }

  Future<void> _invokeVoid(
    String method, [
    Map<String, Object?>? arguments,
  ]) async {
    try {
      await methodChannel.invokeMethod<void>(method, arguments);
    } on PlatformException catch (error) {
      throw OmniBleException(
        code: error.code,
        message: error.message ?? 'Native platform call failed.',
        details: error.details,
      );
    } on MissingPluginException {
      throw OmniBleException(
        code: 'unimplemented',
        message: 'The native method `$method` has not been implemented yet.',
      );
    }
  }

  Future<Map<Object?, Object?>> _invokeMap(
    String method, [
    Map<String, Object?>? arguments,
  ]) async {
    try {
      final response = await methodChannel.invokeMethod<Object?>(
        method,
        arguments,
      );
      if (response is Map<Object?, Object?>) {
        return response;
      }
      throw OmniBleException(
        code: 'invalid-response',
        message: 'Expected a map from `$method`, but received $response.',
      );
    } on PlatformException catch (error) {
      throw OmniBleException(
        code: error.code,
        message: error.message ?? 'Native platform call failed.',
        details: error.details,
      );
    } on MissingPluginException {
      throw OmniBleException(
        code: 'unimplemented',
        message: 'The native method `$method` has not been implemented yet.',
      );
    }
  }

  Future<List<Object?>> _invokeList(
    String method, [
    Map<String, Object?>? arguments,
  ]) async {
    try {
      final response = await methodChannel.invokeMethod<Object?>(
        method,
        arguments,
      );
      if (response is Uint8List) {
        return response.toList(growable: false);
      }
      if (response is List<Object?>) {
        return response;
      }
      throw OmniBleException(
        code: 'invalid-response',
        message: 'Expected a list from `$method`, but received $response.',
      );
    } on PlatformException catch (error) {
      throw OmniBleException(
        code: error.code,
        message: error.message ?? 'Native platform call failed.',
        details: error.details,
      );
    } on MissingPluginException {
      throw OmniBleException(
        code: 'unimplemented',
        message: 'The native method `$method` has not been implemented yet.',
      );
    }
  }

  Uint8List _bytesFromDynamic(List<Object?> values) {
    return Uint8List.fromList(
      values.map((value) => (value as num).toInt()).toList(growable: false),
    );
  }

  Map<Object?, Object?>? _eventMap(Object? event) {
    if (event is Map) {
      return Map<Object?, Object?>.from(event);
    }
    return null;
  }
}
