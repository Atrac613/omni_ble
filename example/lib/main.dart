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
  bool _isScanning = false;
  List<OmniBleScanResult> _scanResults = const [];

  @override
  void initState() {
    super.initState();
    _capabilitiesFuture = _ble.getCapabilities();
    _eventsSubscription = _ble.events.listen(_onEvent);
    unawaited(_refreshPermissionStatus());
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
      }
    });
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
    } on OmniBleException catch (error) {
      if (!mounted) {
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('${error.code}: ${error.message}')),
      );
    }
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
        final permissionStatus = await _ble.permissions.request({
          OmniBlePermission.scan,
        });
        if (!mounted) {
          return;
        }
        setState(() {
          _permissionStatus = OmniBlePermissionStatus(
            permissions: {
              ..._permissionStatus.permissions,
              ...permissionStatus.permissions,
            },
          );
        });
        if (!permissionStatus.isGranted(OmniBlePermission.scan)) {
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
                          : 'Central and peripheral BLE operations are currently implemented on Apple platforms and Android. Other targets still expose the scaffold only.'
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
              const SizedBox(height: 12),
              FilledButton.tonal(
                onPressed: capabilities.platform == 'android'
                    ? _requestPermissionStatus
                    : null,
                child: const Text('Request BLE permissions'),
              ),
              const SizedBox(height: 24),
              FilledButton(
                onPressed: capabilities.supports(OmniBleFeature.scanning)
                    ? () => _toggleScan(capabilities)
                    : null,
                child: Text(_isScanning ? 'Stop scan' : 'Start scan'),
              ),
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
                                child: Text(
                                  '${result.name ?? 'Unnamed'}\n'
                                  '${result.deviceId}\n'
                                  'RSSI ${result.rssi}',
                                ),
                              ),
                          ],
                        ),
                ),
              ),
              const SizedBox(height: 24),
              Text(
                'Planned surface area',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const SizedBox(height: 8),
              const Card(
                child: Padding(
                  padding: EdgeInsets.all(16),
                  child: Text(
                    'Permissions: check/request BLE runtime permissions\n'
                    'Central: startScan, connect, discoverServices, read/write, setNotification, characteristic events\n'
                    'Peripheral: publishGattDatabase, startAdvertising, notifyCharacteristicValue, request events, respondToReadRequest/respondToWriteRequest',
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
