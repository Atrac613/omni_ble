import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:omni_ble/omni_ble.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockOmniBlePlatform
    with MockPlatformInterfaceMixin
    implements OmniBlePlatform {
  @override
  Stream<OmniBleEvent> observeEvents() => const Stream<OmniBleEvent>.empty();

  OmniBleScanConfig? lastScanConfig;
  OmniBleGattDatabase? lastDatabase;
  Set<OmniBlePermission>? lastRequestedPermissions;

  @override
  Future<OmniBlePermissionStatus> checkPermissions(
    Set<OmniBlePermission> permissions,
  ) async {
    return OmniBlePermissionStatus(
      permissions: {
        for (final permission in permissions)
          permission: OmniBlePermissionState.notRequired,
      },
    );
  }

  @override
  Future<OmniBlePermissionStatus> requestPermissions(
    Set<OmniBlePermission> permissions,
  ) async {
    lastRequestedPermissions = permissions;
    return OmniBlePermissionStatus(
      permissions: {
        for (final permission in permissions)
          permission: OmniBlePermissionState.granted,
      },
    );
  }

  @override
  Future<OmniBleCapabilities> getCapabilities() async {
    return const OmniBleCapabilities(
      platform: 'test',
      platformVersion: '1.0',
      availableFeatures: {OmniBleFeature.central},
    );
  }

  @override
  Future<void> startScan(OmniBleScanConfig config) async {
    lastScanConfig = config;
  }

  @override
  Future<void> stopScan() async {}

  @override
  Future<void> connect(String deviceId, {Duration? timeout}) async {}

  @override
  Future<void> disconnect(String deviceId) async {}

  @override
  Future<List<OmniBleGattService>> discoverServices(String deviceId) async {
    return const [];
  }

  @override
  Future<Uint8List> readCharacteristic(
    OmniBleCharacteristicAddress address,
  ) async {
    return Uint8List.fromList([1, 2, 3]);
  }

  @override
  Future<Uint8List> readDescriptor(OmniBleDescriptorAddress address) async {
    return Uint8List.fromList([4, 5, 6]);
  }

  @override
  Future<void> writeCharacteristic(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    OmniBleWriteType type = OmniBleWriteType.withResponse,
  }) async {}

  @override
  Future<void> writeDescriptor(
    OmniBleDescriptorAddress address,
    Uint8List value,
  ) async {}

  @override
  Future<void> setNotification(
    OmniBleCharacteristicAddress address, {
    required bool enabled,
  }) async {}

  @override
  Future<void> publishGattDatabase(OmniBleGattDatabase database) async {
    lastDatabase = database;
  }

  @override
  Future<void> clearGattDatabase() async {}

  @override
  Future<void> startAdvertising(OmniBleAdvertisement advertisement) async {}

  @override
  Future<void> stopAdvertising() async {}

  @override
  Future<void> notifyCharacteristicValue(
    OmniBleCharacteristicAddress address,
    Uint8List value, {
    String? deviceId,
  }) async {}

  @override
  Future<void> respondToReadRequest(String requestId, Uint8List value) async {}

  @override
  Future<void> respondToWriteRequest(
    String requestId, {
    bool accept = true,
  }) async {}
}

void main() {
  final OmniBlePlatform initialPlatform = OmniBlePlatform.instance;

  tearDown(() {
    OmniBlePlatform.instance = initialPlatform;
  });

  test('$MethodChannelOmniBle is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelOmniBle>());
  });

  test('getCapabilities', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    final capabilities = await omniBlePlugin.getCapabilities();

    expect(capabilities.platform, 'test');
    expect(capabilities.supports(OmniBleFeature.central), isTrue);
  });

  test('permissions.request forwards permission set', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    final status = await omniBlePlugin.permissions.request({
      OmniBlePermission.scan,
      OmniBlePermission.connect,
    });

    expect(fakePlatform.lastRequestedPermissions, {
      OmniBlePermission.scan,
      OmniBlePermission.connect,
    });
    expect(status.allGranted, isTrue);
    expect(status.isGranted(OmniBlePermission.scan), isTrue);
  });

  test('central.startScan forwards configuration', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    await omniBlePlugin.central.startScan(
      config: const OmniBleScanConfig(
        serviceUuids: ['180D'],
        allowDuplicates: true,
      ),
    );

    expect(fakePlatform.lastScanConfig?.serviceUuids, ['180D']);
    expect(fakePlatform.lastScanConfig?.allowDuplicates, isTrue);
  });

  test('central.readDescriptor forwards descriptor address', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    final value = await omniBlePlugin.central.readDescriptor(
      const OmniBleDescriptorAddress(
        deviceId: 'device-1',
        serviceUuid: '180D',
        characteristicUuid: '2A37',
        descriptorUuid: '2902',
      ),
    );

    expect(value, Uint8List.fromList([4, 5, 6]));
  });

  test('OmniBleEvent parses connection state changes', () {
    final event = OmniBleEvent.fromMap({
      'type': 'connectionStateChanged',
      'deviceId': 'device-1',
      'state': 'connected',
    });

    expect(event, isA<OmniBleConnectionStateChanged>());
    expect((event as OmniBleConnectionStateChanged).deviceId, 'device-1');
    expect(event.state, OmniBleConnectionState.connected);
  });

  test('OmniBleEvent parses characteristic value changes', () {
    final event = OmniBleEvent.fromMap({
      'type': 'characteristicValueChanged',
      'deviceId': 'device-1',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
      'value': [1, 2, 3],
    });

    expect(event, isA<OmniBleCharacteristicValueChanged>());
    expect(
      (event as OmniBleCharacteristicValueChanged).address.deviceId,
      'device-1',
    );
    expect(event.value, Uint8List.fromList([1, 2, 3]));
  });

  test('OmniBleEvent parses read requests', () {
    final event = OmniBleEvent.fromMap({
      'type': 'readRequest',
      'requestId': 'request-1',
      'deviceId': 'device-1',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
    });

    expect(event, isA<OmniBleReadRequestEvent>());
    expect((event as OmniBleReadRequestEvent).requestId, 'request-1');
    expect(event.deviceId, 'device-1');
  });

  test('OmniBleEvent parses write requests', () {
    final event = OmniBleEvent.fromMap({
      'type': 'writeRequest',
      'requestId': 'request-2',
      'deviceId': 'device-2',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
      'value': [9, 8, 7],
    });

    expect(event, isA<OmniBleWriteRequestEvent>());
    expect((event as OmniBleWriteRequestEvent).requestId, 'request-2');
    expect(event.value, Uint8List.fromList([9, 8, 7]));
  });

  test('peripheral.publishGattDatabase forwards services', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    await omniBlePlugin.peripheral.publishGattDatabase(
      const OmniBleGattDatabase(services: [OmniBleGattService(uuid: '180D')]),
    );

    expect(fakePlatform.lastDatabase?.services.single.uuid, '180D');
  });
}
