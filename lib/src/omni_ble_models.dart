import 'dart:typed_data';

enum OmniBleFeature {
  central,
  scanning,
  gattClient,
  peripheral,
  advertising,
  gattServer,
  notifications,
}

extension OmniBleFeatureValue on OmniBleFeature {
  String get value => switch (this) {
    OmniBleFeature.central => 'central',
    OmniBleFeature.scanning => 'scanning',
    OmniBleFeature.gattClient => 'gattClient',
    OmniBleFeature.peripheral => 'peripheral',
    OmniBleFeature.advertising => 'advertising',
    OmniBleFeature.gattServer => 'gattServer',
    OmniBleFeature.notifications => 'notifications',
  };
}

enum OmniBlePermission { scan, connect, advertise }

extension OmniBlePermissionValue on OmniBlePermission {
  String get value => switch (this) {
    OmniBlePermission.scan => 'scan',
    OmniBlePermission.connect => 'connect',
    OmniBlePermission.advertise => 'advertise',
  };
}

enum OmniBlePermissionState { granted, denied, notRequired }

extension OmniBlePermissionStateValue on OmniBlePermissionState {
  String get value => switch (this) {
    OmniBlePermissionState.granted => 'granted',
    OmniBlePermissionState.denied => 'denied',
    OmniBlePermissionState.notRequired => 'notRequired',
  };
}

enum OmniBleWriteType { withResponse, withoutResponse }

extension OmniBleWriteTypeValue on OmniBleWriteType {
  String get value => switch (this) {
    OmniBleWriteType.withResponse => 'withResponse',
    OmniBleWriteType.withoutResponse => 'withoutResponse',
  };
}

enum OmniBleAdvertisingMode { balanced, lowLatency, lowPower }

extension OmniBleAdvertisingModeValue on OmniBleAdvertisingMode {
  String get value => switch (this) {
    OmniBleAdvertisingMode.balanced => 'balanced',
    OmniBleAdvertisingMode.lowLatency => 'lowLatency',
    OmniBleAdvertisingMode.lowPower => 'lowPower',
  };
}

enum OmniBleAdvertisingTxPower { ultraLow, low, medium, high }

extension OmniBleAdvertisingTxPowerValue on OmniBleAdvertisingTxPower {
  String get value => switch (this) {
    OmniBleAdvertisingTxPower.ultraLow => 'ultraLow',
    OmniBleAdvertisingTxPower.low => 'low',
    OmniBleAdvertisingTxPower.medium => 'medium',
    OmniBleAdvertisingTxPower.high => 'high',
  };
}

enum OmniBleGattProperty { read, write, writeWithoutResponse, notify, indicate }

extension OmniBleGattPropertyValue on OmniBleGattProperty {
  String get value => switch (this) {
    OmniBleGattProperty.read => 'read',
    OmniBleGattProperty.write => 'write',
    OmniBleGattProperty.writeWithoutResponse => 'writeWithoutResponse',
    OmniBleGattProperty.notify => 'notify',
    OmniBleGattProperty.indicate => 'indicate',
  };
}

enum OmniBleGattPermission { read, write, readEncrypted, writeEncrypted }

extension OmniBleGattPermissionValue on OmniBleGattPermission {
  String get value => switch (this) {
    OmniBleGattPermission.read => 'read',
    OmniBleGattPermission.write => 'write',
    OmniBleGattPermission.readEncrypted => 'readEncrypted',
    OmniBleGattPermission.writeEncrypted => 'writeEncrypted',
  };
}

enum OmniBleAdapterState {
  unknown,
  unavailable,
  unauthorized,
  poweredOff,
  poweredOn,
}

extension OmniBleAdapterStateValue on OmniBleAdapterState {
  String get value => switch (this) {
    OmniBleAdapterState.unknown => 'unknown',
    OmniBleAdapterState.unavailable => 'unavailable',
    OmniBleAdapterState.unauthorized => 'unauthorized',
    OmniBleAdapterState.poweredOff => 'poweredOff',
    OmniBleAdapterState.poweredOn => 'poweredOn',
  };
}

