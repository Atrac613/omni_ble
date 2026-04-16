import CoreBluetooth
import Flutter
import UIKit

public class OmniBlePlugin: NSObject, FlutterPlugin, FlutterStreamHandler, CBCentralManagerDelegate,
  CBPeripheralDelegate, CBPeripheralManagerDelegate
{
  private struct ServiceDiscoveryContext {
    var remainingOperationCount: Int
  }

  private struct PendingNotificationRequest {
    let enabled: Bool
    let result: FlutterResult
  }

  private struct PendingPeripheralReadRequest {
    let request: CBATTRequest
    let characteristicKey: String
  }

  private struct PendingPeripheralWriteRequest {
    let request: CBATTRequest
    let characteristicKey: String
  }

  private let supportedFeatures = [
    "central", "scanning", "gattClient", "peripheral", "advertising", "gattServer",
    "notifications",
  ]
  private let clientCharacteristicConfigurationUuid =
    "00002902-0000-1000-8000-00805f9b34fb"
  private var centralManager: CBCentralManager?
  private var peripheralManager: CBPeripheralManager?
  private var eventSink: FlutterEventSink?
  private var currentAdapterState = "unknown"
  private var discoveredPeripherals: [UUID: CBPeripheral] = [:]
  private var pendingConnectionResults: [UUID: FlutterResult] = [:]
  private var pendingDisconnectResults: [UUID: FlutterResult] = [:]
  private var pendingDiscoveryResults: [UUID: FlutterResult] = [:]
  private var discoveryContexts: [UUID: ServiceDiscoveryContext] = [:]
  private var pendingReadResults: [String: FlutterResult] = [:]
  private var pendingWriteResults: [String: FlutterResult] = [:]
  private var pendingNotificationResults: [String: PendingNotificationRequest] = [:]
  private var pendingConnectionTimeouts: [UUID: DispatchWorkItem] = [:]
  private var characteristicCache: [UUID: [String: CBCharacteristic]] = [:]
  private var pendingGattDatabasePublishResult: FlutterResult?
  private var pendingServicesToAdd: [CBMutableService] = []
  private var pendingAdvertisingStartResult: FlutterResult?
  private var serverCharacteristics: [String: CBMutableCharacteristic] = [:]
  private var serverCharacteristicValues: [String: Data] = [:]
  private var pendingPeripheralReadRequests: [String: PendingPeripheralReadRequest] = [:]
  private var pendingPeripheralWriteRequests: [String: PendingPeripheralWriteRequest] = [:]
  private var subscribedCentrals: [String: [UUID: CBCentral]] = [:]

  public static func register(with registrar: FlutterPluginRegistrar) {
    let methodChannel = FlutterMethodChannel(
      name: "omni_ble/methods",
      binaryMessenger: registrar.messenger()
    )
    let eventChannel = FlutterEventChannel(
      name: "omni_ble/events",
      binaryMessenger: registrar.messenger()
    )
    let instance = OmniBlePlugin()
    registrar.addMethodCallDelegate(instance, channel: methodChannel)
    eventChannel.setStreamHandler(instance)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "getCapabilities":
      result(capabilitiesPayload())
    case "checkPermissions":
      checkPermissions(arguments: call.arguments, result: result)
    case "requestPermissions":
      requestPermissions(arguments: call.arguments, result: result)
    case "shouldShowRequestRationale":
      shouldShowRequestRationale(arguments: call.arguments, result: result)
    case "openAppSettings":
      openAppSettings(result: result)
    case "openBluetoothSettings":
      openBluetoothSettings(result: result)
    case "startScan":
      startScan(arguments: call.arguments, result: result)
    case "stopScan":
      centralManager?.stopScan()
      result(nil)
    case "connect":
      connect(arguments: call.arguments, result: result)
    case "disconnect":
      disconnect(arguments: call.arguments, result: result)
    case "discoverServices":
      discoverServices(arguments: call.arguments, result: result)
    case "readRssi":
      readRssi(arguments: call.arguments, result: result)
    case "requestMtu":
      requestMtu(result: result)
    case "requestConnectionPriority":
      requestConnectionPriority(result: result)
    case "setPreferredPhy":
      setPreferredPhy(result: result)
    case "readCharacteristic":
      readCharacteristic(arguments: call.arguments, result: result)
    case "readDescriptor":
      readDescriptor(arguments: call.arguments, result: result)
    case "writeCharacteristic":
      writeCharacteristic(arguments: call.arguments, result: result)
    case "writeDescriptor":
      writeDescriptor(arguments: call.arguments, result: result)
    case "setNotification":
      setNotification(arguments: call.arguments, result: result)
    case "publishGattDatabase":
      publishGattDatabase(arguments: call.arguments, result: result)
    case "clearGattDatabase":
      clearGattDatabase(result: result)
    case "startAdvertising":
      startAdvertising(arguments: call.arguments, result: result)
    case "stopAdvertising":
      stopAdvertising(result: result)
    case "notifyCharacteristicValue":
      notifyCharacteristicValue(arguments: call.arguments, result: result)
    case "respondToReadRequest":
      respondToReadRequest(arguments: call.arguments, result: result)
    case "respondToWriteRequest":
      respondToWriteRequest(arguments: call.arguments, result: result)
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  public func onListen(
    withArguments arguments: Any?,
    eventSink events: @escaping FlutterEventSink
  ) -> FlutterError? {
    eventSink = events
    ensureCentralManager()
    emitCurrentState()
    return nil
  }

  public func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    return nil
  }

  public func centralManagerDidUpdateState(_ central: CBCentralManager) {
    currentAdapterState = adapterStateString(for: central.state)
    emitCurrentState()
  }

  public func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
    currentAdapterState = adapterStateString(for: peripheral.state)
    emitCurrentState()
  }

  public func centralManager(
    _ central: CBCentralManager,
    didDiscover peripheral: CBPeripheral,
    advertisementData: [String: Any],
    rssi RSSI: NSNumber
  ) {
    discoveredPeripherals[peripheral.identifier] = peripheral
    eventSink?([
      "type": "scanResult",
      "result": scanResultPayload(
        peripheral: peripheral,
        advertisementData: advertisementData,
        rssi: RSSI
      ),
    ])
  }

  public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
    let identifier = peripheral.identifier
    peripheral.delegate = self
    discoveredPeripherals[identifier] = peripheral
    cancelConnectionTimeout(identifier)
    pendingConnectionResults.removeValue(forKey: identifier)?(nil)
    emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "connected")
  }

  public func centralManager(
    _ central: CBCentralManager,
    didFailToConnect peripheral: CBPeripheral,
    error: Error?
  ) {
    let identifier = peripheral.identifier
    cancelConnectionTimeout(identifier)
    pendingConnectionResults.removeValue(forKey: identifier)?(
      FlutterError(
        code: "connection-failed",
        message: error?.localizedDescription ?? "Bluetooth connection failed.",
        details: nil
      )
    )
    cleanupDisconnectedState(identifier)
    emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "disconnected")
  }

  public func centralManager(
    _ central: CBCentralManager,
    didDisconnectPeripheral peripheral: CBPeripheral,
    error: Error?
  ) {
    let identifier = peripheral.identifier
    cancelConnectionTimeout(identifier)

    if let connectResult = pendingConnectionResults.removeValue(forKey: identifier) {
      connectResult(
        FlutterError(
          code: "connection-failed",
          message:
            error?.localizedDescription
            ?? "Bluetooth disconnected before the connection completed.",
          details: nil
        )
      )
    }

    if let discoveryResult = pendingDiscoveryResults.removeValue(forKey: identifier) {
      discoveryResult(
        FlutterError(
          code: "device-disconnected",
          message: "Bluetooth device disconnected during service discovery.",
          details: nil
        )
      )
    }

    failPendingCharacteristicOperations(
      for: identifier,
      code: "device-disconnected",
      message: "Bluetooth device disconnected."
    )

    pendingDisconnectResults.removeValue(forKey: identifier)?(nil)
    cleanupDisconnectedState(identifier)
    emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "disconnected")
  }

  public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
    let identifier = peripheral.identifier
    guard pendingDiscoveryResults[identifier] != nil else {
      return
    }

    if let error {
      finishServiceDiscovery(
        identifier: identifier,
        error: FlutterError(
          code: "discovery-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    let services = peripheral.services ?? []
    characteristicCache[identifier] = [:]

    if services.isEmpty {
      finishServiceDiscovery(identifier: identifier, services: [])
      return
    }

    discoveryContexts[identifier] = ServiceDiscoveryContext(remainingOperationCount: services.count)
    for service in services {
      peripheral.discoverCharacteristics(nil, for: service)
    }
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didDiscoverCharacteristicsFor service: CBService,
    error: Error?
  ) {
    let identifier = peripheral.identifier

    if let error {
      finishServiceDiscovery(
        identifier: identifier,
        error: FlutterError(
          code: "discovery-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    cacheCharacteristics(for: peripheral, service: service)
    let characteristics = service.characteristics ?? []
    guard var context = discoveryContexts[identifier] else {
      return
    }

    if characteristics.isEmpty {
      advanceServiceDiscovery(identifier: identifier, services: peripheral.services ?? [])
      return
    }

    context.remainingOperationCount += characteristics.count - 1
    discoveryContexts[identifier] = context
    for characteristic in characteristics {
      peripheral.discoverDescriptors(for: characteristic)
    }
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didDiscoverDescriptorsFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    let identifier = peripheral.identifier

    if let error {
      finishServiceDiscovery(
        identifier: identifier,
        error: FlutterError(
          code: "discovery-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    advanceServiceDiscovery(identifier: identifier, services: peripheral.services ?? [])
  }

  private func advanceServiceDiscovery(identifier: UUID, services: [CBService]) {
    guard var context = discoveryContexts[identifier] else {
      return
    }

    context.remainingOperationCount -= 1
    if context.remainingOperationCount > 0 {
      discoveryContexts[identifier] = context
      return
    }

    finishServiceDiscovery(identifier: identifier, services: services)
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didReadRSSI RSSI: NSNumber,
    error: Error?
  ) {
    let operationKey = rssiOperationKey(deviceIdentifier: peripheral.identifier)

    guard let pendingResult = pendingReadResults.removeValue(forKey: operationKey) else {
      return
    }

    if let error {
      pendingResult(
        FlutterError(
          code: "read-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    pendingResult(RSSI.intValue)
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didUpdateValueFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    let address = characteristicAddress(for: peripheral, characteristic: characteristic)
    let operationKey = characteristicOperationKey(
      deviceIdentifier: peripheral.identifier,
      serviceUuid: address.serviceUuid,
      characteristicUuid: address.characteristicUuid
    )

    if let pendingResult = pendingReadResults.removeValue(forKey: operationKey) {
      if let error {
        pendingResult(
          FlutterError(
            code: "read-failed",
            message: error.localizedDescription,
            details: nil
          )
        )
      } else {
        pendingResult(FlutterStandardTypedData(bytes: characteristic.value ?? Data()))
      }
      return
    }

    guard error == nil else {
      return
    }

    eventSink?([
      "type": "characteristicValueChanged",
      "deviceId": peripheral.identifier.uuidString,
      "serviceUuid": address.serviceUuid,
      "characteristicUuid": address.characteristicUuid,
      "value": Array(characteristic.value ?? Data()),
    ])
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didUpdateValueFor descriptor: CBDescriptor,
    error: Error?
  ) {
    let address = descriptorAddress(for: peripheral, descriptor: descriptor)

    guard let pendingResult = pendingReadResults.removeValue(forKey: address.operationKey) else {
      return
    }

    if let error {
      pendingResult(
        FlutterError(
          code: "read-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    guard let value = descriptorData(from: descriptor.value) else {
      pendingResult(
        FlutterError(
          code: "invalid-response",
          message: "Bluetooth descriptor value could not be converted to bytes.",
          details: [
            "descriptorUuid": address.descriptorUuid
          ]
        )
      )
      return
    }

    pendingResult(FlutterStandardTypedData(bytes: value))
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didWriteValueFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    let address = characteristicAddress(for: peripheral, characteristic: characteristic)
    let operationKey = characteristicOperationKey(
      deviceIdentifier: peripheral.identifier,
      serviceUuid: address.serviceUuid,
      characteristicUuid: address.characteristicUuid
    )

    guard let pendingResult = pendingWriteResults.removeValue(forKey: operationKey) else {
      return
    }

    if let error {
      pendingResult(
        FlutterError(
          code: "write-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    pendingResult(nil)
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didWriteValueFor descriptor: CBDescriptor,
    error: Error?
  ) {
    let address = descriptorAddress(for: peripheral, descriptor: descriptor)

    guard let pendingResult = pendingWriteResults.removeValue(forKey: address.operationKey) else {
      return
    }

    if let error {
      pendingResult(
        FlutterError(
          code: "write-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    pendingResult(nil)
  }

  public func peripheral(
    _ peripheral: CBPeripheral,
    didUpdateNotificationStateFor characteristic: CBCharacteristic,
    error: Error?
  ) {
    let address = characteristicAddress(for: peripheral, characteristic: characteristic)
    let operationKey = characteristicOperationKey(
      deviceIdentifier: peripheral.identifier,
      serviceUuid: address.serviceUuid,
      characteristicUuid: address.characteristicUuid
    )

    guard let request = pendingNotificationResults.removeValue(forKey: operationKey) else {
      return
    }

    if let error {
      request.result(
        FlutterError(
          code: "set-notification-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    if request.enabled != characteristic.isNotifying && request.enabled {
      request.result(
        FlutterError(
          code: "set-notification-failed",
          message: "Bluetooth notifications could not be enabled.",
          details: nil
        )
      )
      return
    }

    request.result(nil)
  }

  public func peripheralManager(
    _ peripheral: CBPeripheralManager,
    didAdd service: CBService,
    error: Error?
  ) {
    guard let pendingResult = pendingGattDatabasePublishResult else {
      return
    }

    if let error {
      pendingGattDatabasePublishResult = nil
      pendingServicesToAdd.removeAll()
      pendingResult(
        FlutterError(
          code: "publish-gatt-database-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    addNextGattService(result: pendingResult)
  }

  public func peripheralManagerDidStartAdvertising(
    _ peripheral: CBPeripheralManager,
    error: Error?
  ) {
    guard let pendingResult = pendingAdvertisingStartResult else {
      return
    }

    pendingAdvertisingStartResult = nil
    if let error {
      pendingResult(
        FlutterError(
          code: "start-advertising-failed",
          message: error.localizedDescription,
          details: nil
        )
      )
      return
    }

    pendingResult(nil)
  }

  public func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest)
  {
    let requestId = UUID().uuidString
    let serviceUuid = canonicalUuidString(request.characteristic.service?.uuid ?? CBUUID(string: "0000"))
    let characteristicUuid = canonicalUuidString(request.characteristic.uuid)
    let characteristicKey = serverCharacteristicKey(
      serviceUuid: serviceUuid,
      characteristicUuid: characteristicUuid
    )

    pendingPeripheralReadRequests[requestId] = PendingPeripheralReadRequest(
      request: request,
      characteristicKey: characteristicKey
    )

    eventSink?([
      "type": "readRequest",
      "requestId": requestId,
      "deviceId": request.central.identifier.uuidString,
      "serviceUuid": serviceUuid,
      "characteristicUuid": characteristicUuid,
      "offset": request.offset,
    ])
  }

  public func peripheralManager(
    _ peripheral: CBPeripheralManager,
    didReceiveWrite requests: [CBATTRequest]
  ) {
    for request in requests {
      let requestId = UUID().uuidString
      let serviceUuid = canonicalUuidString(
        request.characteristic.service?.uuid ?? CBUUID(string: "0000"))
      let characteristicUuid = canonicalUuidString(request.characteristic.uuid)
      let characteristicKey = serverCharacteristicKey(
        serviceUuid: serviceUuid,
        characteristicUuid: characteristicUuid
      )

      pendingPeripheralWriteRequests[requestId] = PendingPeripheralWriteRequest(
        request: request,
        characteristicKey: characteristicKey
      )

      eventSink?([
        "type": "writeRequest",
        "requestId": requestId,
        "deviceId": request.central.identifier.uuidString,
        "serviceUuid": serviceUuid,
        "characteristicUuid": characteristicUuid,
        "offset": request.offset,
        "value": Array(request.value ?? Data()),
      ])
    }
  }

  public func peripheralManager(
    _ peripheral: CBPeripheralManager,
    central: CBCentral,
    didSubscribeTo characteristic: CBCharacteristic
  ) {
    let serviceUuid = canonicalUuidString(characteristic.service?.uuid ?? CBUUID(string: "0000"))
    let characteristicUuid = canonicalUuidString(characteristic.uuid)
    let characteristicKey = serverCharacteristicKey(
      serviceUuid: serviceUuid,
      characteristicUuid: characteristicUuid
    )
    subscribedCentrals[characteristicKey, default: [:]][central.identifier] = central
    eventSink?([
      "type": "subscriptionChanged",
      "deviceId": central.identifier.uuidString,
      "serviceUuid": serviceUuid,
      "characteristicUuid": characteristicUuid,
      "subscribed": true,
    ])
  }

  public func peripheralManager(
    _ peripheral: CBPeripheralManager,
    central: CBCentral,
    didUnsubscribeFrom characteristic: CBCharacteristic
  ) {
    let serviceUuid = canonicalUuidString(characteristic.service?.uuid ?? CBUUID(string: "0000"))
    let characteristicUuid = canonicalUuidString(characteristic.uuid)
    let characteristicKey = serverCharacteristicKey(
      serviceUuid: serviceUuid,
      characteristicUuid: characteristicUuid
    )
    subscribedCentrals[characteristicKey]?[central.identifier] = nil
    if subscribedCentrals[characteristicKey]?.isEmpty == true {
      subscribedCentrals.removeValue(forKey: characteristicKey)
    }
    eventSink?([
      "type": "subscriptionChanged",
      "deviceId": central.identifier.uuidString,
      "serviceUuid": serviceUuid,
      "characteristicUuid": characteristicUuid,
      "subscribed": false,
    ])
  }

  public func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
    eventSink?([
      "type": "notificationQueueReady",
    ])
  }

  private func capabilitiesPayload() -> [String: Any] {
    return [
      "platform": "ios",
      "platformVersion": UIDevice.current.systemVersion,
      "availableFeatures": supportedFeatures,
      "metadata": [
        "adapterState": currentAdapterState
      ],
    ]
  }

  private func checkPermissions(arguments: Any?, result: FlutterResult) {
    result(permissionStatusPayload(arguments: arguments))
  }

  private func requestPermissions(arguments: Any?, result: FlutterResult) {
    result(permissionStatusPayload(arguments: arguments))
  }

  private func shouldShowRequestRationale(arguments: Any?, result: FlutterResult) {
    let permissions = parseRequestedPermissions(arguments: arguments)
    result([
      "permissions": Dictionary(uniqueKeysWithValues: permissions.map { ($0, false) })
    ])
  }

  private func openAppSettings(result: FlutterResult) {
    result(false)
  }

  private func openBluetoothSettings(result: FlutterResult) {
    result(false)
  }

  private func permissionStatusPayload(arguments: Any?) -> [String: Any] {
    let permissions = parseRequestedPermissions(arguments: arguments)
    return [
      "permissions": Dictionary(uniqueKeysWithValues: permissions.map { ($0, "notRequired") }),
      "allGranted": true,
    ]
  }

  private func parseRequestedPermissions(arguments: Any?) -> [String] {
    let payload = arguments as? [String: Any]
    let rawPermissions = payload?["permissions"] as? [String] ?? []
    var seenPermissions = Set<String>()
    return rawPermissions.filter { permission in
      guard !seenPermissions.contains(permission) else {
        return false
      }
      seenPermissions.insert(permission)
      return true
    }
  }

  private func startScan(arguments: Any?, result: FlutterResult) {
    ensureCentralManager()

    guard let centralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard centralManager.state == .poweredOn else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter must be powered on before scanning.",
          details: [
            "state": currentAdapterState
          ]
        )
      )
      return
    }

    let payload = arguments as? [String: Any]
    let serviceUuids = (payload?["serviceUuids"] as? [String] ?? []).map(CBUUID.init(string:))
    let allowDuplicates = payload?["allowDuplicates"] as? Bool ?? false

    centralManager.scanForPeripherals(
      withServices: serviceUuids.isEmpty ? nil : serviceUuids,
      options: [
        CBCentralManagerScanOptionAllowDuplicatesKey: allowDuplicates
      ]
    )
    result(nil)
  }

  private func connect(arguments: Any?, result: @escaping FlutterResult) {
    ensureCentralManager()

    guard let centralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard centralManager.state == .poweredOn else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter must be powered on before connecting.",
          details: [
            "state": currentAdapterState
          ]
        )
      )
      return
    }

    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "`deviceId` is required to connect.",
          details: nil
        )
      )
      return
    }

    guard pendingConnectionResults[identifier] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth connection is already in progress for this device.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = resolvePeripheral(identifier: identifier) else {
      result(
        FlutterError(
          code: "unavailable",
          message: "Bluetooth peripheral is not available. Scan first or reconnect to a known peripheral.",
          details: nil
        )
      )
      return
    }

    if peripheral.state == .connected {
      peripheral.delegate = self
      discoveredPeripherals[identifier] = peripheral
      result(nil)
      emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "connected")
      return
    }

    if peripheral.state == .connecting {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth device is already connecting.",
          details: nil
        )
      )
      return
    }

    peripheral.delegate = self
    discoveredPeripherals[identifier] = peripheral
    pendingConnectionResults[identifier] = result
    emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "connecting")

    let timeoutMs = (payload["timeoutMs"] as? NSNumber)?.doubleValue ?? 0
    if timeoutMs > 0 {
      scheduleConnectionTimeout(identifier: identifier, timeoutMs: timeoutMs)
    }

    centralManager.connect(peripheral, options: nil)
  }

  private func disconnect(arguments: Any?, result: @escaping FlutterResult) {
    ensureCentralManager()

    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "`deviceId` is required to disconnect.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = resolvePeripheral(identifier: identifier) else {
      cleanupDisconnectedState(identifier)
      result(nil)
      return
    }

    if peripheral.state == .disconnected {
      cleanupDisconnectedState(identifier)
      result(nil)
      return
    }

    guard pendingDisconnectResults[identifier] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth disconnection is already in progress for this device.",
          details: nil
        )
      )
      return
    }

    peripheral.delegate = self
    pendingDisconnectResults[identifier] = result
    emitConnectionState(deviceId: peripheral.identifier.uuidString, state: "disconnecting")
    centralManager?.cancelPeripheralConnection(peripheral)
  }

  private func discoverServices(arguments: Any?, result: @escaping FlutterResult) {
    guard let peripheral = connectedPeripheral(from: arguments) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before discovering services.",
          details: nil
        )
      )
      return
    }

    let identifier = peripheral.identifier
    guard pendingDiscoveryResults[identifier] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth service discovery is already in progress for this device.",
          details: nil
        )
      )
      return
    }

    peripheral.delegate = self
    pendingDiscoveryResults[identifier] = result
    characteristicCache[identifier] = [:]
    peripheral.discoverServices(nil)
  }

  private func readRssi(arguments: Any?, result: @escaping FlutterResult) {
    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "`deviceId` is required to read RSSI.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before reading RSSI.",
          details: nil
        )
      )
      return
    }

    let operationKey = rssiOperationKey(deviceIdentifier: identifier)
    guard pendingReadResults[operationKey] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth RSSI read is already in progress for this device.",
          details: nil
        )
      )
      return
    }

    pendingReadResults[operationKey] = result
    peripheral.readRSSI()
  }

  private func requestMtu(result: @escaping FlutterResult) {
    result(
      FlutterError(
        code: "unsupported",
        message: "CoreBluetooth does not expose negotiated ATT MTU requests on iOS.",
        details: nil
      )
    )
  }

  private func requestConnectionPriority(result: @escaping FlutterResult) {
    result(
      FlutterError(
        code: "unsupported",
        message: "Connection priority tuning is currently only available on Android.",
        details: nil
      )
    )
  }

  private func setPreferredPhy(result: @escaping FlutterResult) {
    result(
      FlutterError(
        code: "unsupported",
        message: "Preferred PHY tuning is currently only available on Android.",
        details: nil
      )
    )
  }

  private func readCharacteristic(arguments: Any?, result: @escaping FlutterResult) {
    guard let address = parseCharacteristicAddress(arguments: arguments) else {
      result(
        FlutterError(
          code: "invalid-argument",
          message:
            "`deviceId`, `serviceUuid`, and `characteristicUuid` are required to read a characteristic.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: address.identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before reading a characteristic.",
          details: nil
        )
      )
      return
    }

    guard let characteristic = resolveCharacteristic(on: peripheral, address: address) else {
      result(
        FlutterError(
          code: "unavailable",
          message:
            "Bluetooth characteristic was not found. Call discoverServices() before reading.",
          details: nil
        )
      )
      return
    }

    let operationKey = address.operationKey
    guard pendingReadResults[operationKey] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth read is already in progress for this characteristic.",
          details: nil
        )
      )
      return
    }

    pendingReadResults[operationKey] = result
    peripheral.readValue(for: characteristic)
  }

  private func readDescriptor(arguments: Any?, result: @escaping FlutterResult) {
    guard let address = parseDescriptorAddress(arguments: arguments) else {
      result(
        FlutterError(
          code: "invalid-argument",
          message:
            "`deviceId`, `serviceUuid`, `characteristicUuid`, and `descriptorUuid` are required to read a descriptor.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: address.identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before reading a descriptor.",
          details: nil
        )
      )
      return
    }

    guard let descriptor = resolveDescriptor(on: peripheral, address: address) else {
      result(
        FlutterError(
          code: "unavailable",
          message:
            "Bluetooth descriptor was not found. Call discoverServices() before reading.",
          details: nil
        )
      )
      return
    }

    let operationKey = address.operationKey
    guard pendingReadResults[operationKey] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth read is already in progress for this descriptor.",
          details: nil
        )
      )
      return
    }

    pendingReadResults[operationKey] = result
    peripheral.readValue(for: descriptor)
  }

  private func writeCharacteristic(arguments: Any?, result: @escaping FlutterResult) {
    guard
      let payload = arguments as? [String: Any],
      let address = parseCharacteristicAddress(arguments: payload)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message:
            "`deviceId`, `serviceUuid`, `characteristicUuid`, and `value` are required to write a characteristic.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: address.identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before writing a characteristic.",
          details: nil
        )
      )
      return
    }

    guard let characteristic = resolveCharacteristic(on: peripheral, address: address) else {
      result(
        FlutterError(
          code: "unavailable",
          message:
            "Bluetooth characteristic was not found. Call discoverServices() before writing.",
          details: nil
        )
      )
      return
    }

    let value = data(from: payload["value"])
    let writeTypeValue = payload["writeType"] as? String ?? "withResponse"
    let writeType: CBCharacteristicWriteType =
      writeTypeValue == "withoutResponse" ? .withoutResponse : .withResponse

    if writeType == .withResponse {
      let operationKey = address.operationKey
      guard pendingWriteResults[operationKey] == nil else {
        result(
          FlutterError(
            code: "busy",
            message: "Bluetooth write is already in progress for this characteristic.",
            details: nil
          )
        )
        return
      }
      pendingWriteResults[operationKey] = result
    }

    peripheral.writeValue(value, for: characteristic, type: writeType)

    if writeType == .withoutResponse {
      result(nil)
    }
  }

  private func writeDescriptor(arguments: Any?, result: @escaping FlutterResult) {
    guard
      let payload = arguments as? [String: Any],
      let address = parseDescriptorAddress(arguments: payload)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message:
            "`deviceId`, `serviceUuid`, `characteristicUuid`, `descriptorUuid`, and `value` are required to write a descriptor.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: address.identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before writing a descriptor.",
          details: nil
        )
      )
      return
    }

    if address.descriptorUuid == clientCharacteristicConfigurationUuid {
      result(
        FlutterError(
          code: "unsupported",
          message:
            "Use setNotification() to update the client characteristic configuration descriptor.",
          details: nil
        )
      )
      return
    }

    guard let descriptor = resolveDescriptor(on: peripheral, address: address) else {
      result(
        FlutterError(
          code: "unavailable",
          message:
            "Bluetooth descriptor was not found. Call discoverServices() before writing.",
          details: nil
        )
      )
      return
    }

    let operationKey = address.operationKey
    guard pendingWriteResults[operationKey] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth write is already in progress for this descriptor.",
          details: nil
        )
      )
      return
    }

    pendingWriteResults[operationKey] = result
    peripheral.writeValue(data(from: payload["value"]), for: descriptor)
  }

  private func setNotification(arguments: Any?, result: @escaping FlutterResult) {
    guard
      let payload = arguments as? [String: Any],
      let address = parseCharacteristicAddress(arguments: payload)
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message:
            "`deviceId`, `serviceUuid`, `characteristicUuid`, and `enabled` are required to update notifications.",
          details: nil
        )
      )
      return
    }

    guard let peripheral = connectedPeripheral(identifier: address.identifier) else {
      result(
        FlutterError(
          code: "not-connected",
          message: "Bluetooth device must be connected before updating notifications.",
          details: nil
        )
      )
      return
    }

    guard let characteristic = resolveCharacteristic(on: peripheral, address: address) else {
      result(
        FlutterError(
          code: "unavailable",
          message:
            "Bluetooth characteristic was not found. Call discoverServices() before enabling notifications.",
          details: nil
        )
      )
      return
    }

    guard
      characteristic.properties.contains(.notify)
        || characteristic.properties.contains(.indicate)
    else {
      result(
        FlutterError(
          code: "unsupported",
          message: "Bluetooth characteristic does not support notifications or indications.",
          details: nil
        )
      )
      return
    }

    let enabled = payload["enabled"] as? Bool ?? false
    let operationKey = address.operationKey
    guard pendingNotificationResults[operationKey] == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth notification update is already in progress for this characteristic.",
          details: nil
        )
      )
      return
    }

    pendingNotificationResults[operationKey] = PendingNotificationRequest(
      enabled: enabled,
      result: result
    )
    peripheral.setNotifyValue(enabled, for: characteristic)
  }

  private func publishGattDatabase(arguments: Any?, result: @escaping FlutterResult) {
    ensurePeripheralManager()

    guard let peripheralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard peripheralManager.state == .poweredOn else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter must be powered on before publishing GATT services.",
          details: [
            "state": currentAdapterState
          ]
        )
      )
      return
    }

    guard pendingGattDatabasePublishResult == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "A Bluetooth GATT database publish is already in progress.",
          details: nil
        )
      )
      return
    }

    guard let services = buildMutableServices(arguments: arguments) else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "A valid GATT database payload is required to publish services.",
          details: nil
        )
      )
      return
    }

    clearPublishedGattDatabaseState()
    peripheralManager.removeAllServices()

    if services.isEmpty {
      result(nil)
      return
    }

    pendingGattDatabasePublishResult = result
    pendingServicesToAdd = services
    addNextGattService(result: result)
  }

  private func clearGattDatabase(result: FlutterResult) {
    peripheralManager?.removeAllServices()
    clearPublishedGattDatabaseState()
    result(nil)
  }

  private func startAdvertising(arguments: Any?, result: @escaping FlutterResult) {
    ensurePeripheralManager()

    guard let peripheralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard peripheralManager.state == .poweredOn else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth adapter must be powered on before advertising.",
          details: [
            "state": currentAdapterState
          ]
        )
      )
      return
    }

    guard pendingAdvertisingStartResult == nil else {
      result(
        FlutterError(
          code: "busy",
          message: "Bluetooth advertising is already starting.",
          details: nil
        )
      )
      return
    }

    let payload = arguments as? [String: Any]
    let localName = payload?["localName"] as? String
    let serviceUuids = (payload?["serviceUuids"] as? [String] ?? []).map(CBUUID.init(string:))

    var advertisement: [String: Any] = [:]
    if let localName, !localName.isEmpty {
      advertisement[CBAdvertisementDataLocalNameKey] = localName
    }
    if !serviceUuids.isEmpty {
      advertisement[CBAdvertisementDataServiceUUIDsKey] = serviceUuids
    }

    if peripheralManager.isAdvertising {
      peripheralManager.stopAdvertising()
    }

    pendingAdvertisingStartResult = result
    peripheralManager.startAdvertising(advertisement)
  }

  private func stopAdvertising(result: FlutterResult) {
    pendingAdvertisingStartResult = nil
    peripheralManager?.stopAdvertising()
    result(nil)
  }

  private func notifyCharacteristicValue(arguments: Any?, result: FlutterResult) {
    ensurePeripheralManager()

    guard let peripheralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard let payload = arguments as? [String: Any] else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "A characteristic address and value are required to notify subscribers.",
          details: nil
        )
      )
      return
    }

    guard
      let serviceUuid = payload["serviceUuid"] as? String,
      let characteristicUuid = payload["characteristicUuid"] as? String
    else {
      result(
        FlutterError(
          code: "invalid-argument",
          message: "`serviceUuid` and `characteristicUuid` are required to notify subscribers.",
          details: nil
        )
      )
      return
    }

    let normalizedServiceUuid = normalizedUuidKey(serviceUuid)
    let normalizedCharacteristicUuid = normalizedUuidKey(characteristicUuid)
    let characteristicKey = serverCharacteristicKey(
      serviceUuid: normalizedServiceUuid,
      characteristicUuid: normalizedCharacteristicUuid
    )

    guard let characteristic = serverCharacteristics[characteristicKey] else {
      result(
        FlutterError(
          code: "unavailable",
          message: "Bluetooth characteristic was not found. Publish a GATT database first.",
          details: nil
        )
      )
      return
    }

    let value = data(from: payload["value"])
    serverCharacteristicValues[characteristicKey] = value

    let targets: [CBCentral]?
    if let deviceId = payload["deviceId"] as? String, !deviceId.isEmpty {
      guard
        let targetIdentifier = UUID(uuidString: deviceId),
        let central = subscribedCentrals[characteristicKey]?[targetIdentifier]
      else {
        result(
          FlutterError(
            code: "unavailable",
            message: "The target central is not subscribed to this characteristic.",
            details: nil
          )
        )
        return
      }
      targets = [central]
    } else {
      let subscribers = subscribedCentrals[characteristicKey].map {
        Array($0.values)
      } ?? []
      if subscribers.isEmpty {
        result(nil)
        return
      }
      targets = subscribers
    }

    if peripheralManager.updateValue(value, for: characteristic, onSubscribedCentrals: targets) {
      result(nil)
      return
    }

    result(
      FlutterError(
        code: "busy",
        message: "Bluetooth peripheral is not ready to send another notification yet.",
        details: nil
      )
    )
  }

  private func respondToReadRequest(arguments: Any?, result: FlutterResult) {
    ensurePeripheralManager()

    guard let peripheralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard
      let payload = arguments as? [String: Any],
      let requestId = payload["requestId"] as? String,
      let pendingRequest = pendingPeripheralReadRequests.removeValue(forKey: requestId)
    else {
      result(
        FlutterError(
          code: "unavailable",
          message: "Bluetooth read request was not found or has already been answered.",
          details: nil
        )
      )
      return
    }

    let value = data(from: payload["value"])
    serverCharacteristicValues[pendingRequest.characteristicKey] = value

    guard pendingRequest.request.offset <= value.count else {
      peripheralManager.respond(to: pendingRequest.request, withResult: .invalidOffset)
      result(
        FlutterError(
          code: "invalid-argument",
          message: "The provided value is shorter than the pending read offset.",
          details: nil
        )
      )
      return
    }

    let responseValue = value.subdata(in: pendingRequest.request.offset..<value.count)
    pendingRequest.request.value = responseValue
    peripheralManager.respond(to: pendingRequest.request, withResult: .success)
    result(nil)
  }

  private func respondToWriteRequest(arguments: Any?, result: FlutterResult) {
    ensurePeripheralManager()

    guard let peripheralManager else {
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    guard
      let payload = arguments as? [String: Any],
      let requestId = payload["requestId"] as? String,
      let pendingRequest = pendingPeripheralWriteRequests.removeValue(forKey: requestId)
    else {
      result(
        FlutterError(
          code: "unavailable",
          message: "Bluetooth write request was not found or has already been answered.",
          details: nil
        )
      )
      return
    }

    let accept = payload["accept"] as? Bool ?? true
    if accept {
      let currentValue = serverCharacteristicValues[pendingRequest.characteristicKey] ?? Data()
      guard
        let mergedValue = mergedServerCharacteristicValue(
          existingValue: currentValue,
          offset: pendingRequest.request.offset,
          incomingValue: pendingRequest.request.value ?? Data()
        )
      else {
        peripheralManager.respond(to: pendingRequest.request, withResult: .invalidOffset)
        result(
          FlutterError(
            code: "invalid-argument",
            message: "The pending write request used an unsupported offset.",
            details: nil
          )
        )
        return
      }

      serverCharacteristicValues[pendingRequest.characteristicKey] = mergedValue
      peripheralManager.respond(to: pendingRequest.request, withResult: .success)
      result(nil)
      return
    }

    peripheralManager.respond(to: pendingRequest.request, withResult: .unlikelyError)
    result(nil)
  }

  private func ensureCentralManager() {
    guard centralManager == nil else {
      return
    }
    centralManager = CBCentralManager(delegate: self, queue: nil)
  }

  private func ensurePeripheralManager() {
    guard peripheralManager == nil else {
      return
    }
    peripheralManager = CBPeripheralManager(delegate: self, queue: nil)
  }

  private func resolvePeripheral(identifier: UUID) -> CBPeripheral? {
    if let peripheral = discoveredPeripherals[identifier] {
      return peripheral
    }

    guard let centralManager else {
      return nil
    }

    let peripherals = centralManager.retrievePeripherals(withIdentifiers: [identifier])
    guard let peripheral = peripherals.first else {
      return nil
    }

    discoveredPeripherals[identifier] = peripheral
    return peripheral
  }

  private func connectedPeripheral(from arguments: Any?) -> CBPeripheral? {
    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId)
    else {
      return nil
    }

    return connectedPeripheral(identifier: identifier)
  }

  private func connectedPeripheral(identifier: UUID) -> CBPeripheral? {
    guard let peripheral = resolvePeripheral(identifier: identifier), peripheral.state == .connected
    else {
      return nil
    }

    peripheral.delegate = self
    return peripheral
  }

  private func parseCharacteristicAddress(arguments: Any?) -> ParsedCharacteristicAddress? {
    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId),
      let serviceUuid = payload["serviceUuid"] as? String,
      let characteristicUuid = payload["characteristicUuid"] as? String
    else {
      return nil
    }

    return ParsedCharacteristicAddress(
      identifier: identifier,
      serviceUuid: normalizedUuidKey(serviceUuid),
      characteristicUuid: normalizedUuidKey(characteristicUuid)
    )
  }

  private func parseDescriptorAddress(arguments: Any?) -> ParsedDescriptorAddress? {
    guard
      let payload = arguments as? [String: Any],
      let deviceId = payload["deviceId"] as? String,
      let identifier = UUID(uuidString: deviceId),
      let serviceUuid = payload["serviceUuid"] as? String,
      let characteristicUuid = payload["characteristicUuid"] as? String,
      let descriptorUuid = payload["descriptorUuid"] as? String
    else {
      return nil
    }

    return ParsedDescriptorAddress(
      identifier: identifier,
      serviceUuid: normalizedUuidKey(serviceUuid),
      characteristicUuid: normalizedUuidKey(characteristicUuid),
      descriptorUuid: normalizedUuidKey(descriptorUuid)
    )
  }

  private func resolveCharacteristic(
    on peripheral: CBPeripheral,
    address: ParsedCharacteristicAddress
  ) -> CBCharacteristic? {
    let operationKey = characteristicOperationKey(
      deviceIdentifier: address.identifier,
      serviceUuid: address.serviceUuid,
      characteristicUuid: address.characteristicUuid
    )

    if let characteristic = characteristicCache[address.identifier]?[operationKey] {
      return characteristic
    }

    for service in peripheral.services ?? [] {
      let serviceUuid = canonicalUuidString(service.uuid)
      guard serviceUuid == address.serviceUuid else {
        continue
      }

      for characteristic in service.characteristics ?? [] {
        let characteristicUuid = canonicalUuidString(characteristic.uuid)
        guard characteristicUuid == address.characteristicUuid else {
          continue
        }

        characteristicCache[address.identifier, default: [:]][operationKey] = characteristic
        return characteristic
      }
    }

    return nil
  }

  private func resolveDescriptor(
    on peripheral: CBPeripheral,
    address: ParsedDescriptorAddress
  ) -> CBDescriptor? {
    guard
      let characteristic = resolveCharacteristic(
        on: peripheral,
        address: ParsedCharacteristicAddress(
          identifier: address.identifier,
          serviceUuid: address.serviceUuid,
          characteristicUuid: address.characteristicUuid
        )
      )
    else {
      return nil
    }

    return (characteristic.descriptors ?? []).first { descriptor in
      canonicalUuidString(descriptor.uuid) == address.descriptorUuid
    }
  }

  private func cacheCharacteristics(for peripheral: CBPeripheral, service: CBService) {
    let serviceUuid = canonicalUuidString(service.uuid)
    for characteristic in service.characteristics ?? [] {
      let characteristicUuid = canonicalUuidString(characteristic.uuid)
      let operationKey = characteristicOperationKey(
        deviceIdentifier: peripheral.identifier,
        serviceUuid: serviceUuid,
        characteristicUuid: characteristicUuid
      )
      characteristicCache[peripheral.identifier, default: [:]][operationKey] = characteristic
    }
  }

  private func buildMutableServices(arguments: Any?) -> [CBMutableService]? {
    guard let payload = arguments as? [String: Any] else {
      return nil
    }

    let services = payload["services"] as? [Any] ?? []
    return services.compactMap { service in
      guard let serviceMap = service as? [String: Any] else {
        return nil
      }
      return buildMutableService(serviceMap)
    }
  }

  private func buildMutableService(_ map: [String: Any]) -> CBMutableService? {
    guard
      let uuidString = map["uuid"] as? String,
      let uuid = CBUUID(string: normalizedUuidKey(uuidString)) as CBUUID?
    else {
      return nil
    }

    let service = CBMutableService(type: uuid, primary: map["primary"] as? Bool ?? true)
    let characteristics = map["characteristics"] as? [Any] ?? []
    service.characteristics = characteristics.compactMap { characteristic in
      guard let characteristicMap = characteristic as? [String: Any] else {
        return nil
      }
      return buildMutableCharacteristic(
        characteristicMap,
        serviceUuid: canonicalUuidString(uuid)
      )
    }
    return service
  }

  private func buildMutableCharacteristic(
    _ map: [String: Any],
    serviceUuid: String
  ) -> CBMutableCharacteristic? {
    guard
      let uuidString = map["uuid"] as? String,
      let uuid = CBUUID(string: normalizedUuidKey(uuidString)) as CBUUID?
    else {
      return nil
    }

    let characteristicUuid = canonicalUuidString(uuid)
    let characteristicKey = serverCharacteristicKey(
      serviceUuid: serviceUuid,
      characteristicUuid: characteristicUuid
    )
    let characteristic = CBMutableCharacteristic(
      type: uuid,
      properties: peripheralCharacteristicProperties(map["properties"]),
      value: nil,
      permissions: peripheralAttributePermissions(map["permissions"])
    )

    let descriptors = map["descriptors"] as? [Any] ?? []
    characteristic.descriptors = descriptors.compactMap { descriptor in
      guard let descriptorMap = descriptor as? [String: Any] else {
        return nil
      }
      return buildMutableDescriptor(descriptorMap)
    }

    serverCharacteristics[characteristicKey] = characteristic
    serverCharacteristicValues[characteristicKey] = data(from: map["initialValue"])
    return characteristic
  }

  private func buildMutableDescriptor(_ map: [String: Any]) -> CBMutableDescriptor? {
    guard
      let uuidString = map["uuid"] as? String,
      let uuid = CBUUID(string: normalizedUuidKey(uuidString)) as CBUUID?
    else {
      return nil
    }

    return CBMutableDescriptor(type: uuid, value: data(from: map["initialValue"]))
  }

  private func addNextGattService(result: FlutterResult) {
    guard let peripheralManager else {
      pendingGattDatabasePublishResult = nil
      pendingServicesToAdd.removeAll()
      result(
        FlutterError(
          code: "adapter-unavailable",
          message: "Bluetooth peripheral manager could not be initialized.",
          details: nil
        )
      )
      return
    }

    if pendingServicesToAdd.isEmpty {
      pendingGattDatabasePublishResult = nil
      result(nil)
      return
    }

    let service = pendingServicesToAdd.removeFirst()
    peripheralManager.add(service)
  }

  private func clearPublishedGattDatabaseState() {
    pendingGattDatabasePublishResult = nil
    pendingServicesToAdd.removeAll()
    pendingPeripheralReadRequests.removeAll()
    pendingPeripheralWriteRequests.removeAll()
    subscribedCentrals.removeAll()
    serverCharacteristics.removeAll()
    serverCharacteristicValues.removeAll()
  }

  private func serverCharacteristicKey(serviceUuid: String, characteristicUuid: String) -> String {
    return "\(serviceUuid.lowercased())|\(characteristicUuid.lowercased())"
  }

  private func peripheralCharacteristicProperties(_ value: Any?) -> CBCharacteristicProperties {
    let entries = value as? [String] ?? []
    var properties: CBCharacteristicProperties = []

    if entries.contains("read") {
      properties.insert(.read)
    }
    if entries.contains("write") {
      properties.insert(.write)
    }
    if entries.contains("writeWithoutResponse") {
      properties.insert(.writeWithoutResponse)
    }
    if entries.contains("notify") {
      properties.insert(.notify)
    }
    if entries.contains("indicate") {
      properties.insert(.indicate)
    }

    return properties
  }

  private func peripheralAttributePermissions(_ value: Any?) -> CBAttributePermissions {
    let entries = value as? [String] ?? []
    var permissions: CBAttributePermissions = []

    if entries.contains("read") {
      permissions.insert(.readable)
    }
    if entries.contains("write") {
      permissions.insert(.writeable)
    }
    if entries.contains("readEncrypted") {
      permissions.insert(.readEncryptionRequired)
    }
    if entries.contains("writeEncrypted") {
      permissions.insert(.writeEncryptionRequired)
    }

    return permissions
  }

  private func mergedServerCharacteristicValue(
    existingValue: Data,
    offset: Int,
    incomingValue: Data
  ) -> Data? {
    guard offset >= 0, offset <= existingValue.count else {
      return nil
    }

    var nextValue = existingValue
    if offset == nextValue.count {
      nextValue.append(incomingValue)
      return nextValue
    }

    if offset + incomingValue.count > nextValue.count {
      nextValue.replaceSubrange(offset..<nextValue.count, with: incomingValue)
      return nextValue
    }

    nextValue.replaceSubrange(offset..<(offset + incomingValue.count), with: incomingValue)
    return nextValue
  }

  private func finishServiceDiscovery(identifier: UUID, services: [CBService]) {
    discoveryContexts.removeValue(forKey: identifier)
    let payload = services.map(servicePayload)
    pendingDiscoveryResults.removeValue(forKey: identifier)?(payload)
  }

  private func finishServiceDiscovery(identifier: UUID, error: FlutterError) {
    discoveryContexts.removeValue(forKey: identifier)
    pendingDiscoveryResults.removeValue(forKey: identifier)?(error)
  }

  private func scheduleConnectionTimeout(identifier: UUID, timeoutMs: Double) {
    cancelConnectionTimeout(identifier)

    let timeout = DispatchWorkItem { [weak self] in
      guard let self else {
        return
      }

      guard let result = self.pendingConnectionResults.removeValue(forKey: identifier) else {
        return
      }

      self.pendingConnectionTimeouts.removeValue(forKey: identifier)
      if let peripheral = self.resolvePeripheral(identifier: identifier) {
        self.centralManager?.cancelPeripheralConnection(peripheral)
      }
      self.cleanupDisconnectedState(identifier)
      self.emitConnectionState(deviceId: identifier.uuidString, state: "disconnected")
      result(
        FlutterError(
          code: "connection-timeout",
          message: "Bluetooth connection timed out.",
          details: nil
        )
      )
    }

    pendingConnectionTimeouts[identifier] = timeout
    DispatchQueue.main.asyncAfter(
      deadline: .now() + timeoutMs / 1_000.0,
      execute: timeout
    )
  }

  private func cancelConnectionTimeout(_ identifier: UUID) {
    pendingConnectionTimeouts.removeValue(forKey: identifier)?.cancel()
  }

  private func failPendingCharacteristicOperations(
    for identifier: UUID,
    code: String,
    message: String
  ) {
    let prefix = characteristicOperationPrefix(deviceIdentifier: identifier)

    for key in pendingReadResults.keys.filter({ $0.hasPrefix(prefix) }) {
      pendingReadResults.removeValue(forKey: key)?(
        FlutterError(code: code, message: message, details: nil)
      )
    }

    for key in pendingWriteResults.keys.filter({ $0.hasPrefix(prefix) }) {
      pendingWriteResults.removeValue(forKey: key)?(
        FlutterError(code: code, message: message, details: nil)
      )
    }

    for key in pendingNotificationResults.keys.filter({ $0.hasPrefix(prefix) }) {
      pendingNotificationResults.removeValue(forKey: key)?.result(
        FlutterError(code: code, message: message, details: nil)
      )
    }
  }

  private func cleanupDisconnectedState(_ identifier: UUID) {
    characteristicCache.removeValue(forKey: identifier)
    discoveryContexts.removeValue(forKey: identifier)
  }

  private func emitCurrentState() {
    eventSink?([
      "type": "adapterStateChanged",
      "state": currentAdapterState,
    ])
  }

  private func emitConnectionState(deviceId: String, state: String) {
    eventSink?([
      "type": "connectionStateChanged",
      "deviceId": deviceId,
      "state": state,
    ])
  }

  private func adapterStateString(for state: CBManagerState) -> String {
    switch state {
    case .poweredOn:
      return "poweredOn"
    case .poweredOff:
      return "poweredOff"
    case .unauthorized:
      return "unauthorized"
    case .unsupported:
      return "unavailable"
    case .resetting:
      return "unavailable"
    case .unknown:
      return "unknown"
    @unknown default:
      return "unknown"
    }
  }

  private func scanResultPayload(
    peripheral: CBPeripheral,
    advertisementData: [String: Any],
    rssi: NSNumber
  ) -> [String: Any] {
    let serviceUuids = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? [])
      .map(canonicalUuidString)
    let serviceData =
      (advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] ?? [:]).reduce(
        into: [String: [UInt8]]()
      ) { partialResult, entry in
        partialResult[canonicalUuidString(entry.key)] = Array(entry.value)
      }
    let manufacturerData = (advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data).map(
      Array.init)
    let connectableValue = advertisementData[CBAdvertisementDataIsConnectable] as? NSNumber
    let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String

    return [
      "deviceId": peripheral.identifier.uuidString,
      "name": name as Any,
      "rssi": rssi.intValue,
      "serviceUuids": serviceUuids,
      "serviceData": serviceData,
      "manufacturerData": manufacturerData as Any,
      "connectable": connectableValue?.boolValue ?? true,
    ]
  }

  private func servicePayload(_ service: CBService) -> [String: Any] {
    return [
      "uuid": canonicalUuidString(service.uuid),
      "primary": service.isPrimary,
      "characteristics": (service.characteristics ?? []).map(characteristicPayload),
    ]
  }

  private func characteristicPayload(_ characteristic: CBCharacteristic) -> [String: Any] {
    return [
      "uuid": canonicalUuidString(characteristic.uuid),
      "properties": characteristicProperties(characteristic.properties),
      "permissions": [String](),
      "descriptors": (characteristic.descriptors ?? []).map(descriptorPayload),
      "initialValue": characteristic.value.map(Array.init) as Any,
    ]
  }

  private func descriptorPayload(_ descriptor: CBDescriptor) -> [String: Any] {
    return [
      "uuid": canonicalUuidString(descriptor.uuid),
      "permissions": [String](),
      "initialValue": descriptorData(from: descriptor.value).map(Array.init) as Any,
    ]
  }

  private func characteristicAddress(
    for peripheral: CBPeripheral,
    characteristic: CBCharacteristic
  ) -> (serviceUuid: String, characteristicUuid: String) {
    return (
      serviceUuid: canonicalUuidString(characteristic.service?.uuid ?? CBUUID(string: "0000")),
      characteristicUuid: canonicalUuidString(characteristic.uuid)
    )
  }

  private func descriptorAddress(
    for peripheral: CBPeripheral,
    descriptor: CBDescriptor
  ) -> ParsedDescriptorAddress {
    let characteristic = descriptor.characteristic
    return ParsedDescriptorAddress(
      identifier: peripheral.identifier,
      serviceUuid: canonicalUuidString(characteristic?.service?.uuid ?? CBUUID(string: "0000")),
      characteristicUuid: canonicalUuidString(characteristic?.uuid ?? CBUUID(string: "0000")),
      descriptorUuid: canonicalUuidString(descriptor.uuid)
    )
  }

  private func characteristicProperties(_ properties: CBCharacteristicProperties) -> [String] {
    var values: [String] = []

    if properties.contains(.read) {
      values.append("read")
    }
    if properties.contains(.write) {
      values.append("write")
    }
    if properties.contains(.writeWithoutResponse) {
      values.append("writeWithoutResponse")
    }
    if properties.contains(.notify) {
      values.append("notify")
    }
    if properties.contains(.indicate) {
      values.append("indicate")
    }

    return values
  }

  private func characteristicOperationPrefix(deviceIdentifier: UUID) -> String {
    return "\(deviceIdentifier.uuidString.lowercased())|"
  }

  private func characteristicOperationKey(
    deviceIdentifier: UUID,
    serviceUuid: String,
    characteristicUuid: String
  ) -> String {
    return
      "\(deviceIdentifier.uuidString.lowercased())|\(serviceUuid.lowercased())|\(characteristicUuid.lowercased())"
  }

  private func rssiOperationKey(deviceIdentifier: UUID) -> String {
    return "\(characteristicOperationPrefix(deviceIdentifier: deviceIdentifier))rssi"
  }

  private func descriptorOperationKey(
    deviceIdentifier: UUID,
    serviceUuid: String,
    characteristicUuid: String,
    descriptorUuid: String
  ) -> String {
    return
      "\(deviceIdentifier.uuidString.lowercased())|\(serviceUuid.lowercased())|\(characteristicUuid.lowercased())|\(descriptorUuid.lowercased())"
  }

  private func normalizedUuidKey(_ value: String) -> String {
    return canonicalUuidString(CBUUID(string: value))
  }

  private func canonicalUuidString(_ uuid: CBUUID) -> String {
    let hex = Array(uuid.data).map { String(format: "%02x", $0) }.joined()

    switch uuid.data.count {
    case 2:
      return "0000\(hex)-0000-1000-8000-00805f9b34fb"
    case 4:
      return "\(hex)-0000-1000-8000-00805f9b34fb"
    case 16:
      return [
        String(hex.prefix(8)),
        String(hex.dropFirst(8).prefix(4)),
        String(hex.dropFirst(12).prefix(4)),
        String(hex.dropFirst(16).prefix(4)),
        String(hex.dropFirst(20).prefix(12)),
      ].joined(separator: "-")
    default:
      return uuid.uuidString.lowercased()
    }
  }

  private func data(from value: Any?) -> Data {
    if let typedData = value as? FlutterStandardTypedData {
      return typedData.data
    }
    if let data = value as? Data {
      return data
    }
    if let values = value as? [NSNumber] {
      return Data(values.map(\.uint8Value))
    }
    if let values = value as? [UInt8] {
      return Data(values)
    }
    if let values = value as? [Int] {
      return Data(values.map(UInt8.init))
    }
    return Data()
  }

  private func descriptorData(from value: Any?) -> Data? {
    if let typedData = value as? FlutterStandardTypedData {
      return typedData.data
    }
    if let data = value as? Data {
      return data
    }
    if let values = value as? [NSNumber] {
      return Data(values.map(\.uint8Value))
    }
    if let values = value as? [UInt8] {
      return Data(values)
    }
    if let values = value as? [Int] {
      return Data(values.map(UInt8.init))
    }
    if let string = value as? String {
      return string.data(using: .utf8)
    }
    if let string = value as? NSString {
      return String(string).data(using: .utf8)
    }
    if let number = value as? NSNumber {
      var littleEndianValue = number.uint16Value.littleEndian
      return withUnsafeBytes(of: &littleEndianValue) { Data($0) }
    }
    return nil
  }
}

private struct ParsedCharacteristicAddress {
  let identifier: UUID
  let serviceUuid: String
  let characteristicUuid: String

  var operationKey: String {
    return
      "\(identifier.uuidString.lowercased())|\(serviceUuid.lowercased())|\(characteristicUuid.lowercased())"
  }
}

private struct ParsedDescriptorAddress {
  let identifier: UUID
  let serviceUuid: String
  let characteristicUuid: String
  let descriptorUuid: String

  var operationKey: String {
    return
      "\(identifier.uuidString.lowercased())|\(serviceUuid.lowercased())|\(characteristicUuid.lowercased())|\(descriptorUuid.lowercased())"
  }
}
