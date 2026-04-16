# Device-Lab Runbook

## Purpose

This runbook describes how to execute the real-hardware BLE interoperability
matrix for `omni_ble` by using the example app as the operator console.

Use this document when validating a new platform backend, confirming a release
candidate, or reproducing a field interoperability issue.

## Required Hardware

- 1 Android phone with BLE support
- 1 iPhone or iPad with BLE support
- 1 macOS host with BLE support
- 1 Windows host with BLE support
- 1 Linux host running BlueZ with BLE support

## Required Software

- The current branch checked out on every host
- Flutter SDK installed on every host
- A successful local build of the example app on every host before testing

Recommended verification before starting the matrix:

```bash
flutter analyze
flutter test
cd example && flutter test
cd example && flutter test integration_test
cd example && flutter build apk --debug
```

## Example App Roles

The example app now supports both central and peripheral smoke flows.

- Central smoke:
  `startScan -> connect -> discoverServices -> optional connected RSSI`
- Desktop peripheral smoke:
  `publishGattDatabase -> startAdvertising -> auto-ack read/write -> notifyCharacteristicValue`

Demo GATT values used by the peripheral smoke flow:

- Service UUID: `12345678-1234-5678-1234-56789abcdef0`
- Characteristic UUID: `12345678-1234-5678-1234-56789abcdef1`
- Descriptor UUID: `00002901-0000-1000-8000-00805f9b34fb`

## Session Setup

Do this on every host before running a scenario:

1. Launch the example app from the current branch.
2. Tap `Refresh capabilities`.
3. Confirm the platform label and available features look correct.
4. On Android, request runtime BLE permissions before scanning or advertising.
5. Leave the example app open for the full scenario.

## Scenario Matrix

### Priority 1

1. `A1`: Android central -> Windows peripheral
2. `A2`: Android central -> Linux peripheral
3. `I1`: iPhone or iPad central -> Windows peripheral
4. `I2`: iPhone or iPad central -> Linux peripheral

### Priority 2

1. `M1`: macOS central -> Android peripheral
2. `M2`: macOS central -> Linux peripheral
3. `W1`: Windows central -> Android peripheral
4. `W2`: Windows central -> Apple peripheral
5. `L1`: Linux central -> Android peripheral
6. `L2`: Linux central -> Apple peripheral

## Shared Peripheral Procedure

Run these steps on the host acting as the peripheral:

1. Open the `Desktop Peripheral Smoke` section when available.
2. Tap `Publish demo GATT`.
3. Tap `Start demo advertising`.
4. Leave the app on-screen until the central-side checks finish.
5. When prompted, tap `Send demo notification` once after the central has subscribed.
6. Record any read/write/subscription events shown in the app.
7. After the scenario finishes, tap `Stop demo advertising`.
8. Tap `Clear demo GATT`.

For Android or Apple peripheral scenarios that do not use the desktop demo GATT,
use the platform-specific app or fixture that exposes a known readable,
writable, and notifiable characteristic.

## Shared Central Procedure

Run these steps on the host acting as the central:

1. Tap `Start scan`.
2. Verify the target peripheral appears in scan results.
3. Tap the central smoke button for the target row.
4. Confirm the app reports service and characteristic counts.
5. If the host supports connected RSSI, record the value shown after connect.
6. If manual GATT fixtures are available, additionally validate:
   - `readCharacteristic()`
   - `writeCharacteristic()`
   - `readDescriptor()`
   - `writeDescriptor()`
   - `setNotification()`
7. Confirm that at least one notification is received after the peripheral sends
   the demo value.
8. Stop the scan after the scenario is complete.

## Pass Criteria

A scenario passes only when all of the following are true:

1. The central finds the target peripheral while scanning.
2. The connection transitions through `connecting` to `connected`.
3. `discoverServices()` returns a non-empty service tree.
4. Known characteristic reads and writes succeed.
5. Known descriptor reads and writes succeed when supported by the fixture.
6. Notification subscription succeeds and at least one value update is observed.
7. Peripheral advertising can be stopped and restarted cleanly afterward.

## Failure Capture

When a scenario fails, capture all of the following before retrying:

1. Host and peer platform names
2. Scenario id such as `A1` or `W2`
3. Commit SHA on both hosts
4. Example app session log from both sides
5. The exact UI step where the failure happened
6. Any native error text surfaced in the app
7. Whether retrying without restarting Bluetooth changes the outcome

## Operator Notes Template

Copy this into the issue, PR, or release notes after each run:

```text
Scenario:
Host build:
Peer build:
Result:
Observed services:
Observed notifications:
Observed caveats:
Next action:
```
