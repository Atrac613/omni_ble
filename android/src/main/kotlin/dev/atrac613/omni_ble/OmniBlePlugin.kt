package dev.atrac613.omni_ble

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.provider.Settings
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.PluginRegistry
import java.util.Locale
import java.util.UUID

class OmniBlePlugin :
  FlutterPlugin,
  MethodChannel.MethodCallHandler,
  EventChannel.StreamHandler,
  ActivityAware,
  PluginRegistry.RequestPermissionsResultListener {
  private data class PendingOperation(
    val type: String,
    val key: String? = null,
    val enabled: Boolean? = null,
    val result: MethodChannel.Result,
  )

  private data class ConnectionContext(
    val deviceId: String,
    val device: BluetoothDevice,
    var gatt: BluetoothGatt? = null,
    var state: String = "disconnected",
    var connectResult: MethodChannel.Result? = null,
    var connectTimeoutRunnable: Runnable? = null,
    var disconnectResult: MethodChannel.Result? = null,
    var activeOperation: PendingOperation? = null,
    val characteristics: MutableMap<String, BluetoothGattCharacteristic> = mutableMapOf(),
  )

  private data class ParsedCharacteristicAddress(
    val deviceId: String,
    val serviceUuid: String,
    val characteristicUuid: String,
  ) {
    val characteristicKey: String
      get() = "$serviceUuid|$characteristicUuid"
  }

  private data class ParsedDescriptorAddress(
    val deviceId: String,
    val serviceUuid: String,
    val characteristicUuid: String,
    val descriptorUuid: String,
  ) {
    val characteristicKey: String
      get() = "$serviceUuid|$characteristicUuid"

    val descriptorKey: String
      get() = "$serviceUuid|$characteristicUuid|$descriptorUuid"
  }

  private data class PendingServerReadRequest(
    val device: BluetoothDevice,
    val requestId: Int,
    val offset: Int,
    val characteristicKey: String,
  )

  private data class PendingServerWriteRequest(
    val device: BluetoothDevice,
    val requestId: Int,
    val offset: Int,
    val value: ByteArray,
    val responseNeeded: Boolean,
    val characteristicKey: String,
  )

  private data class PendingPermissionRequest(
    val permissions: List<String>,
    val result: MethodChannel.Result,
  )

  private companion object {
    const val OP_DISCOVER_SERVICES = "discoverServices"
    const val OP_READ_RSSI = "readRssi"
    const val OP_REQUEST_MTU = "requestMtu"
    const val OP_READ_CHARACTERISTIC = "readCharacteristic"
    const val OP_READ_DESCRIPTOR = "readDescriptor"
    const val OP_WRITE_CHARACTERISTIC = "writeCharacteristic"
    const val OP_WRITE_DESCRIPTOR = "writeDescriptor"
    const val OP_SET_NOTIFICATION = "setNotification"
    const val OP_SET_PREFERRED_PHY = "setPreferredPhy"
    const val REQUEST_CODE_PERMISSIONS = 0x0B1E
    val CLIENT_CHARACTERISTIC_CONFIGURATION_UUID: UUID =
      UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
  }

  private var applicationContext: Context? = null
  private var activity: Activity? = null
  private var activityBinding: ActivityPluginBinding? = null
  private var bluetoothManager: BluetoothManager? = null
  private var bluetoothAdapter: BluetoothAdapter? = null
  private var gattServer: BluetoothGattServer? = null
  private var methodChannel: MethodChannel? = null
  private var eventChannel: EventChannel? = null
  private var eventSink: EventChannel.EventSink? = null
  private val mainHandler = Handler(Looper.getMainLooper())
  private val seenDeviceIds = mutableSetOf<String>()
  private val connections = mutableMapOf<String, ConnectionContext>()
  private val serverCharacteristics = mutableMapOf<String, BluetoothGattCharacteristic>()
  private val serverCharacteristicValues = mutableMapOf<String, ByteArray>()
  private val pendingServerReadRequests = mutableMapOf<String, PendingServerReadRequest>()
  private val pendingServerWriteRequests = mutableMapOf<String, PendingServerWriteRequest>()
  private val subscribedDevices = mutableMapOf<String, MutableMap<String, BluetoothDevice>>()
  private val pendingServicesToPublish = ArrayDeque<BluetoothGattService>()
  private var allowDuplicates = false
  private var isScanning = false
  private var isAdvertising = false
  private var stateReceiverRegistered = false
  private var publishGattDatabaseResult: MethodChannel.Result? = null
  private var pendingStartAdvertisingResult: MethodChannel.Result? = null
  private var pendingPermissionRequest: PendingPermissionRequest? = null

  private val stateReceiver =
    object : BroadcastReceiver() {
      override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) {
          return
        }

        emitAdapterState(
          intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR),
        )
      }
    }

  private val gattCallback =
    object : BluetoothGattCallback() {
      override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return

        when (newState) {
          BluetoothProfile.STATE_CONNECTED -> {
            connection.state = "connected"
            cancelConnectTimeout(connection)
            connection.connectResult?.let { pendingResult ->
              connection.connectResult = null
              replySuccess(pendingResult, null)
            }
            emitConnectionState(deviceId, "connected")
          }

          BluetoothProfile.STATE_CONNECTING -> {
            connection.state = "connecting"
            emitConnectionState(deviceId, "connecting")
          }

          BluetoothProfile.STATE_DISCONNECTING -> {
            connection.state = "disconnecting"
            emitConnectionState(deviceId, "disconnecting")
          }

          BluetoothProfile.STATE_DISCONNECTED -> {
            val message =
              if (status == BluetoothGatt.GATT_SUCCESS) {
                "Bluetooth device disconnected."
              } else {
                "Bluetooth connection closed with status $status."
              }
            handleDisconnected(connection, status, message)
          }
        }
      }

      override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation

        if (operation?.type != OP_DISCOVER_SERVICES) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "discovery-failed",
            "Bluetooth service discovery failed with status $status.",
            mapOf("status" to status),
          )
          return
        }

        rebuildCharacteristicCache(connection, gatt)
        replySuccess(
          operation.result,
          gatt.services.map(::servicePayload),
        )
      }

      override fun onReadRemoteRssi(gatt: BluetoothGatt, rssi: Int, status: Int) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation

        if (operation?.type != OP_READ_RSSI || operation.key != deviceId) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "read-failed",
            "Bluetooth RSSI read failed with status $status.",
            mapOf("status" to status),
          )
          return
        }

        replySuccess(operation.result, rssi)
      }

      override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation

        emitMtuChanged(deviceId, mtu, status)

        if (operation?.type != OP_REQUEST_MTU || operation.key != deviceId) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "request-mtu-failed",
            "Bluetooth MTU request failed with status $status.",
            mapOf("status" to status, "mtu" to mtu),
          )
          return
        }

        replySuccess(operation.result, mtu)
      }

      override fun onPhyUpdate(gatt: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation

        emitPhyUpdated(deviceId, txPhy, rxPhy, status)

        if (operation?.type != OP_SET_PREFERRED_PHY || operation.key != deviceId) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "set-preferred-phy-failed",
            "Bluetooth preferred PHY update failed with status $status.",
            mapOf("status" to status, "txPhy" to phyValue(txPhy), "rxPhy" to phyValue(rxPhy)),
          )
          return
        }

        replySuccess(operation.result, null)
      }

      @Suppress("DEPRECATION")
      override fun onCharacteristicRead(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        status: Int,
      ) {
        handleCharacteristicRead(gatt, characteristic, characteristic.value, status)
      }

      override fun onCharacteristicRead(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        value: ByteArray,
        status: Int,
      ) {
        handleCharacteristicRead(gatt, characteristic, value, status)
      }

      @Suppress("DEPRECATION")
      override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
      ) {
        emitCharacteristicValueChanged(gatt, characteristic, characteristic.value ?: ByteArray(0))
      }

      override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        value: ByteArray,
      ) {
        emitCharacteristicValueChanged(gatt, characteristic, value)
      }

      override fun onCharacteristicWrite(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        status: Int,
      ) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation
        val characteristicKey =
          characteristicKey(
            normalizeUuidString(characteristic.service.uuid.toString()),
            normalizeUuidString(characteristic.uuid.toString()),
          )

        if (operation?.type != OP_WRITE_CHARACTERISTIC || operation.key != characteristicKey) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "write-failed",
            "Bluetooth characteristic write failed with status $status.",
            mapOf("status" to status),
          )
          return
        }

        replySuccess(operation.result, null)
      }

      @Suppress("DEPRECATION")
      override fun onDescriptorRead(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int,
      ) {
        handleDescriptorRead(gatt, descriptor, descriptor.value, status)
      }

      override fun onDescriptorRead(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int,
        value: ByteArray,
      ) {
        handleDescriptorRead(gatt, descriptor, value, status)
      }

      override fun onDescriptorWrite(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int,
      ) {
        val deviceId = normalizeDeviceId(gatt.device.address)
        val connection = connections[deviceId] ?: return
        val operation = connection.activeOperation
        val characteristicKey =
          characteristicKey(
            normalizeUuidString(descriptor.characteristic.service.uuid.toString()),
            normalizeUuidString(descriptor.characteristic.uuid.toString()),
          )
        val descriptorKey =
          descriptorKey(
            normalizeUuidString(descriptor.characteristic.service.uuid.toString()),
            normalizeUuidString(descriptor.characteristic.uuid.toString()),
            normalizeUuidString(descriptor.uuid.toString()),
          )

        if (operation?.type == OP_SET_NOTIFICATION && operation.key == characteristicKey) {
          connection.activeOperation = null
          if (status != BluetoothGatt.GATT_SUCCESS) {
            replyError(
              operation.result,
              "set-notification-failed",
              "Bluetooth notification update failed with status $status.",
              mapOf("status" to status),
            )
            return
          }

          replySuccess(operation.result, null)
          return
        }

        if (operation?.type != OP_WRITE_DESCRIPTOR || operation.key != descriptorKey) {
          return
        }

        connection.activeOperation = null
        if (status != BluetoothGatt.GATT_SUCCESS) {
          replyError(
            operation.result,
            "write-failed",
            "Bluetooth descriptor write failed with status $status.",
            mapOf("status" to status),
          )
          return
        }

        replySuccess(operation.result, null)
      }
    }

  private val gattServerCallback =
    object : BluetoothGattServerCallback() {
      override fun onServiceAdded(status: Int, service: BluetoothGattService?) {
        val pendingResult = publishGattDatabaseResult ?: return

        if (status != BluetoothGatt.GATT_SUCCESS) {
          publishGattDatabaseResult = null
          pendingServicesToPublish.clear()
          replyError(
            pendingResult,
            "publish-gatt-database-failed",
            "Bluetooth GATT database publish failed with status $status.",
            mapOf("status" to status),
          )
          return
        }

        addNextGattServerService(pendingResult)
      }

      override fun onCharacteristicReadRequest(
        device: BluetoothDevice,
        requestId: Int,
        offset: Int,
        characteristic: BluetoothGattCharacteristic,
      ) {
        val requestKey = UUID.randomUUID().toString()
        val characteristicKey =
          characteristicKey(
            normalizeUuidString(characteristic.service.uuid.toString()),
            normalizeUuidString(characteristic.uuid.toString()),
          )

        pendingServerReadRequests[requestKey] =
          PendingServerReadRequest(
            device = device,
            requestId = requestId,
            offset = offset,
            characteristicKey = characteristicKey,
          )

        emitEvent(
          mapOf(
            "type" to "readRequest",
            "requestId" to requestKey,
            "deviceId" to normalizeDeviceId(device.address),
            "serviceUuid" to normalizeUuidString(characteristic.service.uuid.toString()),
            "characteristicUuid" to normalizeUuidString(characteristic.uuid.toString()),
            "offset" to offset,
          ),
        )
      }

      override fun onCharacteristicWriteRequest(
        device: BluetoothDevice,
        requestId: Int,
        characteristic: BluetoothGattCharacteristic,
        preparedWrite: Boolean,
        responseNeeded: Boolean,
        offset: Int,
        value: ByteArray,
      ) {
        val requestKey = UUID.randomUUID().toString()
        val characteristicKey =
          characteristicKey(
            normalizeUuidString(characteristic.service.uuid.toString()),
            normalizeUuidString(characteristic.uuid.toString()),
          )

        pendingServerWriteRequests[requestKey] =
          PendingServerWriteRequest(
            device = device,
            requestId = requestId,
            offset = offset,
            value = value,
            responseNeeded = responseNeeded,
            characteristicKey = characteristicKey,
          )

        emitEvent(
          mapOf(
            "type" to "writeRequest",
            "requestId" to requestKey,
            "deviceId" to normalizeDeviceId(device.address),
            "serviceUuid" to normalizeUuidString(characteristic.service.uuid.toString()),
            "characteristicUuid" to normalizeUuidString(characteristic.uuid.toString()),
            "offset" to offset,
            "preparedWrite" to preparedWrite,
            "responseNeeded" to responseNeeded,
            "value" to value.asUnsignedList(),
          ),
        )
      }

      @Suppress("DEPRECATION")
      override fun onDescriptorReadRequest(
        device: BluetoothDevice,
        requestId: Int,
        offset: Int,
        descriptor: BluetoothGattDescriptor,
      ) {
        val currentValue =
          if (descriptor.uuid == CLIENT_CHARACTERISTIC_CONFIGURATION_UUID) {
            cccdValueForDevice(descriptor.characteristic, device)
          } else {
            descriptor.value ?: ByteArray(0)
          }

        sendGattServerResponse(
          device = device,
          requestId = requestId,
          status =
            if (offset > currentValue.size) {
              BluetoothGatt.GATT_INVALID_OFFSET
            } else {
              BluetoothGatt.GATT_SUCCESS
            },
          offset = offset,
          value = currentValue.copyOfRange(offset.coerceAtMost(currentValue.size), currentValue.size),
        )
      }

      @Suppress("DEPRECATION")
      override fun onDescriptorWriteRequest(
        device: BluetoothDevice,
        requestId: Int,
        descriptor: BluetoothGattDescriptor,
        preparedWrite: Boolean,
        responseNeeded: Boolean,
        offset: Int,
        value: ByteArray,
      ) {
        if (descriptor.uuid == CLIENT_CHARACTERISTIC_CONFIGURATION_UUID) {
          updateDeviceSubscription(descriptor.characteristic, device, value)
        } else {
          descriptor.value = value
        }

        if (responseNeeded) {
          sendGattServerResponse(
            device = device,
            requestId = requestId,
            status = BluetoothGatt.GATT_SUCCESS,
            offset = offset,
            value = null,
          )
        }
      }

      override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
        if (newState == BluetoothProfile.STATE_DISCONNECTED) {
          removeDeviceFromSubscriptions(device)
        }
      }

      override fun onNotificationSent(device: BluetoothDevice, status: Int) {
        emitEvent(
          mapOf(
            "type" to "notificationQueueReady",
            "deviceId" to normalizeDeviceId(device.address),
            "status" to status,
          ),
        )
      }
    }

  private val advertiseCallback =
    object : AdvertiseCallback() {
      override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
        isAdvertising = true
        pendingStartAdvertisingResult?.let { pendingResult ->
          pendingStartAdvertisingResult = null
          replySuccess(pendingResult, null)
        }
      }

      override fun onStartFailure(errorCode: Int) {
        isAdvertising = false
        pendingStartAdvertisingResult?.let { pendingResult ->
          pendingStartAdvertisingResult = null
          replyError(
            pendingResult,
            "start-advertising-failed",
            "Bluetooth advertising failed to start with error code $errorCode.",
            mapOf("errorCode" to errorCode),
          )
        }
      }
    }

  private val scanCallback =
    object : ScanCallback() {
      override fun onScanResult(callbackType: Int, result: ScanResult) {
        emitScanResult(result)
      }

      override fun onBatchScanResults(results: MutableList<ScanResult>) {
        results.forEach(::emitScanResult)
      }

      override fun onScanFailed(errorCode: Int) {
        emitEvent(
          mapOf(
            "type" to "scanError",
            "code" to errorCode,
          ),
        )
      }
    }

  override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
    applicationContext = flutterPluginBinding.applicationContext
    bluetoothManager =
      applicationContext?.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    bluetoothAdapter = bluetoothManager?.adapter

    methodChannel =
      MethodChannel(flutterPluginBinding.binaryMessenger, "omni_ble/methods").also {
        it.setMethodCallHandler(this)
      }
    eventChannel =
      EventChannel(flutterPluginBinding.binaryMessenger, "omni_ble/events").also {
        it.setStreamHandler(this)
      }
  }

  override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
    when (call.method) {
      "getCapabilities" -> result.success(capabilitiesPayload())
      "checkPermissions" -> checkPermissions(call, result)
      "requestPermissions" -> requestPermissions(call, result)
      "shouldShowRequestRationale" -> shouldShowRequestRationale(call, result)
      "openAppSettings" -> openAppSettings(result)
      "openBluetoothSettings" -> openBluetoothSettings(result)
      "startScan" -> startScan(call, result)
      "stopScan" -> {
        stopActiveScan()
        result.success(null)
      }
      "connect" -> connect(call, result)
      "disconnect" -> disconnect(call, result)
      "discoverServices" -> discoverServices(call, result)
      "readRssi" -> readRssi(call, result)
      "requestMtu" -> requestMtu(call, result)
      "requestConnectionPriority" -> requestConnectionPriority(call, result)
      "setPreferredPhy" -> setPreferredPhy(call, result)
      "readCharacteristic" -> readCharacteristic(call, result)
      "readDescriptor" -> readDescriptor(call, result)
      "writeCharacteristic" -> writeCharacteristic(call, result)
      "writeDescriptor" -> writeDescriptor(call, result)
      "setNotification" -> setNotification(call, result)
      "publishGattDatabase" -> publishGattDatabase(call, result)
      "clearGattDatabase" -> {
        clearGattDatabase()
        result.success(null)
      }
      "startAdvertising" -> startAdvertising(call, result)
      "stopAdvertising" -> {
        stopAdvertising()
        result.success(null)
      }
      "notifyCharacteristicValue" -> notifyCharacteristicValue(call, result)
      "respondToReadRequest" -> respondToReadRequest(call, result)
      "respondToWriteRequest" -> respondToWriteRequest(call, result)
      else -> result.notImplemented()
    }
  }

  override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
    eventSink = events
    registerStateReceiver()
    emitAdapterState()
  }

  override fun onCancel(arguments: Any?) {
    eventSink = null
    unregisterStateReceiver()
  }

  override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
    clearPendingPermissionRequest(
      code = "cancelled",
      message = "The Android BLE permission request was cancelled because the plugin detached.",
    )
    detachFromActivityBinding()
    stopActiveScan()
    stopAdvertising()
    unregisterStateReceiver()
    closeAllConnections()
    clearGattDatabase()
    closeGattServer()
    eventChannel?.setStreamHandler(null)
    methodChannel?.setMethodCallHandler(null)
    eventSink = null
    eventChannel = null
    methodChannel = null
    bluetoothAdapter = null
    bluetoothManager = null
    applicationContext = null
  }

  override fun onAttachedToActivity(binding: ActivityPluginBinding) {
    activity = binding.activity
    activityBinding = binding
    binding.addRequestPermissionsResultListener(this)
  }

  override fun onDetachedFromActivityForConfigChanges() {
    detachFromActivityBinding()
  }

  override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
    onAttachedToActivity(binding)
  }

  override fun onDetachedFromActivity() {
    clearPendingPermissionRequest(
      code = "cancelled",
      message = "The Android BLE permission request was cancelled because the activity detached.",
    )
    detachFromActivityBinding()
  }

  override fun onRequestPermissionsResult(
    requestCode: Int,
    permissions: Array<out String>,
    grantResults: IntArray,
  ): Boolean {
    if (requestCode != REQUEST_CODE_PERMISSIONS) {
      return false
    }

    val pendingRequest = pendingPermissionRequest ?: return false
    pendingPermissionRequest = null
    replySuccess(pendingRequest.result, permissionStatusPayload(pendingRequest.permissions))
    return true
  }

  private fun capabilitiesPayload(): Map<String, Any?> {
    val features = linkedSetOf<String>()
    if (supportsBleCentral()) {
      features += listOf("central", "scanning", "gattClient", "notifications")
    }
    if (supportsGattServer()) {
      features += listOf("peripheral", "gattServer", "notifications")
    }
    if (supportsAdvertising()) {
      features += "advertising"
    }

    return mapOf(
      "platform" to "android",
      "platformVersion" to Build.VERSION.RELEASE,
      "availableFeatures" to features.toList(),
      "metadata" to
        mapOf(
          "adapterState" to currentAdapterState(),
        ),
    )
  }

  private fun checkPermissions(call: MethodCall, result: MethodChannel.Result) {
    val permissions = parseRequestedPermissions(call.arguments)
    if (permissions == null) {
      result.error(
        "invalid-argument",
        "`permissions` must be a list containing only `scan`, `connect`, or `advertise`.",
        null,
      )
      return
    }

    replySuccess(result, permissionStatusPayload(permissions))
  }

  private fun requestPermissions(call: MethodCall, result: MethodChannel.Result) {
    val permissions = parseRequestedPermissions(call.arguments)
    if (permissions == null) {
      result.error(
        "invalid-argument",
        "`permissions` must be a list containing only `scan`, `connect`, or `advertise`.",
        null,
      )
      return
    }

    val status = permissionStatusPayload(permissions)
    if (status["allGranted"] == true) {
      replySuccess(result, status)
      return
    }

    val androidPermissions = permissions.flatMap { requiredAndroidPermissions(it).orEmpty() }.distinct()
    if (androidPermissions.isEmpty()) {
      replySuccess(result, status)
      return
    }

    val attachedActivity = activity
    if (attachedActivity == null) {
      result.error(
        "no-activity",
        "An Android activity is required to request Bluetooth permissions.",
        null,
      )
      return
    }

    if (pendingPermissionRequest != null) {
      result.error(
        "busy",
        "Another Android BLE permission request is already in progress.",
        null,
      )
      return
    }

    pendingPermissionRequest = PendingPermissionRequest(permissions = permissions, result = result)
    ActivityCompat.requestPermissions(
      attachedActivity,
      androidPermissions.toTypedArray(),
      REQUEST_CODE_PERMISSIONS,
    )
  }

  private fun shouldShowRequestRationale(
    call: MethodCall,
    result: MethodChannel.Result,
  ) {
    val permissions = parseRequestedPermissions(call.arguments)
    if (permissions == null) {
      result.error(
        "invalid-argument",
        "`permissions` must be a list containing only `scan`, `connect`, or `advertise`.",
        null,
      )
      return
    }

    replySuccess(result, permissionRationalePayload(permissions))
  }

  private fun openAppSettings(result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val intent =
      Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
        .setData(Uri.fromParts("package", context.packageName, null))
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    replySuccess(result, launchSettingsIntent(context, intent))
  }

  private fun openBluetoothSettings(result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val intent =
      Intent(Settings.ACTION_BLUETOOTH_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    replySuccess(result, launchSettingsIntent(context, intent))
  }

  private fun supportsBleCentral(): Boolean {
    val context = applicationContext ?: return false
    return bluetoothAdapter != null &&
      context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)
  }

  private fun supportsGattServer(): Boolean {
    return supportsBleCentral()
  }

  @SuppressLint("MissingPermission")
  private fun supportsAdvertising(): Boolean {
    return try {
      supportsBleCentral() && bluetoothAdapter?.bluetoothLeAdvertiser != null
    } catch (_: SecurityException) {
      false
    }
  }

  @SuppressLint("MissingPermission")
  private fun startScan(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingScanPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth scan permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    if (currentAdapterState() != "poweredOn") {
      result.error(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before scanning.",
        mapOf("state" to currentAdapterState()),
      )
      return
    }

    val scanner =
      try {
        bluetoothAdapter?.bluetoothLeScanner
      } catch (_: SecurityException) {
        null
      }

    if (scanner == null) {
      result.error("unavailable", "Bluetooth LE scanner is unavailable.", null)
      return
    }

    val arguments = call.arguments as? Map<*, *>
    val serviceUuids =
      (arguments?.get("serviceUuids") as? List<*>)
        ?.mapNotNull { it as? String }
        .orEmpty()
    allowDuplicates = arguments?.get("allowDuplicates") as? Boolean ?: false
    stopActiveScan()
    seenDeviceIds.clear()

    try {
      scanner.startScan(
        buildScanFilters(serviceUuids),
        buildScanSettings(),
        scanCallback,
      )
      isScanning = true
      result.success(null)
    } catch (error: SecurityException) {
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth scan permission is missing on Android.",
        missingPermissions,
      )
    } catch (error: IllegalArgumentException) {
      result.error(
        "invalid-argument",
        error.message ?: "Invalid scan configuration.",
        null,
      )
    }
  }

  private fun connect(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    if (currentAdapterState() != "poweredOn") {
      result.error(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before connecting.",
        mapOf("state" to currentAdapterState()),
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    if (deviceId == null) {
      result.error("invalid-argument", "`deviceId` is required to connect.", null)
      return
    }

    val existingConnection = connections[deviceId]
    if (existingConnection?.state == "connected" && existingConnection.gatt != null) {
      replySuccess(result, null)
      return
    }
    if (existingConnection?.connectResult != null || existingConnection?.state == "connecting") {
      result.error(
        "busy",
        "Bluetooth connection is already in progress for this device.",
        null,
      )
      return
    }

    if (existingConnection != null) {
      cleanupConnection(deviceId)
    }

    val device =
      try {
        bluetoothAdapter?.getRemoteDevice(deviceId)
      } catch (_: IllegalArgumentException) {
        null
      } catch (_: SecurityException) {
        null
      }

    if (device == null) {
      result.error(
        "unavailable",
        "Bluetooth device `$deviceId` is not available.",
        null,
      )
      return
    }

    val connection =
      ConnectionContext(
        deviceId = deviceId,
        device = device,
        state = "connecting",
        connectResult = result,
      )
    connections[deviceId] = connection
    emitConnectionState(deviceId, "connecting")

    val timeoutMs = (call.argument<Number>("timeoutMs")?.toLong() ?: 0L).coerceAtLeast(0L)
    if (timeoutMs > 0) {
      scheduleConnectTimeout(connection, timeoutMs)
    }

    val autoConnect = call.argument<Boolean>("androidAutoConnect") ?: false
    try {
      val gatt =
        device.connectGatt(
          context,
          autoConnect,
          gattCallback,
          BluetoothDevice.TRANSPORT_LE,
        )

      if (gatt == null) {
        cancelConnectTimeout(connection)
        connections.remove(deviceId)
        result.error("connection-failed", "Bluetooth GATT could not be opened.", null)
        return
      }

      connection.gatt = gatt
    } catch (error: SecurityException) {
      cancelConnectTimeout(connection)
      connections.remove(deviceId)
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun disconnect(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    if (deviceId == null) {
      result.error("invalid-argument", "`deviceId` is required to disconnect.", null)
      return
    }

    val connection = connections[deviceId]
    if (connection == null) {
      result.success(null)
      return
    }

    if (connection.disconnectResult != null) {
      result.error(
        "busy",
        "Bluetooth disconnection is already in progress for this device.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null || connection.state == "disconnected") {
      cleanupConnection(deviceId)
      result.success(null)
      return
    }

    connection.disconnectResult = result
    connection.state = "disconnecting"
    emitConnectionState(deviceId, "disconnecting")

    try {
      gatt.disconnect()
    } catch (error: SecurityException) {
      connection.disconnectResult = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun discoverServices(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    val connection = deviceId?.let(::connectedConnection)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before discovering services.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    connection.activeOperation = PendingOperation(type = OP_DISCOVER_SERVICES, result = result)
    try {
      if (!gatt.discoverServices()) {
        connection.activeOperation = null
        result.error("discovery-failed", "Bluetooth service discovery could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun readRssi(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val payload = call.arguments as? Map<*, *>
    val deviceId = (payload?.get("deviceId") as? String)?.let(::normalizeDeviceId)
    if (deviceId == null) {
      result.error("invalid-argument", "`deviceId` is required to read RSSI.", null)
      return
    }

    val connection = connectedConnection(deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before reading RSSI.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    connection.activeOperation =
      PendingOperation(
        type = OP_READ_RSSI,
        key = deviceId,
        result = result,
      )

    try {
      if (!gatt.readRemoteRssi()) {
        connection.activeOperation = null
        result.error("read-failed", "Bluetooth RSSI read could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun requestMtu(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    if (deviceId == null) {
      result.error("invalid-argument", "`deviceId` is required to request MTU.", null)
      return
    }

    val connection = connectedConnection(deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before requesting MTU.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    val mtu = (call.argument<Number>("mtu")?.toInt() ?: 512).coerceIn(23, 517)
    connection.activeOperation =
      PendingOperation(
        type = OP_REQUEST_MTU,
        key = deviceId,
        result = result,
      )

    try {
      if (!gatt.requestMtu(mtu)) {
        connection.activeOperation = null
        result.error("request-mtu-failed", "Bluetooth MTU request could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun requestConnectionPriority(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    if (deviceId == null) {
      result.error(
        "invalid-argument",
        "`deviceId` is required to request connection priority.",
        null,
      )
      return
    }

    val connection = connectedConnection(deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before requesting connection priority.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    val priority =
      connectionPriorityFromValue(call.argument<String>("priority"))
        ?: run {
          result.error(
            "invalid-argument",
            "`priority` must be `balanced`, `high`, or `lowPower`.",
            null,
          )
          return
        }

    try {
      if (!gatt.requestConnectionPriority(priority)) {
        result.error(
          "request-connection-priority-failed",
          "Bluetooth connection priority request could not start.",
          null,
        )
        return
      }
    } catch (error: SecurityException) {
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    result.success(null)
  }

  @SuppressLint("MissingPermission")
  private fun setPreferredPhy(call: MethodCall, result: MethodChannel.Result) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
      result.error(
        "unsupported",
        "Bluetooth preferred PHY updates require Android 8.0 or newer.",
        null,
      )
      return
    }

    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val deviceId = parseDeviceId(call.arguments)
    if (deviceId == null) {
      result.error("invalid-argument", "`deviceId` is required to set preferred PHY.", null)
      return
    }

    val connection = connectedConnection(deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before setting preferred PHY.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    val txPhy =
      preferredPhyFromValue(call.argument<String>("txPhy"))
        ?: run {
          result.error(
            "invalid-argument",
            "`txPhy` must be `le1m`, `le2m`, or `leCoded`.",
            null,
          )
          return
        }
    val rxPhy =
      preferredPhyFromValue(call.argument<String>("rxPhy"))
        ?: run {
          result.error(
            "invalid-argument",
            "`rxPhy` must be `le1m`, `le2m`, or `leCoded`.",
            null,
          )
          return
        }
    val coding =
      phyOptionsFromValue(call.argument<String>("coding"))
        ?: run {
          result.error(
            "invalid-argument",
            "`coding` must be `unspecified`, `s2`, or `s8`.",
            null,
          )
          return
        }

    connection.activeOperation =
      PendingOperation(
        type = OP_SET_PREFERRED_PHY,
        key = deviceId,
        result = result,
      )

    try {
      gatt.setPreferredPhy(txPhy, rxPhy, coding)
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun readCharacteristic(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val address = parseCharacteristicAddress(call.arguments)
    if (address == null) {
      result.error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, and `characteristicUuid` are required to read a characteristic.",
        null,
      )
      return
    }

    val connection = connectedConnection(address.deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before reading a characteristic.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val characteristic = findCharacteristic(connection, address)
    if (characteristic == null) {
      result.error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before reading.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    connection.activeOperation =
      PendingOperation(
        type = OP_READ_CHARACTERISTIC,
        key = address.characteristicKey,
        result = result,
      )

    try {
      if (!gatt.readCharacteristic(characteristic)) {
        connection.activeOperation = null
        result.error("read-failed", "Bluetooth read could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun readDescriptor(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val address = parseDescriptorAddress(call.arguments)
    if (address == null) {
      result.error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `descriptorUuid` are required to read a descriptor.",
        null,
      )
      return
    }

    val connection = connectedConnection(address.deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before reading a descriptor.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val descriptor = findDescriptor(connection, address)
    if (descriptor == null) {
      result.error(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before reading.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    connection.activeOperation =
      PendingOperation(
        type = OP_READ_DESCRIPTOR,
        key = address.descriptorKey,
        result = result,
      )

    try {
      if (!gatt.readDescriptor(descriptor)) {
        connection.activeOperation = null
        result.error("read-failed", "Bluetooth descriptor read could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun writeCharacteristic(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val address = parseCharacteristicAddress(call.arguments)
    if (address == null) {
      result.error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `value` are required to write a characteristic.",
        null,
      )
      return
    }

    val connection = connectedConnection(address.deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before writing a characteristic.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val characteristic = findCharacteristic(connection, address)
    if (characteristic == null) {
      result.error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before writing.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    val payload = call.arguments as? Map<*, *>
    val value = payload?.get("value").toByteArray()
    val writeType =
      if ((payload?.get("writeType") as? String) == "withoutResponse") {
        BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
      } else {
        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
      }

    if (writeType == BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) {
      connection.activeOperation =
        PendingOperation(
          type = OP_WRITE_CHARACTERISTIC,
          key = address.characteristicKey,
          result = result,
        )
    }

    try {
      val started = writeCharacteristic(gatt, characteristic, value, writeType)
      if (!started) {
        connection.activeOperation = null
        result.error("write-failed", "Bluetooth write could not start.", null)
        return
      }

      if (writeType != BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) {
        replySuccess(result, null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun writeDescriptor(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val address = parseDescriptorAddress(call.arguments)
    if (address == null) {
      result.error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, `descriptorUuid`, and `value` are required to write a descriptor.",
        null,
      )
      return
    }

    if (address.descriptorUuid == CLIENT_CHARACTERISTIC_CONFIGURATION_UUID.toString()) {
      result.error(
        "unsupported",
        "Use setNotification() to update the client characteristic configuration descriptor.",
        null,
      )
      return
    }

    val connection = connectedConnection(address.deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before writing a descriptor.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val descriptor = findDescriptor(connection, address)
    if (descriptor == null) {
      result.error(
        "unavailable",
        "Bluetooth descriptor was not found. Call discoverServices() before writing.",
        null,
      )
      return
    }

    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    val payload = call.arguments as? Map<*, *>
    val value = payload?.get("value").toByteArray()
    connection.activeOperation =
      PendingOperation(
        type = OP_WRITE_DESCRIPTOR,
        key = address.descriptorKey,
        result = result,
      )

    try {
      val started = writeDescriptor(gatt, descriptor, value)
      if (!started) {
        connection.activeOperation = null
        result.error("write-failed", "Bluetooth descriptor write could not start.", null)
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun setNotification(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val address = parseCharacteristicAddress(call.arguments)
    if (address == null) {
      result.error(
        "invalid-argument",
        "`deviceId`, `serviceUuid`, `characteristicUuid`, and `enabled` are required to update notifications.",
        null,
      )
      return
    }

    val connection = connectedConnection(address.deviceId)
    if (connection == null) {
      result.error(
        "not-connected",
        "Bluetooth device must be connected before updating notifications.",
        null,
      )
      return
    }

    if (connection.activeOperation != null) {
      result.error("busy", "Another Bluetooth GATT operation is already in progress.", null)
      return
    }

    val characteristic = findCharacteristic(connection, address)
    if (characteristic == null) {
      result.error(
        "unavailable",
        "Bluetooth characteristic was not found. Call discoverServices() before enabling notifications.",
        null,
      )
      return
    }

    val supportsNotify =
      characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0
    val supportsIndicate =
      characteristic.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0
    if (!supportsNotify && !supportsIndicate) {
      result.error(
        "unsupported",
        "Bluetooth characteristic does not support notifications or indications.",
        null,
      )
      return
    }

    val enabled = call.argument<Boolean>("enabled") ?: false
    val gatt = connection.gatt
    if (gatt == null) {
      result.error("not-connected", "Bluetooth GATT is not available.", null)
      return
    }

    try {
      if (!gatt.setCharacteristicNotification(characteristic, enabled)) {
        result.error(
          "set-notification-failed",
          "Bluetooth notifications could not be updated.",
          null,
        )
        return
      }

      val cccd = characteristic.getDescriptor(CLIENT_CHARACTERISTIC_CONFIGURATION_UUID)
      if (cccd == null) {
        replySuccess(result, null)
        return
      }

      val descriptorValue =
        when {
          !enabled -> BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
          supportsIndicate -> BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
          else -> BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        }

      connection.activeOperation =
        PendingOperation(
          type = OP_SET_NOTIFICATION,
          key = address.characteristicKey,
          enabled = enabled,
          result = result,
        )

      val started = writeDescriptor(gatt, cccd, descriptorValue)
      if (!started) {
        connection.activeOperation = null
        result.error(
          "set-notification-failed",
          "Bluetooth notifications could not be updated.",
          null,
        )
      }
    } catch (error: SecurityException) {
      connection.activeOperation = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  @SuppressLint("MissingPermission")
  private fun publishGattDatabase(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    if (currentAdapterState() != "poweredOn") {
      result.error(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before publishing GATT services.",
        mapOf("state" to currentAdapterState()),
      )
      return
    }

    if (publishGattDatabaseResult != null) {
      result.error("busy", "A Bluetooth GATT database publish is already in progress.", null)
      return
    }

    val services = buildGattServerServices(call.arguments)
    if (services == null) {
      result.error(
        "invalid-argument",
        "A valid GATT database payload is required to publish services.",
        null,
      )
      return
    }

    val gattServer = ensureGattServer(context)
    if (gattServer == null) {
      result.error("unavailable", "Bluetooth GATT server is unavailable.", null)
      return
    }

    clearGattDatabase()
    if (services.isEmpty()) {
      result.success(null)
      return
    }

    publishGattDatabaseResult = result
    pendingServicesToPublish.clear()
    pendingServicesToPublish.addAll(services)
    addNextGattServerService(result)
  }

  private fun startAdvertising(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingAdvertisingPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth advertise permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    if (currentAdapterState() != "poweredOn") {
      result.error(
        "adapter-unavailable",
        "Bluetooth adapter must be powered on before advertising.",
        mapOf("state" to currentAdapterState()),
      )
      return
    }

    if (pendingStartAdvertisingResult != null) {
      result.error("busy", "Bluetooth advertising is already starting.", null)
      return
    }

    val advertiser = bluetoothLeAdvertiser()
    if (advertiser == null) {
      result.error("unavailable", "Bluetooth LE advertiser is unavailable.", null)
      return
    }

    val payload = call.arguments as? Map<*, *>
    val advertiseData = buildAdvertiseData(payload)
    val scanResponse = buildAdvertiseScanResponse(payload)

    if (isAdvertising) {
      advertiser.stopAdvertising(advertiseCallback)
      isAdvertising = false
    }

    pendingStartAdvertisingResult = result
    try {
      advertiser.startAdvertising(
        AdvertiseSettings.Builder()
          .setAdvertiseMode(advertiseModeFromPayload(payload))
          .setConnectable(payload?.get("connectable") as? Boolean ?: true)
          .setTxPowerLevel(advertiseTxPowerLevelFromPayload(payload))
          .setTimeout((payload?.get("timeoutMs") as? Number)?.toInt()?.coerceIn(0, 180_000) ?: 0)
          .build(),
        advertiseData,
        scanResponse,
        advertiseCallback,
      )
    } catch (error: SecurityException) {
      pendingStartAdvertisingResult = null
      result.error(
        "permission-denied",
        error.message ?: "Bluetooth advertise permission is missing on Android.",
        missingPermissions,
      )
    }
  }

  private fun notifyCharacteristicValue(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val payload = call.arguments as? Map<*, *>
    val serviceUuid = (payload?.get("serviceUuid") as? String)?.let(::safeNormalizeUuidString)
    val characteristicUuid =
      (payload?.get("characteristicUuid") as? String)?.let(::safeNormalizeUuidString)
    if (serviceUuid == null || characteristicUuid == null) {
      result.error(
        "invalid-argument",
        "`serviceUuid` and `characteristicUuid` are required to notify subscribers.",
        null,
      )
      return
    }

    val characteristicKey = characteristicKey(serviceUuid, characteristicUuid)
    val characteristic = serverCharacteristics[characteristicKey]
    val gattServer = gattServer
    if (characteristic == null || gattServer == null) {
      result.error(
        "unavailable",
        "Bluetooth characteristic was not found. Publish a GATT database first.",
        null,
      )
      return
    }

    val value = payload?.get("value").toByteArray()
    serverCharacteristicValues[characteristicKey] = value

    val targets =
      (payload?.get("deviceId") as? String)?.let { deviceId ->
        val normalizedDeviceId = normalizeDeviceId(deviceId)
        val device = subscribedDevices[characteristicKey]?.get(normalizedDeviceId)
        if (device == null) {
          result.error(
            "unavailable",
            "The target central is not subscribed to this characteristic.",
            null,
          )
          return
        }
        listOf(device)
      } ?: subscribedDevices[characteristicKey]?.values?.toList().orEmpty()

    if (targets.isEmpty()) {
      result.success(null)
      return
    }

    val confirm =
      characteristic.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0 &&
        characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0

    for (device in targets) {
      val notifyStarted = notifyCharacteristicChanged(gattServer, device, characteristic, confirm, value)
      if (!notifyStarted) {
        result.error(
          "notify-failed",
          "Bluetooth notification could not be queued for ${normalizeDeviceId(device.address)}.",
          null,
        )
        return
      }
    }

    result.success(null)
  }

  private fun respondToReadRequest(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val requestId = call.argument<String>("requestId")
    val pendingRequest =
      requestId?.let { pendingServerReadRequests.remove(it) }
        ?: run {
          result.error(
            "unavailable",
            "Bluetooth read request was not found or has already been answered.",
            null,
          )
          return
        }

    val value = call.argument<Any?>("value").toByteArray()
    serverCharacteristicValues[pendingRequest.characteristicKey] = value

    val status =
      if (pendingRequest.offset > value.size) {
        BluetoothGatt.GATT_INVALID_OFFSET
      } else {
        BluetoothGatt.GATT_SUCCESS
      }
    val responseValue =
      value.copyOfRange(pendingRequest.offset.coerceAtMost(value.size), value.size)

    sendGattServerResponse(
      device = pendingRequest.device,
      requestId = pendingRequest.requestId,
      status = status,
      offset = pendingRequest.offset,
      value = responseValue,
    )

    if (status != BluetoothGatt.GATT_SUCCESS) {
      result.error(
        "invalid-argument",
        "The provided value is shorter than the pending read offset.",
        null,
      )
      return
    }

    result.success(null)
  }

  private fun respondToWriteRequest(call: MethodCall, result: MethodChannel.Result) {
    val context = applicationContext
    if (context == null) {
      result.error("unavailable", "Android context is not attached.", null)
      return
    }

    val missingPermissions = missingConnectionPermissions(context)
    if (missingPermissions.isNotEmpty()) {
      result.error(
        "permission-denied",
        "Bluetooth connect permission is missing on Android.",
        missingPermissions,
      )
      return
    }

    val requestId = call.argument<String>("requestId")
    val pendingRequest =
      requestId?.let { pendingServerWriteRequests.remove(it) }
        ?: run {
          result.error(
            "unavailable",
            "Bluetooth write request was not found or has already been answered.",
            null,
          )
          return
        }

    val accept = call.argument<Boolean>("accept") ?: true
    val status =
      if (!accept) {
        BluetoothGatt.GATT_FAILURE
      } else {
        val currentValue = serverCharacteristicValues[pendingRequest.characteristicKey] ?: ByteArray(0)
        val mergedValue = mergeCharacteristicValue(currentValue, pendingRequest.offset, pendingRequest.value)
        if (mergedValue == null) {
          BluetoothGatt.GATT_INVALID_OFFSET
        } else {
          serverCharacteristicValues[pendingRequest.characteristicKey] = mergedValue
          BluetoothGatt.GATT_SUCCESS
        }
      }

    if (pendingRequest.responseNeeded) {
      sendGattServerResponse(
        device = pendingRequest.device,
        requestId = pendingRequest.requestId,
        status = status,
        offset = pendingRequest.offset,
        value = null,
      )
    }

    if (status == BluetoothGatt.GATT_INVALID_OFFSET) {
      result.error(
        "invalid-argument",
        "The pending write request used an unsupported offset.",
        null,
      )
      return
    }

    result.success(null)
  }

  @SuppressLint("MissingPermission")
  private fun ensureGattServer(context: Context): BluetoothGattServer? {
    gattServer?.let { return it }
    return try {
      bluetoothManager?.openGattServer(context, gattServerCallback)?.also {
        gattServer = it
      }
    } catch (_: SecurityException) {
      null
    }
  }

  @SuppressLint("MissingPermission")
  private fun clearGattDatabase() {
    pendingServicesToPublish.clear()
    publishGattDatabaseResult = null
    pendingServerReadRequests.clear()
    pendingServerWriteRequests.clear()
    subscribedDevices.clear()
    serverCharacteristics.clear()
    serverCharacteristicValues.clear()

    try {
      gattServer?.clearServices()
    } catch (_: SecurityException) {
    }
  }

  @SuppressLint("MissingPermission")
  private fun closeGattServer() {
    try {
      gattServer?.close()
    } catch (_: SecurityException) {
    }
    gattServer = null
  }

  @SuppressLint("MissingPermission")
  private fun addNextGattServerService(result: MethodChannel.Result) {
    val gattServer = gattServer
    if (gattServer == null) {
      publishGattDatabaseResult = null
      pendingServicesToPublish.clear()
      result.error("unavailable", "Bluetooth GATT server is unavailable.", null)
      return
    }

    val nextService = pendingServicesToPublish.removeFirstOrNull()
    if (nextService == null) {
      publishGattDatabaseResult = null
      replySuccess(result, null)
      return
    }

    val added =
      try {
        gattServer.addService(nextService)
      } catch (_: SecurityException) {
        false
      }

    if (!added) {
      publishGattDatabaseResult = null
      pendingServicesToPublish.clear()
      replyError(
        result,
        "publish-gatt-database-failed",
        "Bluetooth GATT service could not be added.",
        null,
      )
    }
  }

  private fun buildGattServerServices(arguments: Any?): List<BluetoothGattService>? {
    val payload = arguments as? Map<*, *> ?: return null
    val services = payload["services"] as? List<*> ?: return emptyList()

    val builtServices = mutableListOf<BluetoothGattService>()
    val nextCharacteristics = mutableMapOf<String, BluetoothGattCharacteristic>()
    val nextValues = mutableMapOf<String, ByteArray>()

    for (serviceEntry in services) {
      val serviceMap = serviceEntry as? Map<*, *> ?: return null
      val serviceUuid = (serviceMap["uuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
      val service =
        BluetoothGattService(
          UUID.fromString(serviceUuid),
          if (serviceMap["primary"] as? Boolean == false) {
            BluetoothGattService.SERVICE_TYPE_SECONDARY
          } else {
            BluetoothGattService.SERVICE_TYPE_PRIMARY
          },
        )

      val characteristics = serviceMap["characteristics"] as? List<*> ?: emptyList<Any>()
      for (characteristicEntry in characteristics) {
        val characteristicMap = characteristicEntry as? Map<*, *> ?: return null
        val characteristicUuid =
          (characteristicMap["uuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
        val characteristic =
          BluetoothGattCharacteristic(
            UUID.fromString(characteristicUuid),
            gattCharacteristicProperties(characteristicMap["properties"] as? List<*>),
            gattAttributePermissions(characteristicMap["permissions"] as? List<*>),
          )

        val initialValue = characteristicMap["initialValue"].toByteArray()
        if (initialValue.isNotEmpty()) {
          @Suppress("DEPRECATION")
          run {
            characteristic.value = initialValue
          }
        }

        val descriptors = characteristicMap["descriptors"] as? List<*> ?: emptyList<Any>()
        var hasCccd = false
        for (descriptorEntry in descriptors) {
          val descriptorMap = descriptorEntry as? Map<*, *> ?: return null
          val descriptorUuid =
            (descriptorMap["uuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
          if (descriptorUuid == CLIENT_CHARACTERISTIC_CONFIGURATION_UUID.toString()) {
            hasCccd = true
          }

          val descriptor =
            BluetoothGattDescriptor(
              UUID.fromString(descriptorUuid),
              gattAttributePermissions(descriptorMap["permissions"] as? List<*>),
            )
          @Suppress("DEPRECATION")
          run {
            descriptor.value = descriptorMap["initialValue"].toByteArray()
          }
          characteristic.addDescriptor(descriptor)
        }

        if (!hasCccd &&
          (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 ||
            characteristic.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0)
        ) {
          characteristic.addDescriptor(
            BluetoothGattDescriptor(
              CLIENT_CHARACTERISTIC_CONFIGURATION_UUID,
              BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE,
            ),
          )
        }

        service.addCharacteristic(characteristic)
        val key = characteristicKey(serviceUuid, characteristicUuid)
        nextCharacteristics[key] = characteristic
        nextValues[key] = initialValue
      }

      builtServices += service
    }

    serverCharacteristics.clear()
    serverCharacteristics.putAll(nextCharacteristics)
    serverCharacteristicValues.clear()
    serverCharacteristicValues.putAll(nextValues)
    return builtServices
  }

  private fun gattCharacteristicProperties(properties: List<*>?): Int {
    var value = 0
    val entries = properties?.mapNotNull { it as? String }.orEmpty()
    if ("read" in entries) {
      value = value or BluetoothGattCharacteristic.PROPERTY_READ
    }
    if ("write" in entries) {
      value = value or BluetoothGattCharacteristic.PROPERTY_WRITE
    }
    if ("writeWithoutResponse" in entries) {
      value = value or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE
    }
    if ("notify" in entries) {
      value = value or BluetoothGattCharacteristic.PROPERTY_NOTIFY
    }
    if ("indicate" in entries) {
      value = value or BluetoothGattCharacteristic.PROPERTY_INDICATE
    }
    return value
  }

  private fun gattAttributePermissions(permissions: List<*>?): Int {
    var value = 0
    val entries = permissions?.mapNotNull { it as? String }.orEmpty()
    if ("read" in entries) {
      value = value or BluetoothGattCharacteristic.PERMISSION_READ
    }
    if ("write" in entries) {
      value = value or BluetoothGattCharacteristic.PERMISSION_WRITE
    }
    if ("readEncrypted" in entries) {
      value = value or BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED
    }
    if ("writeEncrypted" in entries) {
      value = value or BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED
    }
    return value
  }

  private fun buildAdvertiseData(payload: Map<*, *>?): AdvertiseData {
    val builder = AdvertiseData.Builder()
      .setIncludeTxPowerLevel(payload?.get("includeTxPowerLevel") as? Boolean ?: false)

    val serviceUuids =
      (payload?.get("serviceUuids") as? List<*>)?.mapNotNull { uuid ->
        (uuid as? String)?.let(::safeNormalizeUuidString)?.let(UUID::fromString)
      }.orEmpty()
    serviceUuids.forEach { builder.addServiceUuid(ParcelUuid(it)) }

    val manufacturerData = payload?.get("manufacturerData").toByteArray()
    if (manufacturerData.size >= 2) {
      val manufacturerId = (manufacturerData[0].toInt() and 0xFF) or
        ((manufacturerData[1].toInt() and 0xFF) shl 8)
      builder.addManufacturerData(manufacturerId, manufacturerData.copyOfRange(2, manufacturerData.size))
    }

    val serviceData = payload?.get("serviceData") as? Map<*, *> ?: emptyMap<Any?, Any?>()
    serviceData.forEach { (uuid, value) ->
      val serviceUuid = (uuid as? String)?.let(::safeNormalizeUuidString) ?: return@forEach
      builder.addServiceData(ParcelUuid(UUID.fromString(serviceUuid)), value.toByteArray())
    }

    val localName = payload?.get("localName") as? String
    if (!localName.isNullOrBlank() && normalizeDeviceName(localName) == normalizeDeviceName(bluetoothAdapter?.name)) {
      builder.setIncludeDeviceName(true)
    }

    return builder.build()
  }

  private fun buildAdvertiseScanResponse(payload: Map<*, *>?): AdvertiseData {
    val localName = payload?.get("localName") as? String
    val builder = AdvertiseData.Builder()
    if (!localName.isNullOrBlank() && normalizeDeviceName(localName) != normalizeDeviceName(bluetoothAdapter?.name)) {
      // Android's legacy advertiser only exposes the adapter's current device name.
      builder.setIncludeDeviceName(false)
    }
    return builder.build()
  }

  private fun advertiseModeFromPayload(payload: Map<*, *>?): Int {
    return when (payload?.get("androidMode") as? String) {
      "lowPower" -> AdvertiseSettings.ADVERTISE_MODE_LOW_POWER
      "balanced" -> AdvertiseSettings.ADVERTISE_MODE_BALANCED
      else -> AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY
    }
  }

  private fun advertiseTxPowerLevelFromPayload(payload: Map<*, *>?): Int {
    return when (payload?.get("androidTxPowerLevel") as? String) {
      "ultraLow" -> AdvertiseSettings.ADVERTISE_TX_POWER_ULTRA_LOW
      "low" -> AdvertiseSettings.ADVERTISE_TX_POWER_LOW
      "medium" -> AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM
      else -> AdvertiseSettings.ADVERTISE_TX_POWER_HIGH
    }
  }

  @SuppressLint("MissingPermission")
  private fun bluetoothLeAdvertiser(): BluetoothLeAdvertiser? {
    return try {
      bluetoothAdapter?.bluetoothLeAdvertiser
    } catch (_: SecurityException) {
      null
    }
  }

  @SuppressLint("MissingPermission")
  private fun stopAdvertising() {
    pendingStartAdvertisingResult = null
    try {
      bluetoothLeAdvertiser()?.stopAdvertising(advertiseCallback)
    } catch (_: SecurityException) {
    }
    isAdvertising = false
  }

  private fun notifyCharacteristicChanged(
    gattServer: BluetoothGattServer,
    device: BluetoothDevice,
    characteristic: BluetoothGattCharacteristic,
    confirm: Boolean,
    value: ByteArray,
  ): Boolean {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      gattServer.notifyCharacteristicChanged(device, characteristic, confirm, value) ==
        BluetoothStatusCodes.SUCCESS
    } else {
      @Suppress("DEPRECATION")
      run {
        characteristic.value = value
      }
      gattServer.notifyCharacteristicChanged(device, characteristic, confirm)
    }
  }

  private fun updateDeviceSubscription(
    characteristic: BluetoothGattCharacteristic,
    device: BluetoothDevice,
    value: ByteArray,
  ) {
    val characteristicKey =
      characteristicKey(
        normalizeUuidString(characteristic.service.uuid.toString()),
        normalizeUuidString(characteristic.uuid.toString()),
      )
    val normalizedDeviceId = normalizeDeviceId(device.address)
    val subscriptionMap = subscribedDevices.getOrPut(characteristicKey) { mutableMapOf() }
    val wasSubscribed = subscriptionMap.containsKey(normalizedDeviceId)

    when {
      value.contentEquals(BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE) -> {
        subscriptionMap.remove(normalizedDeviceId)
      }
      value.contentEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ||
        value.contentEquals(BluetoothGattDescriptor.ENABLE_INDICATION_VALUE) -> {
        subscriptionMap[normalizedDeviceId] = device
      }
    }

    if (subscriptionMap.isEmpty()) {
      subscribedDevices.remove(characteristicKey)
    }

    val isSubscribed = subscriptionMap.containsKey(normalizedDeviceId)
    if (wasSubscribed != isSubscribed) {
      emitEvent(
        mapOf(
          "type" to "subscriptionChanged",
          "deviceId" to normalizedDeviceId,
          "serviceUuid" to normalizeUuidString(characteristic.service.uuid.toString()),
          "characteristicUuid" to normalizeUuidString(characteristic.uuid.toString()),
          "subscribed" to isSubscribed,
        ),
      )
    }
  }

  private fun cccdValueForDevice(
    characteristic: BluetoothGattCharacteristic,
    device: BluetoothDevice,
  ): ByteArray {
    val characteristicKey =
      characteristicKey(
        normalizeUuidString(characteristic.service.uuid.toString()),
        normalizeUuidString(characteristic.uuid.toString()),
      )
    val subscribed = subscribedDevices[characteristicKey]?.containsKey(normalizeDeviceId(device.address)) == true
    if (!subscribed) {
      return BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
    }

    return if (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0 &&
      characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0
    ) {
      BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
    } else {
      BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
    }
  }

  private fun removeDeviceFromSubscriptions(device: BluetoothDevice) {
    val deviceId = normalizeDeviceId(device.address)
    val emptyKeys = mutableListOf<String>()
    subscribedDevices.forEach { (characteristicKey, devices) ->
      devices.remove(deviceId)
      if (devices.isEmpty()) {
        emptyKeys += characteristicKey
      }
    }
    emptyKeys.forEach(subscribedDevices::remove)
  }

  @SuppressLint("MissingPermission")
  private fun sendGattServerResponse(
    device: BluetoothDevice,
    requestId: Int,
    status: Int,
    offset: Int,
    value: ByteArray?,
  ) {
    try {
      gattServer?.sendResponse(device, requestId, status, offset, value)
    } catch (_: SecurityException) {
    }
  }

  private fun mergeCharacteristicValue(
    currentValue: ByteArray,
    offset: Int,
    incomingValue: ByteArray,
  ): ByteArray? {
    if (offset < 0 || offset > currentValue.size) {
      return null
    }

    if (offset == currentValue.size) {
      return currentValue + incomingValue
    }

    val nextSize = maxOf(currentValue.size, offset + incomingValue.size)
    val nextValue = currentValue.copyOf(nextSize)
    incomingValue.forEachIndexed { index, byte ->
      nextValue[offset + index] = byte
    }
    return nextValue
  }

  private fun buildScanFilters(serviceUuids: List<String>): List<ScanFilter> {
    return serviceUuids.map { uuid ->
      ScanFilter.Builder()
        .setServiceUuid(ParcelUuid.fromString(normalizeUuidString(uuid)))
        .build()
    }
  }

  private fun buildScanSettings(): ScanSettings {
    return ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
  }

  @SuppressLint("MissingPermission")
  private fun stopActiveScan() {
    if (!isScanning) {
      return
    }

    try {
      bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
    } catch (_: SecurityException) {
    }

    isScanning = false
    seenDeviceIds.clear()
  }

  private fun registerStateReceiver() {
    val context = applicationContext ?: return
    if (stateReceiverRegistered) {
      return
    }

    val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      context.registerReceiver(stateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
    } else {
      @Suppress("DEPRECATION")
      context.registerReceiver(stateReceiver, filter)
    }
    stateReceiverRegistered = true
  }

  private fun unregisterStateReceiver() {
    val context = applicationContext ?: return
    if (!stateReceiverRegistered) {
      return
    }

    try {
      context.unregisterReceiver(stateReceiver)
    } catch (_: IllegalArgumentException) {
    }
    stateReceiverRegistered = false
  }

  private fun emitAdapterState(state: Int? = null) {
    emitEvent(
      mapOf(
        "type" to "adapterStateChanged",
        "state" to (state?.let(::adapterStateValue) ?: currentAdapterState()),
      ),
    )
  }

  private fun emitConnectionState(deviceId: String, state: String) {
    emitEvent(
      mapOf(
        "type" to "connectionStateChanged",
        "deviceId" to deviceId,
        "state" to state,
      ),
    )
  }

  private fun emitMtuChanged(deviceId: String, mtu: Int, status: Int) {
    emitEvent(
      mapOf(
        "type" to "mtuChanged",
        "deviceId" to deviceId,
        "mtu" to mtu,
        "status" to status,
      ),
    )
  }

  private fun emitPhyUpdated(deviceId: String, txPhy: Int, rxPhy: Int, status: Int) {
    emitEvent(
      mapOf(
        "type" to "phyUpdated",
        "deviceId" to deviceId,
        "txPhy" to phyValue(txPhy),
        "rxPhy" to phyValue(rxPhy),
        "status" to status,
      ),
    )
  }

  @SuppressLint("MissingPermission")
  private fun currentAdapterState(): String {
    val adapter = bluetoothAdapter ?: return "unavailable"
    return try {
      adapterStateValue(adapter.state)
    } catch (_: SecurityException) {
      "unauthorized"
    }
  }

  private fun adapterStateValue(state: Int): String {
    return when (state) {
      BluetoothAdapter.STATE_ON -> "poweredOn"
      BluetoothAdapter.STATE_OFF -> "poweredOff"
      BluetoothAdapter.STATE_TURNING_ON,
      BluetoothAdapter.STATE_TURNING_OFF,
      BluetoothAdapter.ERROR,
      -> "unknown"
      else -> "unknown"
    }
  }

  private fun connectionPriorityFromValue(value: String?): Int? {
    return when (value) {
      "balanced",
      null,
      -> BluetoothGatt.CONNECTION_PRIORITY_BALANCED
      "high" -> BluetoothGatt.CONNECTION_PRIORITY_HIGH
      "lowPower" -> BluetoothGatt.CONNECTION_PRIORITY_LOW_POWER
      else -> null
    }
  }

  private fun preferredPhyFromValue(value: String?): Int? {
    return when (value) {
      "le1m",
      null,
      -> BluetoothDevice.PHY_LE_1M_MASK
      "le2m" -> BluetoothDevice.PHY_LE_2M_MASK
      "leCoded" -> BluetoothDevice.PHY_LE_CODED_MASK
      else -> null
    }
  }

  private fun phyOptionsFromValue(value: String?): Int? {
    return when (value) {
      "unspecified",
      null,
      -> BluetoothDevice.PHY_OPTION_NO_PREFERRED
      "s2" -> BluetoothDevice.PHY_OPTION_S2
      "s8" -> BluetoothDevice.PHY_OPTION_S8
      else -> null
    }
  }

  private fun phyValue(phy: Int): String {
    return when (phy) {
      BluetoothDevice.PHY_LE_2M,
      BluetoothDevice.PHY_LE_2M_MASK,
      -> "le2m"
      BluetoothDevice.PHY_LE_CODED,
      BluetoothDevice.PHY_LE_CODED_MASK,
      -> "leCoded"
      else -> "le1m"
    }
  }

  @SuppressLint("MissingPermission")
  private fun emitScanResult(result: ScanResult) {
    val deviceId =
      try {
        normalizeDeviceId(result.device.address)
      } catch (_: SecurityException) {
        null
      }

    if (deviceId.isNullOrBlank()) {
      return
    }

    if (!allowDuplicates && !seenDeviceIds.add(deviceId)) {
      return
    }

    emitEvent(
      mapOf(
        "type" to "scanResult",
        "result" to scanResultPayload(result, deviceId),
      ),
    )
  }

  @SuppressLint("MissingPermission")
  private fun scanResultPayload(result: ScanResult, deviceId: String): Map<String, Any?> {
    val scanRecord = result.scanRecord
    val serviceUuids =
      scanRecord?.serviceUuids?.map { normalizeUuidString(it.toString()) }.orEmpty()
    val serviceData = mutableMapOf<String, List<Int>>()
    scanRecord?.serviceData?.forEach { uuid, value ->
      serviceData[normalizeUuidString(uuid.toString())] = value?.asUnsignedList().orEmpty()
    }

    val manufacturerData =
      scanRecord
        ?.manufacturerSpecificData
        ?.takeIf { it.size() > 0 }
        ?.let { entries ->
          val manufacturerId = entries.keyAt(0)
          val data = entries.valueAt(0)
          buildList {
            add(manufacturerId and 0xFF)
            add((manufacturerId shr 8) and 0xFF)
            addAll(data?.asUnsignedList().orEmpty())
          }
        }

    val name =
      scanRecord?.deviceName ?: try {
        result.device.name
      } catch (_: SecurityException) {
        null
      }

    return mapOf(
      "deviceId" to deviceId,
      "name" to name,
      "rssi" to result.rssi,
      "serviceUuids" to serviceUuids,
      "serviceData" to serviceData,
      "manufacturerData" to manufacturerData,
      "connectable" to
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
          result.isConnectable
        } else {
          true
        },
    )
  }

  private fun parseDeviceId(arguments: Any?): String? {
    return ((arguments as? Map<*, *>)?.get("deviceId") as? String)?.let(::normalizeDeviceId)
  }

  private fun parseCharacteristicAddress(arguments: Any?): ParsedCharacteristicAddress? {
    val payload = arguments as? Map<*, *> ?: return null
    val deviceId = (payload["deviceId"] as? String)?.let(::normalizeDeviceId) ?: return null
    val serviceUuid = (payload["serviceUuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
    val characteristicUuid =
      (payload["characteristicUuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null

    return ParsedCharacteristicAddress(
      deviceId = deviceId,
      serviceUuid = serviceUuid,
      characteristicUuid = characteristicUuid,
    )
  }

  private fun parseDescriptorAddress(arguments: Any?): ParsedDescriptorAddress? {
    val payload = arguments as? Map<*, *> ?: return null
    val deviceId = (payload["deviceId"] as? String)?.let(::normalizeDeviceId) ?: return null
    val serviceUuid = (payload["serviceUuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
    val characteristicUuid =
      (payload["characteristicUuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null
    val descriptorUuid =
      (payload["descriptorUuid"] as? String)?.let(::safeNormalizeUuidString) ?: return null

    return ParsedDescriptorAddress(
      deviceId = deviceId,
      serviceUuid = serviceUuid,
      characteristicUuid = characteristicUuid,
      descriptorUuid = descriptorUuid,
    )
  }

  private fun connectedConnection(deviceId: String): ConnectionContext? {
    val connection = connections[deviceId] ?: return null
    return if (connection.state == "connected" && connection.gatt != null) connection else null
  }

  private fun rebuildCharacteristicCache(connection: ConnectionContext, gatt: BluetoothGatt) {
    connection.characteristics.clear()
    gatt.services.forEach { service ->
      val serviceUuid = normalizeUuidString(service.uuid.toString())
      service.characteristics.forEach { characteristic ->
        connection.characteristics[
          characteristicKey(serviceUuid, normalizeUuidString(characteristic.uuid.toString()))
        ] = characteristic
      }
    }
  }

  private fun findCharacteristic(
    connection: ConnectionContext,
    address: ParsedCharacteristicAddress,
  ): BluetoothGattCharacteristic? {
    connection.characteristics[address.characteristicKey]?.let { return it }

    val gatt = connection.gatt ?: return null
    for (service in gatt.services) {
      val serviceUuid = normalizeUuidString(service.uuid.toString())
      if (serviceUuid != address.serviceUuid) {
        continue
      }

      for (characteristic in service.characteristics) {
        val characteristicUuid = normalizeUuidString(characteristic.uuid.toString())
        if (characteristicUuid != address.characteristicUuid) {
          continue
        }

        connection.characteristics[address.characteristicKey] = characteristic
        return characteristic
      }
    }

    return null
  }

  private fun findDescriptor(
    connection: ConnectionContext,
    address: ParsedDescriptorAddress,
  ): BluetoothGattDescriptor? {
    val characteristic =
      findCharacteristic(
        connection,
        ParsedCharacteristicAddress(
          deviceId = address.deviceId,
          serviceUuid = address.serviceUuid,
          characteristicUuid = address.characteristicUuid,
        ),
      ) ?: return null

    return characteristic.descriptors.firstOrNull { descriptor ->
      normalizeUuidString(descriptor.uuid.toString()) == address.descriptorUuid
    }
  }

  @Suppress("DEPRECATION")
  private fun handleCharacteristicRead(
    gatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray?,
    status: Int,
  ) {
    val deviceId = normalizeDeviceId(gatt.device.address)
    val connection = connections[deviceId] ?: return
    val operation = connection.activeOperation
    val characteristicKey =
      characteristicKey(
        normalizeUuidString(characteristic.service.uuid.toString()),
        normalizeUuidString(characteristic.uuid.toString()),
      )

    if (operation?.type != OP_READ_CHARACTERISTIC || operation.key != characteristicKey) {
      return
    }

    connection.activeOperation = null
    if (status != BluetoothGatt.GATT_SUCCESS) {
      replyError(
        operation.result,
        "read-failed",
        "Bluetooth characteristic read failed with status $status.",
        mapOf("status" to status),
      )
      return
    }

    replySuccess(operation.result, value ?: ByteArray(0))
  }

  @Suppress("DEPRECATION")
  private fun handleDescriptorRead(
    gatt: BluetoothGatt,
    descriptor: BluetoothGattDescriptor,
    value: ByteArray?,
    status: Int,
  ) {
    val deviceId = normalizeDeviceId(gatt.device.address)
    val connection = connections[deviceId] ?: return
    val operation = connection.activeOperation
    val descriptorKey =
      descriptorKey(
        normalizeUuidString(descriptor.characteristic.service.uuid.toString()),
        normalizeUuidString(descriptor.characteristic.uuid.toString()),
        normalizeUuidString(descriptor.uuid.toString()),
      )

    if (operation?.type != OP_READ_DESCRIPTOR || operation.key != descriptorKey) {
      return
    }

    connection.activeOperation = null
    if (status != BluetoothGatt.GATT_SUCCESS) {
      replyError(
        operation.result,
        "read-failed",
        "Bluetooth descriptor read failed with status $status.",
        mapOf("status" to status),
      )
      return
    }

    replySuccess(operation.result, value ?: ByteArray(0))
  }

  private fun emitCharacteristicValueChanged(
    gatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
  ) {
    emitEvent(
      mapOf(
        "type" to "characteristicValueChanged",
        "deviceId" to normalizeDeviceId(gatt.device.address),
        "serviceUuid" to normalizeUuidString(characteristic.service.uuid.toString()),
        "characteristicUuid" to normalizeUuidString(characteristic.uuid.toString()),
        "value" to value.asUnsignedList(),
      ),
    )
  }

  private fun servicePayload(service: BluetoothGattService): Map<String, Any?> {
    return mapOf(
      "uuid" to normalizeUuidString(service.uuid.toString()),
      "primary" to (service.type == BluetoothGattService.SERVICE_TYPE_PRIMARY),
      "characteristics" to service.characteristics.map(::characteristicPayload),
    )
  }

  @Suppress("DEPRECATION")
  private fun characteristicPayload(
    characteristic: BluetoothGattCharacteristic,
  ): Map<String, Any?> {
    return mapOf(
      "uuid" to normalizeUuidString(characteristic.uuid.toString()),
      "properties" to characteristicProperties(characteristic.properties),
      "permissions" to characteristicPermissions(characteristic.permissions),
      "descriptors" to characteristic.descriptors.map(::descriptorPayload),
      "initialValue" to characteristic.value?.asUnsignedList(),
    )
  }

  @Suppress("DEPRECATION")
  private fun descriptorPayload(descriptor: BluetoothGattDescriptor): Map<String, Any?> {
    return mapOf(
      "uuid" to normalizeUuidString(descriptor.uuid.toString()),
      "permissions" to descriptorPermissions(descriptor.permissions),
      "initialValue" to descriptor.value?.asUnsignedList(),
    )
  }

  private fun descriptorKey(
    serviceUuid: String,
    characteristicUuid: String,
    descriptorUuid: String,
  ): String {
    return "$serviceUuid|$characteristicUuid|$descriptorUuid"
  }

  private fun characteristicProperties(properties: Int): List<String> {
    val values = mutableListOf<String>()
    if (properties and BluetoothGattCharacteristic.PROPERTY_READ != 0) {
      values += "read"
    }
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) {
      values += "write"
    }
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) {
      values += "writeWithoutResponse"
    }
    if (properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) {
      values += "notify"
    }
    if (properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) {
      values += "indicate"
    }
    return values
  }

  private fun characteristicPermissions(permissions: Int): List<String> {
    val values = mutableListOf<String>()
    if (permissions and BluetoothGattCharacteristic.PERMISSION_READ != 0) {
      values += "read"
    }
    if (permissions and BluetoothGattCharacteristic.PERMISSION_WRITE != 0) {
      values += "write"
    }
    if (permissions and BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED != 0) {
      values += "readEncrypted"
    }
    if (permissions and BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED != 0) {
      values += "writeEncrypted"
    }
    return values
  }

  private fun descriptorPermissions(permissions: Int): List<String> {
    val values = mutableListOf<String>()
    if (permissions and BluetoothGattDescriptor.PERMISSION_READ != 0) {
      values += "read"
    }
    if (permissions and BluetoothGattDescriptor.PERMISSION_WRITE != 0) {
      values += "write"
    }
    if (permissions and BluetoothGattDescriptor.PERMISSION_READ_ENCRYPTED != 0) {
      values += "readEncrypted"
    }
    if (permissions and BluetoothGattDescriptor.PERMISSION_WRITE_ENCRYPTED != 0) {
      values += "writeEncrypted"
    }
    return values
  }

  private fun characteristicKey(serviceUuid: String, characteristicUuid: String): String {
    return "$serviceUuid|$characteristicUuid"
  }

  private fun handleDisconnected(connection: ConnectionContext, status: Int, message: String) {
    cancelConnectTimeout(connection)

    connection.connectResult?.let { pendingResult ->
      connection.connectResult = null
      replyError(
        pendingResult,
        "connection-failed",
        message,
        mapOf("status" to status),
      )
    }

    connection.activeOperation?.let { operation ->
      connection.activeOperation = null
      replyError(
        operation.result,
        "device-disconnected",
        "Bluetooth device disconnected during `${operation.type}`.",
        mapOf("status" to status),
      )
    }

    connection.disconnectResult?.let { disconnectResult ->
      connection.disconnectResult = null
      replySuccess(disconnectResult, null)
    }

    emitConnectionState(connection.deviceId, "disconnected")
    cleanupConnection(connection.deviceId)
  }

  private fun scheduleConnectTimeout(connection: ConnectionContext, timeoutMs: Long) {
    cancelConnectTimeout(connection)

    val timeoutRunnable =
      Runnable {
        val currentConnection = connections[connection.deviceId] ?: return@Runnable
        val pendingResult = currentConnection.connectResult ?: return@Runnable
        currentConnection.connectResult = null
        replyError(
          pendingResult,
          "connection-timeout",
          "Bluetooth connection timed out.",
          null,
        )
        emitConnectionState(connection.deviceId, "disconnected")
        cleanupConnection(connection.deviceId)
      }

    connection.connectTimeoutRunnable = timeoutRunnable
    mainHandler.postDelayed(timeoutRunnable, timeoutMs)
  }

  private fun cancelConnectTimeout(connection: ConnectionContext) {
    connection.connectTimeoutRunnable?.let(mainHandler::removeCallbacks)
    connection.connectTimeoutRunnable = null
  }

  @SuppressLint("MissingPermission")
  private fun cleanupConnection(deviceId: String) {
    val connection = connections.remove(deviceId) ?: return
    cancelConnectTimeout(connection)
    connection.activeOperation = null
    connection.characteristics.clear()
    try {
      connection.gatt?.close()
    } catch (_: SecurityException) {
    }
    connection.gatt = null
    connection.state = "disconnected"
  }

  private fun closeAllConnections() {
    connections.keys.toList().forEach(::cleanupConnection)
  }

  private fun replySuccess(result: MethodChannel.Result, value: Any?) {
    mainHandler.post {
      result.success(value)
    }
  }

  private fun replyError(
    result: MethodChannel.Result,
    code: String,
    message: String,
    details: Any?,
  ) {
    mainHandler.post {
      result.error(code, message, details)
    }
  }

  private fun emitEvent(event: Map<String, Any?>) {
    mainHandler.post {
      eventSink?.success(event)
    }
  }

  private fun detachFromActivityBinding() {
    activityBinding?.removeRequestPermissionsResultListener(this)
    activityBinding = null
    activity = null
  }

  private fun clearPendingPermissionRequest(code: String, message: String) {
    pendingPermissionRequest?.let { request ->
      pendingPermissionRequest = null
      replyError(request.result, code, message, null)
    }
  }

  private fun parseRequestedPermissions(arguments: Any?): List<String>? {
    val rawPermissions =
      ((arguments as? Map<*, *>)?.get("permissions") as? List<*>) ?: return emptyList()
    val permissions = mutableListOf<String>()

    rawPermissions.forEach { entry ->
      val permission = (entry as? String)?.trim()?.lowercase(Locale.US) ?: return null
      if (requiredAndroidPermissions(permission) == null) {
        return null
      }
      if (permission !in permissions) {
        permissions += permission
      }
    }

    return permissions
  }

  private fun permissionStatusPayload(permissions: List<String>): Map<String, Any?> {
    val permissionStates =
      linkedMapOf<String, String>().apply {
        permissions.forEach { permission ->
          this[permission] = permissionState(permission)
        }
      }

    return mapOf(
      "permissions" to permissionStates,
      "allGranted" to permissionStates.values.all { it == "granted" || it == "notRequired" },
    )
  }

  private fun permissionRationalePayload(permissions: List<String>): Map<String, Any?> {
    val rationale =
      linkedMapOf<String, Boolean>().apply {
        permissions.forEach { permission ->
          this[permission] = shouldShowPermissionRationale(permission)
        }
      }

    return mapOf("permissions" to rationale)
  }

  private fun permissionState(permission: String): String {
    val context = applicationContext ?: return "denied"
    val androidPermissions = requiredAndroidPermissions(permission) ?: return "denied"
    if (androidPermissions.isEmpty()) {
      return "notRequired"
    }

    val allGranted =
      androidPermissions.all { androidPermission ->
        ContextCompat.checkSelfPermission(context, androidPermission) == PackageManager.PERMISSION_GRANTED
    }
    return if (allGranted) "granted" else "denied"
  }

  private fun shouldShowPermissionRationale(permission: String): Boolean {
    val context = applicationContext ?: return false
    val attachedActivity = activity ?: return false
    val androidPermissions = requiredAndroidPermissions(permission) ?: return false
    if (androidPermissions.isEmpty()) {
      return false
    }

    return androidPermissions.any { androidPermission ->
      ContextCompat.checkSelfPermission(context, androidPermission) != PackageManager.PERMISSION_GRANTED &&
        ActivityCompat.shouldShowRequestPermissionRationale(attachedActivity, androidPermission)
    }
  }

  private fun launchSettingsIntent(context: Context, intent: Intent): Boolean {
    return try {
      if (intent.resolveActivity(context.packageManager) == null) {
        false
      } else {
        context.startActivity(intent)
        true
      }
    } catch (_: RuntimeException) {
      false
    }
  }

  private fun requiredAndroidPermissions(permission: String): List<String>? {
    return when (permission) {
      "scan" ->
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
          listOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
          )
        } else {
          listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
      "connect" ->
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
          listOf(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
          emptyList()
        }
      "advertise" ->
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
          listOf(Manifest.permission.BLUETOOTH_ADVERTISE)
        } else {
          emptyList()
        }
      else -> null
    }
  }

  private fun missingScanPermissions(context: Context): List<String> {
    val requiredPermissions = requiredAndroidPermissions("scan").orEmpty()

    return requiredPermissions.filter { permission ->
      ContextCompat.checkSelfPermission(context, permission) != PackageManager.PERMISSION_GRANTED
    }
  }

  private fun missingConnectionPermissions(context: Context): List<String> {
    val requiredPermissions = requiredAndroidPermissions("connect").orEmpty()

    return requiredPermissions.filter { permission ->
      ContextCompat.checkSelfPermission(context, permission) != PackageManager.PERMISSION_GRANTED
    }
  }

  private fun missingAdvertisingPermissions(context: Context): List<String> {
    val requiredPermissions = requiredAndroidPermissions("advertise").orEmpty()

    return requiredPermissions.filter { permission ->
      ContextCompat.checkSelfPermission(context, permission) != PackageManager.PERMISSION_GRANTED
    }
  }

  private fun normalizeDeviceId(value: String): String {
    return value.trim().uppercase(Locale.US)
  }

  private fun normalizeDeviceName(value: String?): String? {
    return value?.trim()?.lowercase(Locale.US)
  }

  private fun safeNormalizeUuidString(value: String): String? {
    return runCatching { normalizeUuidString(value) }.getOrNull()
  }

  private fun normalizeUuidString(value: String): String {
    val trimmed = value.trim().lowercase(Locale.US)
    return when (trimmed.length) {
      4 -> "0000$trimmed-0000-1000-8000-00805f9b34fb"
      8 -> "$trimmed-0000-1000-8000-00805f9b34fb"
      else -> UUID.fromString(trimmed).toString()
    }
  }

  @Suppress("DEPRECATION")
  private fun writeCharacteristic(
    gatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
    writeType: Int,
  ): Boolean {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      gatt.writeCharacteristic(characteristic, value, writeType) == BluetoothStatusCodes.SUCCESS
    } else {
      characteristic.writeType = writeType
      characteristic.value = value
      gatt.writeCharacteristic(characteristic)
    }
  }

  @Suppress("DEPRECATION")
  private fun writeDescriptor(
    gatt: BluetoothGatt,
    descriptor: BluetoothGattDescriptor,
    value: ByteArray,
  ): Boolean {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS
    } else {
      descriptor.value = value
      gatt.writeDescriptor(descriptor)
    }
  }

  private fun Any?.toByteArray(): ByteArray {
    return when (this) {
      is ByteArray -> this
      is List<*> -> ByteArray(size) { index -> (this[index] as Number).toByte() }
      else -> ByteArray(0)
    }
  }

  private fun ByteArray.asUnsignedList(): List<Int> {
    return map { it.toInt() and 0xFF }
  }
}