enum OmniBleConnectionState {
  disconnected,
  connecting,
  connected,
  disconnecting,
}

extension OmniBleConnectionStateValue on OmniBleConnectionState {
  String get value => switch (this) {
    OmniBleConnectionState.disconnected => 'disconnected',
    OmniBleConnectionState.connecting => 'connecting',
    OmniBleConnectionState.connected => 'connected',
    OmniBleConnectionState.disconnecting => 'disconnecting',
  };
}

class OmniBleCapabilities {
  const OmniBleCapabilities({
    required this.platform,
    required this.platformVersion,
    this.availableFeatures = const {},
    this.metadata = const {},
  });

  final String platform;
  final String platformVersion;
  final Set<OmniBleFeature> availableFeatures;
  final Map<String, Object?> metadata;

  bool supports(OmniBleFeature feature) => availableFeatures.contains(feature);

  factory OmniBleCapabilities.fromMap(Map<Object?, Object?> map) {
    return OmniBleCapabilities(
      platform: (map['platform'] ?? 'unknown').toString(),
      platformVersion: (map['platformVersion'] ?? 'unknown').toString(),
      availableFeatures: _stringList(
        map['availableFeatures'],
      ).map(_parseFeature).toSet(),
      metadata: _objectMap(map['metadata']),
    );
  }

  Map<String, Object?> toMap() {
    return {
      'platform': platform,
      'platformVersion': platformVersion,
      'availableFeatures': availableFeatures
          .map((feature) => feature.value)
          .toList(growable: false),
      'metadata': metadata,
    };
  }
}

class OmniBlePermissionStatus {
  const OmniBlePermissionStatus({this.permissions = const {}});

  final Map<OmniBlePermission, OmniBlePermissionState> permissions;

  bool get allGranted => permissions.values.every(
    (state) =>
        state == OmniBlePermissionState.granted ||
        state == OmniBlePermissionState.notRequired,
  );

  bool isGranted(OmniBlePermission permission) {
    final state = permissions[permission];
    return state == OmniBlePermissionState.granted ||
        state == OmniBlePermissionState.notRequired;
  }

  factory OmniBlePermissionStatus.fromMap(Map<Object?, Object?> map) {
    final permissionMap = _objectMap(map['permissions']);
    return OmniBlePermissionStatus(
      permissions: permissionMap.map(
        (key, value) => MapEntry(
          _parsePermission(key),
          _parsePermissionState((value ?? 'denied').toString()),
        ),
      ),
    );
  }

  Map<String, Object?> toMap() {
    return {
      'permissions': permissions.map(
        (permission, state) => MapEntry(permission.value, state.value),
      ),
      'allGranted': allGranted,
    };
  }
}

class OmniBleScanConfig {
  const OmniBleScanConfig({
    this.serviceUuids = const [],
    this.allowDuplicates = false,
  });

  final List<String> serviceUuids;
  final bool allowDuplicates;

  Map<String, Object?> toMap() {
    return {'serviceUuids': serviceUuids, 'allowDuplicates': allowDuplicates};
  }
}

class OmniBleScanResult {
  const OmniBleScanResult({
    required this.deviceId,
    required this.rssi,
    this.name,
    this.serviceUuids = const [],
    this.serviceData = const {},
    this.manufacturerData,
    this.connectable = true,
  });

  final String deviceId;
  final String? name;
  final int rssi;
  final List<String> serviceUuids;
  final Map<String, Uint8List> serviceData;
  final Uint8List? manufacturerData;
  final bool connectable;

  factory OmniBleScanResult.fromMap(Map<Object?, Object?> map) {
    return OmniBleScanResult(
      deviceId: (map['deviceId'] ?? '').toString(),
      name: map['name']?.toString(),
      rssi: (map['rssi'] as num?)?.toInt() ?? 0,
      serviceUuids: _stringList(map['serviceUuids']),
      serviceData: _bytesMap(map['serviceData']),
      manufacturerData: _bytesOrNull(map['manufacturerData']),
      connectable: map['connectable'] as bool? ?? true,
    );
  }

