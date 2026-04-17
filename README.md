# omni_ble

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

`omni_ble` is a Flutter Bluetooth Low Energy plugin with a Dart-first API for
central and peripheral roles across iOS, macOS, Android, Windows, and Linux.

It focuses on giving Flutter apps one shared BLE surface while still exposing
the platform-specific realities that matter in production, such as Android
runtime permissions, desktop capability differences, and device-lab validation.

## Status

This repository is public and usable today as an early cross-platform BLE
package.

What it is:

- A single Dart-first API for central and peripheral BLE workflows
- Implemented native backends for Apple, Android, Windows, and Linux
- An example app that doubles as a smoke tool and device-lab operator console

What it is not:

- A Web Bluetooth package
- A classic Bluetooth package
- Fully hardware-validated across every host and peer combination yet

Implemented backend coverage and validated-on-hardware support are tracked
separately. Use [doc/device_lab_runbook.md](doc/device_lab_runbook.md) for
the operator workflow and
[doc/validated_support_matrix.md](doc/validated_support_matrix.md) for
release-facing validation status.

## Highlights

- Central scanning, connection management, service discovery, characteristic and
  descriptor IO, and notifications across all five native targets
- Peripheral GATT database publishing, advertising, request handling, and
  server-side notifications across all five native targets
- Runtime BLE permission helpers with Android-native request, rationale, and
  settings recovery flows
- Android connection tuning APIs for MTU, connection priority, and preferred
  PHY selection
- A desktop-capable example app for both central smoke flows and peripheral
  smoke flows

## Getting Started

Add the package from pub.dev:

```bash
flutter pub add omni_ble
```

Or depend on the GitHub repository before the next published release:

```yaml
dependencies:
  omni_ble:
    git:
      url: https://github.com/Atrac613/omni_ble.git
```

Then import the package:

```dart
import 'package:omni_ble/omni_ble.dart';
```

## Quick Start

```dart
import 'package:omni_ble/omni_ble.dart';

final ble = OmniBle();

Future<void> startHeartRateScan() async {
  final capabilities = await ble.getCapabilities();
  if (!capabilities.supports(OmniBleFeature.scanning)) return;

  final permissionStatus = await ble.permissions.request({
    OmniBlePermission.scan,
  });
  if (!permissionStatus.isGranted(OmniBlePermission.scan)) return;

  ble.events.listen((event) {
    if (event is OmniBleScanResultEvent) {
      print('found ${event.deviceId} ${event.name ?? ''} rssi=${event.rssi}');
    }
  });

  await ble.central.startScan(
    config: const OmniBleScanConfig(serviceUuids: ['180D']),
  );
}
```

The example app covers both central and peripheral flows and is the recommended
way to smoke-test a host before a device-lab run. See
[`example/`](example) and [`example/README.md`](example/README.md).

## API Overview

The main public entry points are:

- `OmniBle`: top-level facade for capabilities, events, and role-specific APIs
- `OmniBleCentral`: central role scanning, connection, discovery, IO, and
  notification control
- `OmniBlePeripheral`: GATT database publishing, advertising, server-side
  notifications, and request responses
- `OmniBlePermissions`: runtime permission checks, requests, rationale, and
  settings recovery helpers
- `OmniBleEvent`: shared stream for adapter, scan, connection, value-change,
  request, and platform-specific update events

## Platform Coverage

Implemented backend coverage:

| Area | iOS | macOS | Android | Windows | Linux |
| --- | --- | --- | --- | --- | --- |
| Central scan/connect/discover/read/write/notify | yes | yes | yes | yes | yes |
| Peripheral GATT server and advertising | yes | yes | yes | yes | yes |
| Runtime BLE permission request flow | system-managed | system-managed | yes | not required | not required |

Platform notes:

- `readRssi()` is currently implemented on iOS, macOS, and Android.
- `requestMtu()`, `requestConnectionPriority()`, and `setPreferredPhy()` are
  Android-only and return `unsupported` on Apple platforms.
- Desktop permission helpers currently return `notRequired`.
- Linux currently broadcasts server notifications to current BlueZ subscribers
  instead of targeting a specific peer device.
- Implemented support does not automatically mean validated-on-hardware
  support. Track release-facing validation in
  [doc/validated_support_matrix.md](doc/validated_support_matrix.md).

## Android Permissions

- The plugin manifest declares BLE scan, connect, and advertise permissions.
- `ble.permissions.check({...})` reports whether runtime BLE permissions are
  already available.
- `ble.permissions.request({...})` triggers the Android permission prompt when
  an activity is attached.
- `ble.permissions.shouldShowRequestRationale({...})` reports whether Android
  recommends showing an in-app rationale before re-requesting a denied
  permission.
- `ble.permissions.openAppSettings()` and
  `ble.permissions.openBluetoothSettings()` provide recovery shortcuts when the
  app needs to guide the user back into system settings.
- Android 12+ still requires runtime approval for `BLUETOOTH_SCAN`,
  `BLUETOOTH_CONNECT`, and `BLUETOOTH_ADVERTISE`.
- Android 11 and below still requires location permission before scanning.
- Android scan failures are also surfaced through the shared event stream as
  `OmniBleScanErrorEvent`.

On non-Android targets, the permission API returns `notRequired` for the
requested BLE permissions, `shouldShowRequestRationale()` returns `false`, and
the settings helpers return `false`.

## Validation Workflow

Use the example app as the operator console and verify these scenarios on real
hardware:

| Host | Peer | Scenarios |
| --- | --- | --- |
| Android phone | Linux or Windows peripheral | scan, connect, discover, read/write, notify, advertise visibility |
| iPhone or iPad | Linux or Windows peripheral | scan, connect, discover, read/write, notify |
| macOS | Android or Linux peripheral | scan, connect, discover, read/write, notify |
| Windows PC | Android or Apple peripheral | scan, connect, discover, read/write, notify |
| Linux PC | Android or Apple peripheral | scan, connect, discover, read/write, notify |

Pass criteria:

- `startScan()` finds at least one known target and reports stable RSSI or
  advertisement metadata.
- `connect()` emits `connecting` then `connected`, and `disconnect()` tears
  down cleanly.
- `discoverServices()` returns a non-empty service tree for a known GATT
  device.
- Characteristic and descriptor reads and writes succeed on a known test
  service.
- `setNotification()` causes at least one `characteristicValueChanged` event on
  the subscribed characteristic.
- Peripheral scenarios verify advertising start and stop, request events,
  responses, and server-side notifications on every backend.

Operator workflow:

1. Follow [doc/device_lab_runbook.md](doc/device_lab_runbook.md) to execute
   the scenario.
2. Copy the session report from the example app on both hosts after the run.
3. Update [doc/validated_support_matrix.md](doc/validated_support_matrix.md)
   with the outcome and date.

## Development

Run the standard verification set:

```bash
flutter analyze
flutter test
cd example && flutter test
cd example && flutter test integration_test -d macos -r compact
cd example && flutter build apk --debug
cd example && flutter build macos
dart pub publish --dry-run
```

For release-specific steps, see
[doc/release_checklist.md](doc/release_checklist.md).

## License

This project is licensed under the MIT license. See [LICENSE](LICENSE).
