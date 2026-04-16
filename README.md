# omni_ble

A cross-platform Flutter plugin scaffold for Bluetooth LE central and peripheral roles.

`omni_ble` is set up to grow into a single Dart-first API that targets iOS, macOS, Android, Windows, and Linux.

The current state is an early but usable cross-platform BLE foundation:

- Dart-side public API is defined for both central and peripheral workflows.
- Platform channels are wired on every target.
- iOS and macOS now implement adapter-state events, central scanning, connection state events, RSSI reads, service discovery, characteristic/descriptor read-write, notification subscriptions, and a first peripheral backend.
- Android now implements the same central-side surface plus a first peripheral backend, including MTU / connection priority / PHY tuning helpers, and the Dart API can now check/request the runtime Bluetooth permissions those flows need alongside rationale/settings helpers.
- Linux and Windows now expose desktop central GATT client flows plus a first peripheral/server backend. The desktop permission API continues to return `notRequired`.

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
- `Android`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `scanError` events, `connect()` (timeout / autoConnect config), `disconnect()`, connection state events, `readRssi()`, `requestMtu()`, `requestConnectionPriority()`, `setPreferredPhy()`, `mtuChanged`/`phyUpdated` events, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()` (mode / tx power / timeout options), `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()`
- `Windows`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `scanError` events, `connect()`, `disconnect()`, connection state events, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()`, `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()` (`notRequired`)
- `Linux`: `getCapabilities()`, adapter state events, `startScan()`, `stopScan()`, scan result events, `connect()`, `disconnect()`, connection state events, `discoverServices()`, `readCharacteristic()`, `readDescriptor()`, `writeCharacteristic()`, `writeDescriptor()`, `setNotification()`, characteristic value change events, `publishGattDatabase()`, `clearGattDatabase()`, `startAdvertising()`, `stopAdvertising()`, `notifyCharacteristicValue()`, `readRequest`/`writeRequest`/`subscriptionChanged`/`notificationQueueReady` events, `respondToReadRequest()`, `respondToWriteRequest()`, `checkPermissions()`, `requestPermissions()` (`notRequired`)

Peripheral request events now include the request offset on Apple and Android, Android write requests also include `preparedWrite` / `responseNeeded` metadata when the platform provides it, and Apple / Android now surface `notificationQueueReady` to help pace server-side notifications.

Android central connections now accept `OmniBleConnectionConfig(timeout: ..., androidAutoConnect: ...)` and expose `requestMtu()`, `requestConnectionPriority()`, and `setPreferredPhy()` to tune a live connection. Apple targets return `unsupported` for the Android-only tuning APIs that CoreBluetooth does not expose.

Windows and Linux now advertise `peripheral` / `advertising` / `gattServer` alongside `gattClient` / `notifications`, which lets the example app run both the desktop central smoke flow and a first desktop peripheral smoke flow. `readRssi()` remains unavailable on those desktop targets for now, and BlueZ notifications are broadcast to current subscribers rather than targeted per device.

## Android permissions

- The plugin manifest declares BLE scan/connect/advertise permissions.
- `ble.permissions.check({...})` reports whether runtime BLE permissions are already available.
- `ble.permissions.request({...})` triggers the Android permission prompt when an activity is attached.
- `ble.permissions.shouldShowRequestRationale({...})` reports whether Android recommends showing an in-app rationale before re-requesting a denied permission.
- `ble.permissions.openAppSettings()` and `ble.permissions.openBluetoothSettings()` provide recovery shortcuts when the app needs to guide the user back into system settings.
- Android 12+ still requires runtime approval for `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, and `BLUETOOTH_ADVERTISE`.
- Android 11 and below still requires runtime approval for location before scanning.
- Android scan failures are also surfaced through the shared event stream as `OmniBleScanErrorEvent`.
- On non-Android targets, the permission API returns `notRequired` for the requested BLE permissions, `shouldShowRequestRationale()` returns `false`, and the settings helpers return `false`.

## Device-lab matrix

Use the example app as the operator console and verify these scenarios on real hardware:

| Host | Peer | Scenarios |
| --- | --- | --- |
| Android phone | Linux or Windows peripheral | scan, connect, discover, read/write, notify, advertise visibility |
| iPhone or iPad | Linux or Windows peripheral | scan, connect, discover, read/write, notify |
| macOS | Android or Linux peripheral | scan, connect, discover, read/write, notify |
| Windows PC | Android or Apple peripheral | scan, connect, discover, read/write, notify |
| Linux PC | Android or Apple peripheral | scan, connect, discover, read/write, notify |

Pass criteria:

- `startScan()` finds at least one known target and reports stable RSSI / advertisement metadata.
- `connect()` emits `connecting` then `connected`, and `disconnect()` tears down cleanly.
- `discoverServices()` returns a non-empty service tree for a known GATT device.
- `readCharacteristic()` / `writeCharacteristic()` and descriptor reads/writes succeed on a known test service.
- `setNotification()` causes at least one `characteristicValueChanged` event on the subscribed characteristic.
- Peripheral scenarios verify advertising start/stop, request events, responses, and server-side notifications on every backend, with the Linux caveat that subscription events are device-agnostic at the BlueZ layer.

## Recommended next steps

1. Run the device-lab matrix across Android, Apple, Windows, and Linux hardware and capture any interoperability gaps.
2. Document any platform-specific caveats that show up on real hardware, especially around BlueZ advertising and Windows local-name behavior.