  Map<String, Object?> toMap() {
    return {
      'deviceId': deviceId,
      'name': name,
      'rssi': rssi,
      'serviceUuids': serviceUuids,
      'serviceData': serviceData.map(
        (key, value) => MapEntry(key, value.toList(growable: false)),
      ),
      'manufacturerData': manufacturerData?.toList(growable: false),
      'connectable': connectable,
    };
  }
}

class OmniBleCharacteristicAddress {
  const OmniBleCharacteristicAddress({
    required this.serviceUuid,
    required this.characteristicUuid,
    this.deviceId,
  });

  final String? deviceId;
  final String serviceUuid;
  final String characteristicUuid;

  Map<String, Object?> toMap() {
    return {
      if (deviceId != null) 'deviceId': deviceId,
      'serviceUuid': serviceUuid,
      'characteristicUuid': characteristicUuid,
    };
  }
}

class OmniBleDescriptorAddress {
  const OmniBleDescriptorAddress({
    required this.deviceId,
    required this.serviceUuid,
    required this.characteristicUuid,
    required this.descriptorUuid,
  });

  final String deviceId;
  final String serviceUuid;
  final String characteristicUuid;
  final String descriptorUuid;

  Map<String, Object?> toMap() {
    return {
      'deviceId': deviceId,
      'serviceUuid': serviceUuid,
      'characteristicUuid': characteristicUuid,
      'descriptorUuid': descriptorUuid,
    };
  }
}

class OmniBleGattDatabase {
  const OmniBleGattDatabase({required this.services});

  final List<OmniBleGattService> services;

  Map<String, Object?> toMap() {
    return {
      'services': services
          .map((service) => service.toMap())
          .toList(growable: false),
    };
  }
}

class OmniBleGattService {
  const OmniBleGattService({
    required this.uuid,
    this.primary = true,
    this.characteristics = const [],
  });

  final String uuid;
  final bool primary;
  final List<OmniBleGattCharacteristic> characteristics;

  factory OmniBleGattService.fromMap(Map<Object?, Object?> map) {
    return OmniBleGattService(
      uuid: (map['uuid'] ?? '').toString(),
      primary: map['primary'] as bool? ?? true,
      characteristics: _mapList(
        map['characteristics'],
      ).map(OmniBleGattCharacteristic.fromMap).toList(growable: false),
    );
  }

  Map<String, Object?> toMap() {
    return {
      'uuid': uuid,
      'primary': primary,
      'characteristics': characteristics
          .map((characteristic) => characteristic.toMap())
          .toList(growable: false),
    };
  }
}

class OmniBleGattCharacteristic {
  const OmniBleGattCharacteristic({
    required this.uuid,
    this.properties = const {},
    this.permissions = const {},
    this.descriptors = const [],
    this.initialValue,
  });

  final String uuid;
  final Set<OmniBleGattProperty> properties;
  final Set<OmniBleGattPermission> permissions;
  final List<OmniBleGattDescriptor> descriptors;
  final Uint8List? initialValue;

  factory OmniBleGattCharacteristic.fromMap(Map<Object?, Object?> map) {
    return OmniBleGattCharacteristic(
      uuid: (map['uuid'] ?? '').toString(),
      properties: _stringList(
        map['properties'],
      ).map(_parseGattProperty).toSet(),
      permissions: _stringList(
        map['permissions'],
      ).map(_parseGattPermission).toSet(),
      descriptors: _mapList(
        map['descriptors'],
      ).map(OmniBleGattDescriptor.fromMap).toList(growable: false),
      initialValue: _bytesOrNull(map['initialValue']),
    );
  }

  Map<String, Object?> toMap() {
    return {
      'uuid': uuid,
      'properties': properties
          .map((property) => property.value)
          .toList(growable: false),
      'permissions': permissions
          .map((permission) => permission.value)
          .toList(growable: false),
      'descriptors': descriptors
          .map((descriptor) => descriptor.toMap())
          .toList(growable: false),
      'initialValue': initialValue?.toList(growable: false),
    };
  }
}

