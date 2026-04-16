import 'package:flutter_test/flutter_test.dart';
import 'package:omni_ble_example/device_lab_session.dart';

void main() {
  test('buildDeviceLabSessionReport renders session details', () {
    final report = buildDeviceLabSessionReport(
      DeviceLabSessionSnapshot(
        platform: 'linux',
        platformVersion: 'Linux 6.0',
        adapterState: 'poweredOn',
        availableFeatures: const ['central', 'peripheral', 'gattServer'],
        permissionStatuses: const ['scan=notRequired', 'connect=notRequired'],
        isScanning: true,
        peripheralPublished: true,
        peripheralAdvertising: false,
        scanSummaries: const ['demo-device (RSSI -48) - 1 services'],
        lastScanError: null,
        entries: [
          DeviceLabSessionEntry(
            timestamp: DateTime.parse('2026-04-17T10:15:30.000Z'),
            category: 'central',
            message: 'Central smoke passed.',
          ),
        ],
      ),
    );

    expect(report, contains('# omni_ble device-lab session'));
    expect(report, contains('platform: linux Linux 6.0'));
    expect(report, contains('availableFeatures: central, peripheral, gattServer'));
    expect(report, contains('scanSummaries:'));
    expect(report, contains('demo-device (RSSI -48) - 1 services'));
    expect(
      report,
      contains(
        '2026-04-17T10:15:30.000Z [central] Central smoke passed.',
      ),
    );
  });
}
