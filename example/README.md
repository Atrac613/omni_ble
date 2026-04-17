# omni_ble_example

Demonstrates how to use the `omni_ble` plugin as both a feature smoke app and
the operator console for device-lab runs.

## What The Example Covers

- Capability inspection
- Runtime BLE permission checks and requests
- Central scan and connect smoke flows
- Desktop peripheral smoke flows with a demo GATT database
- Device-lab session logging with copyable reports

## Demo GATT Values

- Service UUID: `12345678-1234-5678-1234-56789abcdef0`
- Characteristic UUID: `12345678-1234-5678-1234-56789abcdef1`
- Descriptor UUID: `00002901-0000-1000-8000-00805f9b34fb`

## Running The Example

```bash
flutter run
```

Use `Refresh capabilities` after launch so the app reflects the current host
platform and BLE feature set.

## Device-Lab Workflow

1. Use the root-level
   [device-lab runbook](../doc/device_lab_runbook.md) for the scenario order
   and pass criteria.
2. Use `Start scan` and the central smoke button when this host is acting as a
   BLE central.
3. Use `Publish demo GATT` and `Start demo advertising` when this host is
   acting as a desktop peripheral.
4. After the run, use `Copy session report` to capture the local operator log.
5. Update the root-level
   [validated support matrix](../doc/validated_support_matrix.md) with the
   result.