class OmniBleGattDescriptor {
  const OmniBleGattDescriptor({
    required this.uuid,
    this.permissions = const {},
    this.initialValue,
  });

  final String uuid;
  final Set<OmniBleGattPermission> permissions;
  final Uint8List? initialValue;

  factory OmniBleGattDescriptor.fromMap(Map<Object?, Object?> map) {
    return OmniBleGattDescriptor(
      uuid: (map['uuid'] ?? '').toString(),
      permissions: _stringList(
        map['permissions'],
      ).map(_parseGattPermission).toSet(),
      initialValue: _bytesOrNull(map['initialValue']),
    );
  }

  Map<String, Object?> toMap() {
    return {
      'uuid': uuid,
      'permissions': permissions
          .map((permission) => permission.value)
          .toList(growable: false),
      'initialValue': initialValue?.toList(growable: false),
    };
  }
}

class OmniBleAdvertisement {
  const OmniBleAdvertisement({
    this.localName,
    this.serviceUuids = const [],
    this.serviceData = const {},
    this.manufacturerData,
    this.connectable = true,
    this.includeTxPowerLevel = false,
    this.androidMode = OmniBleAdvertisingMode.lowLatency,
    this.androidTxPowerLevel = OmniBleAdvertisingTxPower.high,
    this.timeout,
  });

  final String? localName;
  final List<String> serviceUuids;
  final Map<String, Uint8List> serviceData;
  final Uint8List? manufacturerData;
  final bool connectable;
  final bool includeTxPowerLevel;
  final OmniBleAdvertisingMode androidMode;
  final OmniBleAdvertisingTxPower androidTxPowerLevel;
  final Duration? timeout;

  Map<String, Object?> toMap() {
    return {
      'localName': localName,
      'serviceUuids': serviceUuids,
      'serviceData': serviceData.map(
        (key, value) => MapEntry(key, value.toList(growable: false)),
      ),
      'manufacturerData': manufacturerData?.toList(growable: false),
      'connectable': connectable,
      'includeTxPowerLevel': includeTxPowerLevel,
      'androidMode': androidMode.value,
      'androidTxPowerLevel': androidTxPowerLevel.value,
      'timeoutMs': timeout?.inMilliseconds,
    };
  }
}

sealed class OmniBleEvent {
  const OmniBleEvent(this.type);

  final String type;

