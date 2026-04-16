# Validated Support Matrix

## Purpose

This document separates `implemented` support from `validated on hardware`
support.

Use it to decide what can be advertised in release notes, what still needs
device-lab confirmation, and which scenarios block a release.

## Status Labels

- `implemented`: code exists in the repository
- `ready-for-lab`: the example app and docs are ready for real-device testing
- `validated`: executed on real hardware and passed
- `blocked`: executed on real hardware and failed
- `not-planned`: intentionally outside the supported release surface

## Platform Role Summary

| Platform | Central client | Peripheral server | Validation status | Notes |
| --- | --- | --- | --- | --- |
| Android | implemented | implemented | ready-for-lab | Runtime BLE permissions required |
| iOS | implemented | implemented | ready-for-lab | CoreBluetooth-backed |
| macOS | implemented | implemented | ready-for-lab | CoreBluetooth-backed |
| Windows | implemented | implemented | ready-for-lab | `readRssi()` still unavailable |
| Linux | implemented | implemented | ready-for-lab | BlueZ notifications are broadcast, not targeted |

## Scenario Matrix

| Scenario | Central host | Peripheral host | Validation status | Last run | Notes |
| --- | --- | --- | --- | --- | --- |
| `A1` | Android | Windows | ready-for-lab | pending | Use desktop demo GATT on Windows |
| `A2` | Android | Linux | ready-for-lab | pending | Watch BlueZ advertising and notify behavior |
| `I1` | iPhone or iPad | Windows | ready-for-lab | pending | Confirm local-name visibility and service discovery |
| `I2` | iPhone or iPad | Linux | ready-for-lab | pending | Confirm BlueZ advertisement compatibility |
| `M1` | macOS | Android | ready-for-lab | pending | Validate descriptor read/write and notification delivery |
| `M2` | macOS | Linux | ready-for-lab | pending | Validate BlueZ characteristic and descriptor paths |
| `W1` | Windows | Android | ready-for-lab | pending | Confirm central notification subscription behavior |
| `W2` | Windows | Apple | ready-for-lab | pending | Confirm interoperability with CoreBluetooth peripheral role |
| `L1` | Linux | Android | ready-for-lab | pending | Confirm BlueZ central stability during notify traffic |
| `L2` | Linux | Apple | ready-for-lab | pending | Confirm Apple peripheral compatibility with BlueZ central |

## Release Gate

Do not mark the desktop BLE surface as `validated` in release notes until:

1. All Priority 1 scenarios have passed on real hardware.
2. At least one Apple peripheral scenario and one Android peripheral scenario
   have passed against both Windows and Linux centrals.
3. Known caveats are written down in `README.md`.

## Updating This Document

After each lab run:

1. Change the scenario status from `ready-for-lab` to `validated` or `blocked`.
2. Fill in the `Last run` column with the execution date.
3. Summarize any caveat or failure pattern in the `Notes` column.
4. If a scenario remains blocked, link the follow-up issue or PR in the notes.
