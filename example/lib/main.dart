import 'dart:async';

import 'package:flutter/material.dart';
import 'package:omni_ble/omni_ble.dart';

void main() {
  runApp(const OmniBleExampleApp());
}

class OmniBleExampleApp extends StatelessWidget {
  const OmniBleExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(home: OmniBleHomePage());
  }
}

class OmniBleHomePage extends StatefulWidget {
  const OmniBleHomePage({super.key});

  @override
  State<OmniBleHomePage> createState() => _OmniBleHomePageState();
}

class _OmniBleHomePageState extends State<OmniBleHomePage> {
  static const _runtimePermissions = {
    OmniBlePermission.scan,
    OmniBlePermission.connect,
    OmniBlePermission.advertise,
  };

  final OmniBle _ble = const OmniBle();
  late Future<OmniBleCapabilities> _capabilitiesFuture;
  StreamSubscription<OmniBleEvent>? _eventsSubscription;
  OmniBleAdapterState _adapterState = OmniBleAdapterState.unknown;
  OmniBlePermissionStatus _permissionStatus = const OmniBlePermissionStatus();
  Map<OmniBlePermission, bool> _permissionRationales = const {};
  Map<String, int> _connectedRssi = const {};
  Map<String, String> _serviceSummaries = const {};
  bool _isScanning = false;
  String? _lastScanError;
  String? _centralActionDeviceId;
  List<OmniBleScanResult> _scanResults = const [];

  @override
  void initState() {
    super.initState();
    _capabilitiesFuture = _ble.getCapabilities();
    _eventsSubscription = _ble.events.listen(_onEvent);
    unawaited(_refreshPermissionStatus());
    unawaited(_refreshPermissionRationales());
  }

  @override
  void dispose() {
    _eventsSubscription?.cancel();
    super.dispose();
  }

