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
  OmniBleConnectionConfig? lastConnectionConfig;
  String? lastConnectedDeviceId;
  Set<OmniBlePermission>? lastRationalePermissions;
  bool didOpenAppSettings = false;
  bool didOpenBluetoothSettings = false;
  String? lastRequestMtuDeviceId;
  int? lastRequestedMtu;
  String? lastPriorityDeviceId;
  OmniBleConnectionPriority? lastConnectionPriority;
  String? lastPreferredPhyDeviceId;
  OmniBlePhy? lastTxPhy;
  OmniBlePhy? lastRxPhy;
  OmniBlePhyCoding? lastPhyCoding;

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
  Future<Map<OmniBlePermission, bool>> shouldShowRequestRationale(
    Set<OmniBlePermission> permissions,
  ) async {
    lastRationalePermissions = permissions;
    return {
      for (final permission in permissions)
        permission: permission == OmniBlePermission.scan,
    };
  }

  @override
  Future<bool> openAppSettings() async {
    didOpenAppSettings = true;
    return true;
  }

  @override
  Future<bool> openBluetoothSettings() async {
    didOpenBluetoothSettings = true;
    return true;
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
  Future<void> connect(
    String deviceId, {
    OmniBleConnectionConfig config = const OmniBleConnectionConfig(),
  }) async {
    lastConnectedDeviceId = deviceId;
    lastConnectionConfig = config;
  }

  @override
  Future<void> disconnect(String deviceId) async {}

  @override
  Future<List<OmniBleGattService>> discoverServices(String deviceId) async {
    return const [];
  }

  @override
  Future<int> readRssi(String deviceId) async {
    return -64;
  }

  @override
  Future<int> requestMtu(String deviceId, {int mtu = 512}) async {
    lastRequestMtuDeviceId = deviceId;
    lastRequestedMtu = mtu;
    return 247;
  }

  @override
  Future<void> requestConnectionPriority(
    String deviceId,
    OmniBleConnectionPriority priority,
  ) async {
    lastPriorityDeviceId = deviceId;
    lastConnectionPriority = priority;
  }

  @override
  Future<void> setPreferredPhy(
    String deviceId, {
    OmniBlePhy txPhy = OmniBlePhy.le1m,
    OmniBlePhy rxPhy = OmniBlePhy.le1m,
    OmniBlePhyCoding coding = OmniBlePhyCoding.unspecified,
  }) async {
    lastPreferredPhyDeviceId = deviceId;
    lastTxPhy = txPhy;
    lastRxPhy = rxPhy;
    lastPhyCoding = coding;
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

  test(
    'permissions.shouldShowRequestRationale forwards permission set',
    () async {
      const omniBlePlugin = OmniBle();
      final fakePlatform = MockOmniBlePlatform();
      OmniBlePlatform.instance = fakePlatform;

      final rationale = await omniBlePlugin.permissions
          .shouldShowRequestRationale({
            OmniBlePermission.scan,
            OmniBlePermission.connect,
          });

      expect(fakePlatform.lastRationalePermissions, {
        OmniBlePermission.scan,
        OmniBlePermission.connect,
      });
      expect(rationale[OmniBlePermission.scan], isTrue);
      expect(rationale[OmniBlePermission.connect], isFalse);
    },
  );

  test('permissions open settings helpers forward to platform', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    final openedAppSettings = await omniBlePlugin.permissions.openAppSettings();
    final openedBluetoothSettings = await omniBlePlugin.permissions
        .openBluetoothSettings();

    expect(openedAppSettings, isTrue);
    expect(openedBluetoothSettings, isTrue);
    expect(fakePlatform.didOpenAppSettings, isTrue);
    expect(fakePlatform.didOpenBluetoothSettings, isTrue);
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

  test('central.connect forwards connection config', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    await omniBlePlugin.central.connect(
      'device-1',
      config: const OmniBleConnectionConfig(
        timeout: Duration(seconds: 8),
        androidAutoConnect: true,
      ),
    );

    expect(fakePlatform.lastConnectedDeviceId, 'device-1');
    expect(
      fakePlatform.lastConnectionConfig?.timeout,
      const Duration(seconds: 8),
    );
    expect(fakePlatform.lastConnectionConfig?.androidAutoConnect, isTrue);
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

  test('central.readRssi forwards device id', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    final rssi = await omniBlePlugin.central.readRssi('device-1');

    expect(rssi, -64);
  });

  test(
    'central.requestMtu forwards payload and returns negotiated value',
    () async {
      const omniBlePlugin = OmniBle();
      final fakePlatform = MockOmniBlePlatform();
      OmniBlePlatform.instance = fakePlatform;

      final mtu = await omniBlePlugin.central.requestMtu('device-1', mtu: 247);

      expect(mtu, 247);
      expect(fakePlatform.lastRequestMtuDeviceId, 'device-1');
      expect(fakePlatform.lastRequestedMtu, 247);
    },
  );

  test('central.requestConnectionPriority forwards priority', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    await omniBlePlugin.central.requestConnectionPriority(
      'device-1',
      OmniBleConnectionPriority.high,
    );

    expect(fakePlatform.lastPriorityDeviceId, 'device-1');
    expect(fakePlatform.lastConnectionPriority, OmniBleConnectionPriority.high);
  });

  test('central.setPreferredPhy forwards PHY settings', () async {
    const omniBlePlugin = OmniBle();
    final fakePlatform = MockOmniBlePlatform();
    OmniBlePlatform.instance = fakePlatform;

    await omniBlePlugin.central.setPreferredPhy(
      'device-1',
      txPhy: OmniBlePhy.le2m,
      rxPhy: OmniBlePhy.leCoded,
      coding: OmniBlePhyCoding.s2,
    );

    expect(fakePlatform.lastPreferredPhyDeviceId, 'device-1');
    expect(fakePlatform.lastTxPhy, OmniBlePhy.le2m);
    expect(fakePlatform.lastRxPhy, OmniBlePhy.leCoded);
    expect(fakePlatform.lastPhyCoding, OmniBlePhyCoding.s2);
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

  test('OmniBleEvent parses mtu changes', () {
    final event = OmniBleEvent.fromMap({
      'type': 'mtuChanged',
      'deviceId': 'device-1',
      'mtu': 247,
      'status': 0,
    });

    expect(event, isA<OmniBleMtuChangedEvent>());
    expect((event as OmniBleMtuChangedEvent).deviceId, 'device-1');
    expect(event.mtu, 247);
    expect(event.status, 0);
  });

  test('OmniBleEvent parses phy updates', () {
    final event = OmniBleEvent.fromMap({
      'type': 'phyUpdated',
      'deviceId': 'device-1',
      'txPhy': 'le2m',
      'rxPhy': 'leCoded',
      'status': 0,
    });

    expect(event, isA<OmniBlePhyUpdatedEvent>());
    expect((event as OmniBlePhyUpdatedEvent).deviceId, 'device-1');
    expect(event.txPhy, OmniBlePhy.le2m);
    expect(event.rxPhy, OmniBlePhy.leCoded);
    expect(event.status, 0);
  });

  test('OmniBleEvent parses notification queue readiness', () {
    final event = OmniBleEvent.fromMap({
      'type': 'notificationQueueReady',
      'deviceId': 'device-1',
      'status': 0,
    });

    expect(event, isA<OmniBleNotificationQueueReadyEvent>());
    expect((event as OmniBleNotificationQueueReadyEvent).deviceId, 'device-1');
    expect(event.status, 0);
  });

  test('OmniBleEvent parses scan errors', () {
    final event = OmniBleEvent.fromMap({
      'type': 'scanError',
      'code': 3,
      'message': 'Scan registration failed',
    });

    expect(event, isA<OmniBleScanErrorEvent>());
    expect((event as OmniBleScanErrorEvent).code, 3);
    expect(event.message, 'Scan registration failed');
  });

  test('OmniBleEvent parses read requests', () {
    final event = OmniBleEvent.fromMap({
      'type': 'readRequest',
      'requestId': 'request-1',
      'deviceId': 'device-1',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
      'offset': 4,
    });

    expect(event, isA<OmniBleReadRequestEvent>());
    expect((event as OmniBleReadRequestEvent).requestId, 'request-1');
    expect(event.deviceId, 'device-1');
    expect(event.offset, 4);
  });

  test('OmniBleEvent parses write requests', () {
    final event = OmniBleEvent.fromMap({
      'type': 'writeRequest',
      'requestId': 'request-2',
      'deviceId': 'device-2',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
      'offset': 2,
      'preparedWrite': true,
      'responseNeeded': false,
      'value': [9, 8, 7],
    });

    expect(event, isA<OmniBleWriteRequestEvent>());
    expect((event as OmniBleWriteRequestEvent).requestId, 'request-2');
    expect(event.offset, 2);
    expect(event.preparedWrite, isTrue);
    expect(event.responseNeeded, isFalse);
    expect(event.value, Uint8List.fromList([9, 8, 7]));
  });

  test('OmniBleEvent parses subscription changes', () {
    final event = OmniBleEvent.fromMap({
      'type': 'subscriptionChanged',
      'deviceId': 'device-3',
      'serviceUuid': '0000180d-0000-1000-8000-00805f9b34fb',
      'characteristicUuid': '00002a37-0000-1000-8000-00805f9b34fb',
      'subscribed': true,
    });

    expect(event, isA<OmniBleSubscriptionChanged>());
    expect((event as OmniBleSubscriptionChanged).deviceId, 'device-3');
    expect(event.subscribed, isTrue);
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