  factory OmniBleEvent.fromMap(Map<Object?, Object?> map) {
    final type = map['type']?.toString() ?? 'unknown';
    switch (type) {
      case OmniBleAdapterStateChanged.typeValue:
        return OmniBleAdapterStateChanged(
          _parseAdapterState((map['state'] ?? 'unknown').toString()),
        );
      case OmniBleScanResultEvent.typeValue:
        return OmniBleScanResultEvent(
          OmniBleScanResult.fromMap(_rawMap(map['result'])),
        );
      case OmniBleScanErrorEvent.typeValue:
        return OmniBleScanErrorEvent(
          code: _intOrNull(map['code']),
          message: map['message']?.toString(),
        );
      case OmniBleConnectionStateChanged.typeValue:
        return OmniBleConnectionStateChanged(
          deviceId: (map['deviceId'] ?? '').toString(),
          state: _parseConnectionState(
            (map['state'] ?? 'disconnected').toString(),
          ),
        );
      case OmniBleCharacteristicValueChanged.typeValue:
        return OmniBleCharacteristicValueChanged(
          address: OmniBleCharacteristicAddress(
            deviceId: map['deviceId']?.toString(),
            serviceUuid: (map['serviceUuid'] ?? '').toString(),
            characteristicUuid: (map['characteristicUuid'] ?? '').toString(),
          ),
          value: _bytesOrNull(map['value']) ?? Uint8List(0),
        );
      case OmniBleNotificationQueueReadyEvent.typeValue:
        return OmniBleNotificationQueueReadyEvent(
          deviceId: map['deviceId']?.toString(),
          status: _intOrNull(map['status']),
        );
      case OmniBleReadRequestEvent.typeValue:
        return OmniBleReadRequestEvent(
          requestId: (map['requestId'] ?? '').toString(),
          deviceId: map['deviceId']?.toString(),
          serviceUuid: (map['serviceUuid'] ?? '').toString(),
          characteristicUuid: (map['characteristicUuid'] ?? '').toString(),
          offset: _intOrNull(map['offset']) ?? 0,
        );
      case OmniBleWriteRequestEvent.typeValue:
        return OmniBleWriteRequestEvent(
          requestId: (map['requestId'] ?? '').toString(),
          deviceId: map['deviceId']?.toString(),
          serviceUuid: (map['serviceUuid'] ?? '').toString(),
          characteristicUuid: (map['characteristicUuid'] ?? '').toString(),
          offset: _intOrNull(map['offset']) ?? 0,
          preparedWrite: _boolOrNull(map['preparedWrite']),
          responseNeeded: _boolOrNull(map['responseNeeded']),
          value: _bytesOrNull(map['value']) ?? Uint8List(0),
        );
      case OmniBleSubscriptionChanged.typeValue:
        return OmniBleSubscriptionChanged(
          deviceId: map['deviceId']?.toString(),
          serviceUuid: (map['serviceUuid'] ?? '').toString(),
          characteristicUuid: (map['characteristicUuid'] ?? '').toString(),
          subscribed: map['subscribed'] as bool? ?? false,
        );
      default:
        return OmniBleUnknownEvent(_objectMap(map));
    }
  }
}

final class OmniBleAdapterStateChanged extends OmniBleEvent {
  const OmniBleAdapterStateChanged(this.state) : super(typeValue);

  static const typeValue = 'adapterStateChanged';

  final OmniBleAdapterState state;
}

final class OmniBleScanResultEvent extends OmniBleEvent {
  const OmniBleScanResultEvent(this.result) : super(typeValue);

  static const typeValue = 'scanResult';

  final OmniBleScanResult result;
}

final class OmniBleScanErrorEvent extends OmniBleEvent {
  const OmniBleScanErrorEvent({this.code, this.message}) : super(typeValue);

  static const typeValue = 'scanError';

  final int? code;
  final String? message;
}

final class OmniBleConnectionStateChanged extends OmniBleEvent {
  const OmniBleConnectionStateChanged({
    required this.deviceId,
    required this.state,
  }) : super(typeValue);

  static const typeValue = 'connectionStateChanged';

  final String deviceId;
  final OmniBleConnectionState state;
}

final class OmniBleCharacteristicValueChanged extends OmniBleEvent {
  const OmniBleCharacteristicValueChanged({
    required this.address,
    required this.value,
  }) : super(typeValue);

  static const typeValue = 'characteristicValueChanged';

  final OmniBleCharacteristicAddress address;
  final Uint8List value;
}

final class OmniBleNotificationQueueReadyEvent extends OmniBleEvent {
  const OmniBleNotificationQueueReadyEvent({this.deviceId, this.status})
    : super(typeValue);

  static const typeValue = 'notificationQueueReady';

  final String? deviceId;
  final int? status;
}

final class OmniBleReadRequestEvent extends OmniBleEvent {
  const OmniBleReadRequestEvent({
    required this.requestId,
    required this.serviceUuid,
    required this.characteristicUuid,
    required this.offset,
    this.deviceId,
  }) : super(typeValue);

  static const typeValue = 'readRequest';

  final String requestId;
  final String? deviceId;
  final String serviceUuid;
  final String characteristicUuid;
  final int offset;
}

final class OmniBleWriteRequestEvent extends OmniBleEvent {
  const OmniBleWriteRequestEvent({
    required this.requestId,
    required this.serviceUuid,
    required this.characteristicUuid,
    required this.offset,
    required this.value,
    this.preparedWrite,
    this.responseNeeded,
    this.deviceId,
  }) : super(typeValue);