  void _onEvent(OmniBleEvent event) {
    if (!mounted) {
      return;
    }

    String? errorMessage;
    setState(() {
      if (event is OmniBleAdapterStateChanged) {
        _adapterState = event.state;
        return;
      }

      if (event is OmniBleScanResultEvent) {
        final existingIndex = _scanResults.indexWhere(
          (result) => result.deviceId == event.result.deviceId,
        );
        final nextResults = [..._scanResults];
        if (existingIndex == -1) {
          nextResults.add(event.result);
        } else {
          nextResults[existingIndex] = event.result;
        }
        nextResults.sort((left, right) => right.rssi.compareTo(left.rssi));
        _scanResults = nextResults;
        return;
      }

      if (event is OmniBleScanErrorEvent) {
        final codeLabel = event.code?.toString() ?? 'unknown';
        _isScanning = false;
        _lastScanError =
            event.message ?? 'Bluetooth scanning failed with code $codeLabel.';
        errorMessage = _lastScanError;
      }
    });

    if (errorMessage != null) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(errorMessage!)));
    }
  }

  Future<void> _refreshPermissionStatus() async {
    try {
      final permissionStatus = await _ble.permissions.check(
        _runtimePermissions,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _permissionStatus = permissionStatus;
      });
    } on OmniBleException {
      if (!mounted) {
        return;
      }
      setState(() {
        _permissionStatus = const OmniBlePermissionStatus();
      });
    }
  }

  Future<void> _refreshPermissionRationales() async {
    try {
      final rationale = await _ble.permissions.shouldShowRequestRationale(
        _runtimePermissions,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _permissionRationales = rationale;
      });
    } on OmniBleException {
      if (!mounted) {
        return;
      }
      setState(() {
        _permissionRationales = const {};
      });
    }
  }

  Future<void> _requestPermissionStatus() async {
    try {
      final permissionStatus = await _ble.permissions.request(
        _runtimePermissions,
      );
      if (!mounted) {
        return;
      }
      setState(() {
        _permissionStatus = permissionStatus;
      });
      await _refreshPermissionRationales();
    } on OmniBleException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('${error.code}: ${error.message}')),
      );
    }
  }

  Future<bool> _ensureAndroidPermission(OmniBlePermission permission) async {
    final permissionStatus = await _ble.permissions.request({permission});
    if (!mounted) {
      return false;
    }
    setState(() {
      _permissionStatus = OmniBlePermissionStatus(
        permissions: {
          ..._permissionStatus.permissions,
          ...permissionStatus.permissions,
        },
      );
    });
    await _refreshPermissionRationales();
    return permissionStatus.isGranted(permission);
  }

  Future<void> _openAppSettings() async {
    try {
      final opened = await _ble.permissions.openAppSettings();
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            opened
                ? 'Opened app settings.'
                : 'App settings are not available on this platform.',
          ),
        ),
      );
    } on OmniBleException catch (error) {
      _showError(error);
    }
  }

  Future<void> _openBluetoothSettings() async {
    try {
      final opened = await _ble.permissions.openBluetoothSettings();
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            opened
                ? 'Opened Bluetooth settings.'
                : 'Bluetooth settings are not available on this platform.',
          ),
        ),
      );
    } on OmniBleException catch (error) {
      _showError(error);
    }
  }

  void _showError(OmniBleException error) {
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text('${error.code}: ${error.message}')));
  }

  bool _supportsConnectedRssi(OmniBleCapabilities capabilities) {
    return switch (capabilities.platform) {
      'android' || 'ios' || 'macos' => true,
      _ => false,
    };
  }

  String _centralActionLabel(OmniBleCapabilities capabilities) {
    return _supportsConnectedRssi(capabilities)
        ? 'Connect, inspect, and read RSSI'
        : 'Connect and inspect GATT';
  }

  Future<void> _toggleScan(OmniBleCapabilities capabilities) async {
    if (!capabilities.supports(OmniBleFeature.scanning)) {
      return;
    }

    try {
      if (_isScanning) {
        await _ble.central.stopScan();
        if (!mounted) {
          return;
        }
        setState(() {
          _isScanning = false;
        });
        return;
      }

      if (capabilities.platform == 'android') {
        final isGranted = await _ensureAndroidPermission(
          OmniBlePermission.scan,
        );
        if (!isGranted) {
          if (!mounted) {
            return;
          }
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Bluetooth scan permission was denied.'),
            ),
          );
          return;
        }
      }

      await _ble.central.startScan();
      if (!mounted) {
        return;
      }
      setState(() {
        _isScanning = true;
        _lastScanError = null;
        _scanResults = const [];
      });
    } on OmniBleException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('${error.code}: ${error.message}')),
      );
    }
  }

  Future<void> _runCentralSmoke(
    OmniBleCapabilities capabilities,
    OmniBleScanResult result,
  ) async {
    if (!capabilities.supports(OmniBleFeature.gattClient)) {
      return;
    }

    if (capabilities.platform == 'android') {
      final isGranted = await _ensureAndroidPermission(
        OmniBlePermission.connect,
      );
      if (!isGranted) {
        if (!mounted) {
          return;
        }
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Bluetooth connect permission was denied.'),
          ),
        );
        return;
      }
    }

    try {
      setState(() {
        _centralActionDeviceId = result.deviceId;
      });

      int? negotiatedMtu;
      int? rssi;
      await _ble.central.connect(
        result.deviceId,
        config: const OmniBleConnectionConfig(timeout: Duration(seconds: 10)),
      );
      if (capabilities.platform == 'android') {
        await _ble.central.requestConnectionPriority(
          result.deviceId,
          OmniBleConnectionPriority.high,
        );
        negotiatedMtu = await _ble.central.requestMtu(
          result.deviceId,
          mtu: 247,
        );
        try {
          await _ble.central.setPreferredPhy(
            result.deviceId,
            txPhy: OmniBlePhy.le2m,
            rxPhy: OmniBlePhy.le2m,
          );
        } on OmniBleException {
          // PHY tuning is best-effort for the demo flow.
        }
      }
      final services = await _ble.central.discoverServices(result.deviceId);
      final characteristicCount = services.fold<int>(
        0,
        (sum, service) => sum + service.characteristics.length,
      );
      final descriptorCount = services.fold<int>(
        0,
        (sum, service) =>
            sum +
            service.characteristics.fold<int>(
              0,
              (value, characteristic) =>
                  value + characteristic.descriptors.length,
            ),
      );
      if (_supportsConnectedRssi(capabilities)) {
        rssi = await _ble.central.readRssi(result.deviceId);
      }

      if (!mounted) {
        return;
      }

      setState(() {
        if (rssi != null) {
          _connectedRssi = {..._connectedRssi, result.deviceId: rssi};
        }
        _serviceSummaries = {
          ..._serviceSummaries,
          result.deviceId:
              '${services.length} services, $characteristicCount characteristics, $descriptorCount descriptors',
        };
      });
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            '${result.name ?? result.deviceId}: '
            '${services.length} services, $characteristicCount characteristics'
            '${descriptorCount == 0 ? '' : ', $descriptorCount descriptors'}'
            '${rssi == null ? '' : ' • RSSI $rssi dBm'}'
            '${negotiatedMtu == null ? '' : ' • MTU $negotiatedMtu'}',
          ),
        ),
      );
    } on OmniBleException catch (error) {
      _showError(error);
    } finally {
      try {
        await _ble.central.disconnect(result.deviceId);
      } on OmniBleException {
        // Best-effort cleanup for the demo flow.
      }
      if (mounted) {
        setState(() {
          _centralActionDeviceId = null;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('omni_ble scaffold')),
      body: FutureBuilder<OmniBleCapabilities>(
        future: _capabilitiesFuture,
        builder: (context, snapshot) {
          if (!snapshot.hasData) {
            return const Center(child: CircularProgressIndicator());
          }

          final capabilities = snapshot.data!;
          final features = capabilities.availableFeatures.toList(
            growable: false,
          );
          final deniedPermissions = _permissionStatus.permissions.entries
              .where((entry) => entry.value == OmniBlePermissionState.denied)
              .toList(growable: false);
          final shouldExplainPermission = deniedPermissions.any(
            (entry) => _permissionRationales[entry.key] == true,
          );

          return ListView(
            padding: const EdgeInsets.all(24),
            children: [
              Text(
                '${capabilities.platform} ${capabilities.platformVersion}',
                style: Theme.of(context).textTheme.headlineSmall,
              ),
              const SizedBox(height: 12),
              Text('Adapter state: ${_adapterState.value}'),
              const SizedBox(height: 12),
              Text(
                capabilities.supports(OmniBleFeature.gattClient)
                    ? capabilities.platform == 'android'
                          ? 'Central and peripheral BLE operations are implemented on Android too. Runtime Bluetooth permission still has to be granted, and this example can request it through the plugin API.'
                          : capabilities.platform == 'windows' ||
                                capabilities.platform == 'linux'
                          ? 'Desktop central GATT client support is now available for scan, connect, discovery, read/write, and notifications. Peripheral/server work is still more limited on desktop, and connected RSSI is not exposed there yet.'
                          : 'Central and peripheral BLE operations, including connected RSSI reads, are currently implemented on Apple platforms and Android.'
                    : capabilities.supports(OmniBleFeature.scanning)
                    ? 'Central scanning is implemented, but the rest of the GATT client surface is still being filled in for this target.'
                    : 'This target currently exposes the scaffold only. Native BLE backends still need to be implemented.',
              ),
              const SizedBox(height: 24),
              Text(
                'Available features',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const SizedBox(height: 8),
              if (features.isEmpty)
                const Card(
                  child: Padding(
                    padding: EdgeInsets.all(16),
                    child: Text(
                      'No platform features are marked as implemented yet.',
                    ),
                  ),
                )
              else
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (final feature in features)
                      Chip(label: Text(feature.value)),
                  ],
                ),
              const SizedBox(height: 24),
              Text(
                'Runtime permissions',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const SizedBox(height: 8),
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: _permissionStatus.permissions.isEmpty
                      ? const Text(
                          'No runtime permission status is available yet.',
                        )
                      : Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            for (final entry
                                in _permissionStatus.permissions.entries)
                              Padding(
                                padding: const EdgeInsets.only(bottom: 8),
                                child: Text(
                                  '${entry.key.value}: ${entry.value.value}',
                                ),
                              ),
                          ],
                        ),
                ),
              ),
              if (capabilities.platform == 'android') ...[
                const SizedBox(height: 12),
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: _permissionRationales.isEmpty
                        ? const Text(
                            'Android permission rationale guidance is not available yet.',
                          )
                        : Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              for (final entry in _permissionRationales.entries)
                                Padding(
                                  padding: const EdgeInsets.only(bottom: 8),
                                  child: Text(
                                    '${entry.key.value}: '
                                    '${entry.value ? 'show rationale before requesting again' : 'no rationale needed right now'}',
                                  ),
                                ),
                              if (deniedPermissions.isNotEmpty)
                                Text(
                                  shouldExplainPermission
                                      ? 'Android recommends showing an in-app rationale before re-requesting at least one denied permission.'
                                      : 'If the system dialog no longer appears, use app settings to recover denied permissions.',
                                ),
                            ],
                          ),
                  ),
                ),
              ],
              const SizedBox(height: 12),
              Wrap(
                spacing: 12,
                runSpacing: 12,
                children: [
                  FilledButton.tonal(
                    onPressed: capabilities.platform == 'android'
                        ? _requestPermissionStatus
                        : null,
                    child: const Text('Request BLE permissions'),
                  ),
                  OutlinedButton(
                    onPressed: capabilities.platform == 'android'
                        ? _openAppSettings
                        : null,
                    child: const Text('Open app settings'),
                  ),
                  OutlinedButton(
                    onPressed: capabilities.platform == 'android'
                        ? _openBluetoothSettings
                        : null,
                    child: const Text('Bluetooth settings'),
                  ),
                ],
              ),
              const SizedBox(height: 24),
              FilledButton(
                onPressed: capabilities.supports(OmniBleFeature.scanning)
                    ? () => _toggleScan(capabilities)
                    : null,
                child: Text(_isScanning ? 'Stop scan' : 'Start scan'),
              ),
              if (_lastScanError != null) ...[
                const SizedBox(height: 12),
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Text('Last scan error: $_lastScanError'),
                  ),
                ),
              ],
              const SizedBox(height: 12),
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: _scanResults.isEmpty
                      ? const Text('No scan results yet.')
                      : Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            for (final result in _scanResults.take(8))
                              Padding(
                                padding: const EdgeInsets.only(bottom: 12),
                                child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: [
                                    Text(
                                      '${result.name ?? 'Unnamed'}\n'
                                      '${result.deviceId}\n'
                                      'RSSI ${result.rssi}',
                                    ),
                                    const SizedBox(height: 8),
                                    Text(
                                      _serviceSummaries[result.deviceId] ??
                                          'Services not inspected yet.',
                                    ),
                                    const SizedBox(height: 8),
                                    Text(
                                      _supportsConnectedRssi(capabilities)
                                          ? _connectedRssi.containsKey(
                                                result.deviceId,
                                              )
                                          ? 'Connected RSSI ${_connectedRssi[result.deviceId]} dBm'
                                          : 'Connected RSSI not read yet.'
                                          : 'Connected RSSI is not exposed on this platform.',
                                    ),
                                    const SizedBox(height: 8),
                                    FilledButton.tonal(
                                      onPressed:
                                          capabilities.supports(
                                                OmniBleFeature.gattClient,
                                              ) &&
                                              _centralActionDeviceId == null
                                          ? () => _runCentralSmoke(
                                              capabilities,
                                              result,
                                            )
                                          : null,
                                      child: Text(
                                        _centralActionDeviceId == result.deviceId
                                            ? 'Running central smoke...'
                                            : _centralActionLabel(capabilities),
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                          ],
                        ),
                ),
              ),
              const SizedBox(height: 24),
              Text(
                'Device-lab checklist',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const SizedBox(height: 8),
              const Card(
                child: Padding(
                  padding: EdgeInsets.all(16),
                  child: Text(
                    'Central smoke: startScan -> connect -> discoverServices\n'
                    'Mobile smoke: readRssi after connect, then disconnect\n'
                    'GATT smoke: read/write known characteristics and descriptors, then enable notifications\n'
                    'Peripheral smoke: publishGattDatabase -> startAdvertising -> handle read/write requests -> notifyCharacteristicValue',
                  ),
                ),
              ),
              const SizedBox(height: 24),
              FilledButton(
                onPressed: () {
                  setState(() {
                    _capabilitiesFuture = _ble.getCapabilities();
                  });
                  unawaited(_refreshPermissionStatus());
                },
                child: const Text('Refresh capabilities'),
              ),
            ],
          );
        },
      ),
    );
  }
}
