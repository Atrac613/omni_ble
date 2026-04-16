import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:omni_ble/omni_ble.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  final platform = MethodChannelOmniBle();
  const channel = MethodChannel('omni_ble/methods');
  MethodCall? lastCall;

  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
          lastCall = methodCall;
          switch (methodCall.method) {
            case 'getCapabilities':
              return {
                'platform': 'android',
                'platformVersion': '16',
                'availableFeatures': ['central', 'scanning'],
              };
            case 'checkPermissions':
              return {
                'permissions': {'scan': 'granted', 'connect': 'denied'},
                'allGranted': false,
              };
            case 'requestPermissions':
              return {
                'permissions': {'scan': 'granted', 'connect': 'granted'},
                'allGranted': true,
              };
            case 'readRssi':
              return -58;
            case 'readCharacteristic':
              return Uint8List.fromList([1, 2, 3]);
            case 'readDescriptor':
              return Uint8List.fromList([4, 5]);
            case 'discoverServices':
              return [
                {
                  'uuid': '0000180d-0000-1000-8000-00805f9b34fb',
                  'primary': true,
                  'characteristics': [
                    {
                      'uuid': '00002a37-0000-1000-8000-00805f9b34fb',
                      'properties': ['read', 'notify'],
                      'permissions': [],
                      'descriptors': [
                        {
                          'uuid': '00002902-0000-1000-8000-00805f9b34fb',
                          'permissions': [],
                          'initialValue': [1, 0],
                        },
                      ],
                    },
                  ],
                },
              ];
            case 'connect':
              throw PlatformException(
                code: 'permission-denied',
                message: 'No permission',
              );
            default:
              return null;
          }
        });
  });

  tearDown(() {
    lastCall = null;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('getCapabilities decodes feature list', () async {
    final capabilities = await platform.getCapabilities();

    expect(capabilities.platform, 'android');
    expect(capabilities.platformVersion, '16');
    expect(capabilities.supports(OmniBleFeature.central), isTrue);
    expect(capabilities.supports(OmniBleFeature.scanning), isTrue);
  });

  test('checkPermissions decodes permission states', () async {
    final status = await platform.checkPermissions({
      OmniBlePermission.scan,
      OmniBlePermission.connect,
    });

    expect(lastCall?.method, 'checkPermissions');
    expect(lastCall?.arguments, {
      'permissions': ['scan', 'connect'],
    });
    expect(status.isGranted(OmniBlePermission.scan), isTrue);
    expect(status.isGranted(OmniBlePermission.connect), isFalse);
    expect(status.allGranted, isFalse);
  });

  test('startScan encodes config payload', () async {
    await platform.startScan(
      const OmniBleScanConfig(serviceUuids: ['180D'], allowDuplicates: true),
    );

    expect(lastCall?.method, 'startScan');
    expect(lastCall?.arguments, {
      'serviceUuids': ['180D'],
      'allowDuplicates': true,
    });
  });

  test('readRssi decodes integer payload', () async {
    final rssi = await platform.readRssi('device-1');

    expect(rssi, -58);
    expect(lastCall?.method, 'readRssi');
    expect(lastCall?.arguments, {'deviceId': 'device-1'});
  });

  test('readCharacteristic decodes bytes', () async {
    final value = await platform.readCharacteristic(
      const OmniBleCharacteristicAddress(
        deviceId: 'device-1',
        serviceUuid: '180D',
        characteristicUuid: '2A37',
      ),
    );

    expect(value, Uint8List.fromList([1, 2, 3]));
  });

  test('readDescriptor decodes bytes', () async {
    final value = await platform.readDescriptor(
      const OmniBleDescriptorAddress(
        deviceId: 'device-1',
        serviceUuid: '180D',
        characteristicUuid: '2A37',
        descriptorUuid: '2902',
      ),
    );

    expect(value, Uint8List.fromList([4, 5]));
    expect(lastCall?.method, 'readDescriptor');
    expect(lastCall?.arguments, {
      'deviceId': 'device-1',
      'serviceUuid': '180D',
      'characteristicUuid': '2A37',
      'descriptorUuid': '2902',
    });
  });

  test('discoverServices decodes GATT payloads', () async {
    final services = await platform.discoverServices('device-1');

    expect(services, hasLength(1));
    expect(services.single.uuid, '0000180d-0000-1000-8000-00805f9b34fb');
    expect(
      services.single.characteristics.single.properties,
      containsAll({OmniBleGattProperty.read, OmniBleGattProperty.notify}),
    );
    expect(
      services.single.characteristics.single.descriptors.single.uuid,
      '00002902-0000-1000-8000-00805f9b34fb',
    );
    expect(
      services.single.characteristics.single.descriptors.single.initialValue,
      Uint8List.fromList([1, 0]),
    );
  });

  test('writeCharacteristic encodes write payload', () async {
    await platform.writeCharacteristic(
      const OmniBleCharacteristicAddress(
        deviceId: 'device-1',
        serviceUuid: '180D',
        characteristicUuid: '2A37',
      ),
      Uint8List.fromList([4, 5, 6]),
      type: OmniBleWriteType.withoutResponse,
    );

    expect(lastCall?.method, 'writeCharacteristic');
    expect(lastCall?.arguments['deviceId'], 'device-1');
    expect(lastCall?.arguments['serviceUuid'], '180D');
    expect(lastCall?.arguments['characteristicUuid'], '2A37');
    expect(lastCall?.arguments['writeType'], 'withoutResponse');
    expect(lastCall?.arguments['value'], Uint8List.fromList([4, 5, 6]));
  });

  test('writeDescriptor encodes write payload', () async {
    await platform.writeDescriptor(
      const OmniBleDescriptorAddress(
        deviceId: 'device-1',
        serviceUuid: '180D',
        characteristicUuid: '2A37',
        descriptorUuid: '2902',
      ),
      Uint8List.fromList([1, 0]),
    );

    expect(lastCall?.method, 'writeDescriptor');
    expect(lastCall?.arguments['deviceId'], 'device-1');
    expect(lastCall?.arguments['serviceUuid'], '180D');
    expect(lastCall?.arguments['characteristicUuid'], '2A37');
    expect(lastCall?.arguments['descriptorUuid'], '2902');
    expect(lastCall?.arguments['value'], Uint8List.fromList([1, 0]));
  });

  test('publishGattDatabase encodes nested services', () async {
    await platform.publishGattDatabase(
      const OmniBleGattDatabase(
        services: [
          OmniBleGattService(
            uuid: '180D',
            characteristics: [
              OmniBleGattCharacteristic(
                uuid: '2A37',
                properties: {OmniBleGattProperty.notify},
              ),
            ],
          ),
        ],
      ),
    );

    expect(lastCall?.method, 'publishGattDatabase');
    expect((lastCall?.arguments['services'] as List).single['uuid'], '180D');
  });

  test('startAdvertising encodes advertisement payload', () async {
    await platform.startAdvertising(
      OmniBleAdvertisement(
        localName: 'omni_ble',
        serviceUuids: const ['180D'],
        manufacturerData: Uint8List.fromList([0x34, 0x12, 0x01]),
        includeTxPowerLevel: true,
        androidMode: OmniBleAdvertisingMode.balanced,
        androidTxPowerLevel: OmniBleAdvertisingTxPower.medium,
        timeout: const Duration(seconds: 30),
      ),
    );

    expect(lastCall?.method, 'startAdvertising');
    expect(lastCall?.arguments['localName'], 'omni_ble');
    expect(lastCall?.arguments['serviceUuids'], ['180D']);
    expect(lastCall?.arguments['includeTxPowerLevel'], isTrue);
    expect(lastCall?.arguments['androidMode'], 'balanced');
    expect(lastCall?.arguments['androidTxPowerLevel'], 'medium');
    expect(lastCall?.arguments['timeoutMs'], 30000);
    expect(
      lastCall?.arguments['manufacturerData'],
      Uint8List.fromList([0x34, 0x12, 0x01]),
    );
  });

  test('respondToReadRequest encodes response payload', () async {
    await platform.respondToReadRequest(
      'request-1',
      Uint8List.fromList([7, 8]),
    );

    expect(lastCall?.method, 'respondToReadRequest');
    expect(lastCall?.arguments['requestId'], 'request-1');
    expect(lastCall?.arguments['value'], Uint8List.fromList([7, 8]));
  });

  test('requestPermissions encodes permission payload', () async {
    final status = await platform.requestPermissions({
      OmniBlePermission.scan,
      OmniBlePermission.connect,
    });

    expect(lastCall?.method, 'requestPermissions');
    expect(lastCall?.arguments, {
      'permissions': ['scan', 'connect'],
    });
    expect(status.allGranted, isTrue);
  });

  test('platform exceptions are wrapped', () async {
    expect(
      () => platform.connect('device-1'),
      throwsA(
        isA<OmniBleException>().having(
          (error) => error.code,
          'code',
          'permission-denied',
        ),
      ),
    );
  });
}
