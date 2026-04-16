# omni_ble

A cross-platform Flutter plugin scaffold for Bluetooth LE central and peripheral roles.

`omni_ble` is set up to grow into a single Dart-first API that targets iOS, macOS, Android, Windows, and Linux.

The current state is an early but usable cross-platform BLE foundation:

- Dart-side public API is defined for both central and peripheral workflows.
- Platform channels are wired on every target.
- iOS and macOS now implement adapter-state events, central scanning, connection state events, RSSI reads, service discovery, characteristic/descriptor read-write, notification subscriptions, and a first peripheral backend.
- Android now implements the same central-side surface plus a first peripheral backend, and the Dart API can now check/request the runtime Bluetooth permissions those flows need.
- Linux now exposes adapter-state events and native LE scanning via BlueZ, and Windows now exposes adapter-state events plus native LE scanning via the WinRT advertisement watcher. The desktop permission API continues to return `notRequired`.

## API shape

```dart
import 'dart:typed_data';

import 'package:omni_ble/omni_ble.dart';

const ble = OmniBle();

Future<void> centralFlow() async {
  final capabilities = await ble.getCapabilities();
  if (!capabilities.supports(OmniBleFeature.scanning)) return;

  final permissionStatus = await ble.permissions.request({
    OmniBlePermission.scan,
  });
  if (!permissionStatus.isGranted(OmniBlePermission.scan)) return;

  await ble.central.startScan(
    config: const OmniBleScanConfig(serviceUuids: ['180D']),
  );
}

Future<void> peripheralFlow() async {
  await ble.peripheral.publishGattDatabase(
    const OmniBleGattDatabase(
      services: [
        OmniBleGattService(
          uuid: '180D',
          characteristics: [
            OmniBleGattCharacteristic(
              uuid: '2A37',
              properties: {
                OmniBleGattProperty.notify,
              },
            ),
          ],
        ),
      ],
    ),
  );

  await ble.peripheral.startAdvertising(
    const OmniBleAdvertisement(
      localName: 'omni_ble',
      serviceUuids: ['180D'],
    ),
  );

  await ble.peripheral.notifyCharacteristicValue(
    const OmniBleCharacteristicAddress(
      serviceUuid: '180D',
      characteristicUuid: '2A37',
    ),
    Uint8List.fromList([72]),
  );
}
```

## Package layout

- `lib/omni_ble.dart`: public central/peripheral facade
- `lib/src/omni_ble_models.dart`: shared BLE models and event types
- `lib/omni_ble_method_channel.dart`: method-channel bridge and error mapping
- `android/`, `ios/`, `macos/`, `windows/`, `linux/`: native stubs for the plugin

## Current coverage

- `iOS`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `connect()`, `disconnect()`, connection state events, `readRssi()`, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()`, `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()` (`notRequired`)
- `macOS`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `connect()`, `disconnect()`, connection state events, `readRssi()`, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()`, `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()` (`notRequired`)
- `Android`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `scanError` events, `connect()`, `disconnect()`, connection state events, `readRssi()`, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()` (mode / tx power / timeout options), `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()`
- `Windows`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `scanError` events, `checkPermissions()`, `requestPermissions()` (`notRequired`)
- `Linux`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `checkPermissions()`, `requestPermissions()` (`notRequired`)

Peripheral request events now include the request offset on Apple and Android, Android write requests also include `preparedWrite` / `responseNeeded` metadata when the platform provides it, and Apple / Android now surface `notificationQueueReady` to help pace server-side notifications.

## Android permissions

- The plugin manifest declares BLE scan/connect/advertise permissions.
- `ble.permissions.check({...})` reports whether runtime BLE permissions are already available.
- `ble.permissions.request({...})` triggers the Android permission prompt when an activity is attached.
- Android 12+ still requires runtime approval for `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, and `BLUETOOTH_ADVERTISE`.
- Android 11 and below still requires runtime approval for location before scanning.
- Android scan failures are also surfaced through the shared event stream as `OmniBleScanErrorEvent`.
- On non-Android targets, the permission API returns `notRequired` for the requested BLE permissions.

## Recommended next steps

1. Harden the peripheral implementation with richer advertising options and subscription/backpressure ergonomics.
2. Harden the central implementation with MTU/PHY options, richer connection tuning, and more integration coverage.
3. Bring Windows and Linux onto either native stacks or a well-scoped abstraction layer.
4. Add higher-level convenience helpers for permission rationale / settings recovery on Android.