  static const typeValue = 'writeRequest';

  final String requestId;
  final String? deviceId;
  final String serviceUuid;
  final String characteristicUuid;
  final int offset;
  final bool? preparedWrite;
  final bool? responseNeeded;
  final Uint8List value;
}

final class OmniBleSubscriptionChanged extends OmniBleEvent {
  const OmniBleSubscriptionChanged({
    required this.serviceUuid,
    required this.characteristicUuid,
    required this.subscribed,
    this.deviceId,
  }) : super(typeValue);

  static const typeValue = 'subscriptionChanged';

  final String? deviceId;
  final String serviceUuid;
  final String characteristicUuid;
  final bool subscribed;
}

final class OmniBleUnknownEvent extends OmniBleEvent {
  const OmniBleUnknownEvent(this.payload) : super('unknown');

  final Map<String, Object?> payload;
}

List<String> _stringList(Object? value) {
  if (value is List) {
    return value.map((item) => item.toString()).toList(growable: false);
  }
  return const [];
}

List<Map<Object?, Object?>> _mapList(Object? value) {
  if (value is List) {
    return value
        .whereType<Map>()
        .map((entry) => Map<Object?, Object?>.from(entry))
        .toList(growable: false);
  }
  return const [];
}

Map<String, Object?> _objectMap(Object? value) {
  if (value is Map) {
    return value.map((key, entryValue) => MapEntry(key.toString(), entryValue));
  }
  return const {};
}

Map<Object?, Object?> _rawMap(Object? value) {
  if (value is Map) {
    return Map<Object?, Object?>.from(value);
  }
  return const {};
}

Map<String, Uint8List> _bytesMap(Object? value) {
  if (value is! Map) {
    return const {};
  }
  return value.map(
    (key, entryValue) =>
        MapEntry(key.toString(), _bytesOrNull(entryValue) ?? Uint8List(0)),
  );
}

int? _intOrNull(Object? value) {
  if (value is int) {
    return value;
  }
  if (value is num) {
    return value.toInt();
  }
  if (value is String) {
    return int.tryParse(value);
  }
  return null;
}

bool? _boolOrNull(Object? value) {
  if (value is bool) {
    return value;
  }
  if (value is String) {
    switch (value) {
      case 'true':
        return true;
      case 'false':
        return false;
    }
  }
  return null;
}

Uint8List? _bytesOrNull(Object? value) {
  if (value == null) {
    return null;
  }
  if (value is Uint8List) {
    return value;
  }
  if (value is List) {
    return Uint8List.fromList(
      value.map((item) => (item as num).toInt()).toList(growable: false),
    );
  }
  return null;
}

OmniBleFeature _parseFeature(String value) {
  return OmniBleFeature.values.firstWhere(
    (feature) => feature.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown BLE feature.'),
  );
}

OmniBlePermission _parsePermission(String value) {
  return OmniBlePermission.values.firstWhere(
    (permission) => permission.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown BLE permission.'),
  );
}

OmniBlePermissionState _parsePermissionState(String value) {
  return OmniBlePermissionState.values.firstWhere(
    (state) => state.value == value,
    orElse: () => throw ArgumentError.value(
      value,
      'value',
      'Unknown BLE permission state.',
    ),
  );
}

OmniBleGattProperty _parseGattProperty(String value) {
  return OmniBleGattProperty.values.firstWhere(
    (property) => property.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown GATT property.'),
  );
}

OmniBleGattPermission _parseGattPermission(String value) {
  return OmniBleGattPermission.values.firstWhere(
    (permission) => permission.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown GATT permission.'),
  );
}

OmniBleAdapterState _parseAdapterState(String value) {
  return OmniBleAdapterState.values.firstWhere(
    (state) => state.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown adapter state.'),
  );
}

OmniBleConnectionState _parseConnectionState(String value) {
  return OmniBleConnectionState.values.firstWhere(
    (state) => state.value == value,
    orElse: () =>
        throw ArgumentError.value(value, 'value', 'Unknown connection state.'),
  );
}
