class DeviceLabSessionEntry {
  const DeviceLabSessionEntry({
    required this.timestamp,
    required this.category,
    required this.message,
  });

  final DateTime timestamp;
  final String category;
  final String message;

  String toDisplayLine() {
    return '${timestamp.toIso8601String()} [$category] $message';
  }
}

class DeviceLabSessionSnapshot {
  const DeviceLabSessionSnapshot({
    required this.platform,
    required this.platformVersion,
    required this.adapterState,
    required this.availableFeatures,
    required this.permissionStatuses,
    required this.isScanning,
    required this.peripheralPublished,
    required this.peripheralAdvertising,
    required this.scanSummaries,
    required this.lastScanError,
    required this.entries,
  });

  final String platform;
  final String platformVersion;
  final String adapterState;
  final List<String> availableFeatures;
  final List<String> permissionStatuses;
  final bool isScanning;
  final bool peripheralPublished;
  final bool peripheralAdvertising;
  final List<String> scanSummaries;
  final String? lastScanError;
  final List<DeviceLabSessionEntry> entries;
}

String buildDeviceLabSessionReport(DeviceLabSessionSnapshot snapshot) {
  final buffer = StringBuffer()
    ..writeln('# omni_ble device-lab session')
    ..writeln()
    ..writeln('platform: ${snapshot.platform} ${snapshot.platformVersion}')
    ..writeln('adapterState: ${snapshot.adapterState}')
    ..writeln(
      'availableFeatures: '
      '${snapshot.availableFeatures.isEmpty ? 'none' : snapshot.availableFeatures.join(', ')}',
    )
    ..writeln(
      'permissionStatuses: '
      '${snapshot.permissionStatuses.isEmpty ? 'none' : snapshot.permissionStatuses.join(', ')}',
    )
    ..writeln('isScanning: ${snapshot.isScanning}')
    ..writeln('peripheralPublished: ${snapshot.peripheralPublished}')
    ..writeln('peripheralAdvertising: ${snapshot.peripheralAdvertising}')
    ..writeln(
      'lastScanError: ${snapshot.lastScanError == null || snapshot.lastScanError!.isEmpty ? 'none' : snapshot.lastScanError}',
    )
    ..writeln()
    ..writeln('scanSummaries:');

  if (snapshot.scanSummaries.isEmpty) {
    buffer.writeln('- none');
  } else {
    for (final summary in snapshot.scanSummaries) {
      buffer.writeln('- $summary');
    }
  }

  buffer
    ..writeln()
    ..writeln('sessionEntries:');

  if (snapshot.entries.isEmpty) {
    buffer.writeln('- none');
  } else {
    for (final entry in snapshot.entries) {
      buffer.writeln('- ${entry.toDisplayLine()}');
    }
  }

  return buffer.toString().trimRight();
}
