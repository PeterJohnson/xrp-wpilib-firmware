#include <Arduino.h>
#include <LittleFS.h>
#include <SingleFileDrive.h>
#include <Wire.h>

#include <string>
#include <string.h>

#include "bluetooth_transport.h"
#include "byteutils.h"
#include "config.h"
#include "encoder.h"
#include "imu.h"
#include "robot.h"
#include "wpilibudp.h"

// Resource strings
extern "C" {
const unsigned char* GetResource_VERSION(size_t* len);
}

// Beta board uses Wire where Production board uses Wire1
#ifdef ARDUINO_SPARKFUN_XRP_CONTROLLER_BETA
  #define MYWIRE Wire
#else
  #define MYWIRE Wire1
#endif

char BLUETOOTH_DEVICE_NAME[32];
char CHIP_ID[20];

char transportPacketBuf[bluetooth_transport::MAX_PACKET_SIZE];

// TEMP: Status
unsigned long _lastMessageStatusPrint = 0;
unsigned long _lastStatusDiskWrite = 0;
uint32_t _statusDiskWriteCount = 0;
volatile bool _statusDriveMounted = false;
constexpr unsigned long STATUS_DISK_UPDATE_INTERVAL_MS = 10000;

unsigned long _avgLoopTimeUs = 0;
unsigned long _maxLoopTimeUs = 0;
unsigned long _loopTimeMeasurementCount = 0;
uint32_t _statusPacketBuildCount = 0;
uint32_t _statusPacketAcceptedCount = 0;
uint32_t _statusPacketRejectedCount = 0;
unsigned long _lastStatusBuildMs = 0;
unsigned long _lastStatusBuildIntervalMs = 0;
unsigned long _maxStatusBuildIntervalMs = 0;
unsigned long _lastStatusAcceptedMs = 0;
unsigned long _lastStatusAcceptedIntervalMs = 0;
unsigned long _maxStatusAcceptedIntervalMs = 0;

uint16_t seq = 0;

void writeHexData(File& f, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (i > 0) {
      f.print(" ");
    }
    f.printf("%02x", data[i]);
  }
}

void writeAsciiData(File& f, const uint8_t* data, size_t length) {
  f.print("\"");
  for (size_t i = 0; i < length; i++) {
    uint8_t c = data[i];
    if (c == '"' || c == '\\') {
      f.print("\\");
      f.print(static_cast<char>(c));
    } else if (c >= 0x20 && c <= 0x7e) {
      f.print(static_cast<char>(c));
    } else {
      f.print(".");
    }
  }
  f.print("\"");
}

void writeUuid128Le(File& f, const uint8_t* uuid) {
  for (int i = 15; i >= 0; i--) {
    f.printf("%02x", uuid[i]);
    if (i == 12 || i == 10 || i == 8 || i == 6) {
      f.print("-");
    }
  }
}

const char* getAdTypeName(uint8_t type) {
  switch (type) {
    case 0x01:
      return "Flags";
    case 0x07:
      return "Complete 128-bit Service UUIDs";
    case 0x08:
      return "Shortened Local Name";
    case 0x09:
      return "Complete Local Name";
    case 0x12:
      return "Slave Connection Interval Range";
    default:
      return "Unknown";
  }
}

void writeAdvertisementFields(File& f, const char* label, const uint8_t* data,
                              uint8_t length) {
  f.printf("%s Fields:\n", label);
  size_t pos = 0;
  int field = 0;
  while (pos < length) {
    uint8_t fieldLength = data[pos++];
    if (fieldLength == 0) {
      f.printf("  [%d] zero length terminator\n", field);
      break;
    }
    if (pos + fieldLength > length || fieldLength < 1) {
      f.printf("  [%d] invalid field length %u at offset %u\n", field,
               fieldLength, static_cast<unsigned>(pos - 1));
      break;
    }

    uint8_t type = data[pos++];
    uint8_t valueLength = fieldLength - 1;
    const uint8_t* value = &data[pos];
    f.printf("  [%d] type 0x%02x (%s), len %u, value ", field, type,
             getAdTypeName(type), valueLength);
    writeHexData(f, value, valueLength);

    if (type == 0x08 || type == 0x09) {
      f.print(", text ");
      writeAsciiData(f, value, valueLength);
    } else if (type == 0x07 && valueLength % 16 == 0) {
      f.print(", uuid");
      if (valueLength > 16) {
        f.print("s");
      }
      f.print(" ");
      for (uint8_t offset = 0; offset < valueLength; offset += 16) {
        if (offset > 0) {
          f.print(", ");
        }
        writeUuid128Le(f, &value[offset]);
      }
    } else if (type == 0x12 && valueLength == 4) {
      uint16_t minInterval =
          static_cast<uint16_t>(value[0]) | static_cast<uint16_t>(value[1] << 8);
      uint16_t maxInterval =
          static_cast<uint16_t>(value[2]) | static_cast<uint16_t>(value[3] << 8);
      f.printf(", interval units %u-%u", minInterval, maxInterval);
    }

    f.print("\n");
    pos += valueLength;
    field++;
  }

  if (field == 0 && length == 0) {
    f.print("  none\n");
  }
}

void statusDrivePlugged(uint32_t data) {
  (void)data;
  _statusDriveMounted = true;
}

void statusDriveUnplugged(uint32_t data) {
  (void)data;
  _statusDriveMounted = false;
}

// Generate the status text file
void writeStatusToDisk(const char* chipID, const char* diagnosticsSnapshot) {
  File f = LittleFS.open("/status.txt", "w");
  if (!f) {
    return;
  }

  _statusDiskWriteCount++;
  size_t len;
  std::string versionString{reinterpret_cast<const char*>(GetResource_VERSION(&len)),
                            len};
  f.printf("Version: %s\n", versionString.c_str());
  f.printf("Chip ID: %s\n", chipID);
  f.printf("Status Update Count: %lu\n",
           static_cast<unsigned long>(_statusDiskWriteCount));
  f.printf("Status Uptime ms: %lu\n", static_cast<unsigned long>(millis()));
  f.printf("USB Status Drive Mounted: %s\n",
           _statusDriveMounted ? "yes" : "no");
  f.printf("Firmware Loop: avg_us=%lu max_us=%lu samples=%lu\n",
           static_cast<unsigned long>(_avgLoopTimeUs),
           static_cast<unsigned long>(_maxLoopTimeUs),
           static_cast<unsigned long>(_loopTimeMeasurementCount));
  f.printf("Status Packet Cadence: built=%lu accepted=%lu rejected=%lu "
           "last_build_gap_ms=%lu max_build_gap_ms=%lu "
           "last_accept_gap_ms=%lu max_accept_gap_ms=%lu\n",
           static_cast<unsigned long>(_statusPacketBuildCount),
           static_cast<unsigned long>(_statusPacketAcceptedCount),
           static_cast<unsigned long>(_statusPacketRejectedCount),
           static_cast<unsigned long>(_lastStatusBuildIntervalMs),
           static_cast<unsigned long>(_maxStatusBuildIntervalMs),
           static_cast<unsigned long>(_lastStatusAcceptedIntervalMs),
           static_cast<unsigned long>(_maxStatusAcceptedIntervalMs));
  f.printf("Config Version: %d\n", XRP_CONFIG_VERSION);
  f.printf("Transport: Bluetooth LE GATT + L2CAP Credit-Based Mode\n");
  f.printf("Bluetooth Name: %s\n", BLUETOOTH_DEVICE_NAME);
  f.printf("GATT Service UUID: %s\n", bluetooth_transport::GATT_SERVICE_UUID);
  f.printf("GATT Control Characteristic UUID: %s\n",
           bluetooth_transport::GATT_CONTROL_CHARACTERISTIC_UUID);
  f.printf("GATT Status Characteristic UUID: %s\n",
           bluetooth_transport::GATT_STATUS_CHARACTERISTIC_UUID);
  f.printf("LE PSM: 0x%04x\n", bluetooth_transport::LE_PSM);
  f.printf("Preferred Connection Interval: 7.5-15 ms, latency 0\n");
  f.printf("Packet Framing: one BLE packet per WPILib XRP payload\n");

  char localAddress[32];
  bluetooth_transport::localAddress(localAddress, sizeof(localAddress));
  const auto& advertisementDiagnostics =
      bluetooth_transport::advertisementDiagnostics();
  const auto& connectionDiagnostics =
      bluetooth_transport::connectionDiagnostics();
  f.print("\nBluetooth Diagnostics:\n");
  f.printf("Bluetooth Diagnostics Snapshot: %s\n", diagnosticsSnapshot);
  f.printf("BTstack HCI State: %s (%u)\n", bluetooth_transport::hciStateName(),
           bluetooth_transport::hciState());
  f.printf("Bluetooth Local Address: %s\n", localAddress);
  f.printf("LE Connected: %s handle=0x%04x\n",
           connectionDiagnostics.leConnected ? "yes" : "no",
           connectionDiagnostics.leConnectionHandle);
  f.printf("LE Connection Parameters: interval_units=%u interval_ms_x100=%u "
           "latency=%u supervision_timeout_units=%u updates=%lu "
           "last_disconnect_reason=0x%02x\n",
           connectionDiagnostics.connectionInterval,
           connectionDiagnostics.connectionInterval * 125,
           connectionDiagnostics.connectionLatency,
           connectionDiagnostics.connectionSupervisionTimeout,
           static_cast<unsigned long>(connectionDiagnostics.connectionUpdates),
           connectionDiagnostics.lastDisconnectReason);
  f.printf("L2CAP Connected: %s cid=0x%04x remote_mtu=%u\n",
           connectionDiagnostics.l2capConnected ? "yes" : "no",
           connectionDiagnostics.l2capChannelId,
           connectionDiagnostics.l2capRemoteMtu);
  f.printf("L2CAP Packet Counters: status_queued=%lu requests=%lu "
           "callbacks=%lu immediate=%lu sent=%lu drops=%lu last=0x%02x\n",
           static_cast<unsigned long>(
               connectionDiagnostics.l2capStatusPacketsQueued),
           static_cast<unsigned long>(connectionDiagnostics.l2capCanSendRequests),
           static_cast<unsigned long>(connectionDiagnostics.l2capCanSendCallbacks),
           static_cast<unsigned long>(connectionDiagnostics.l2capImmediateSends),
           static_cast<unsigned long>(connectionDiagnostics.l2capPacketsSent),
           static_cast<unsigned long>(connectionDiagnostics.l2capSendDrops),
           connectionDiagnostics.lastL2capSendResult);
  f.printf("GATT Connected: %s handle=0x%04x payload_mtu=%u\n",
           connectionDiagnostics.gattConnected ? "yes" : "no",
           connectionDiagnostics.gattConnectionHandle,
           connectionDiagnostics.gattPayloadMtu);
  f.printf("GATT Handles: control=0x%04x status=0x%04x cccd=0x%04x\n",
           connectionDiagnostics.gattControlValueHandle,
           connectionDiagnostics.gattStatusValueHandle,
           connectionDiagnostics.gattStatusCccHandle);
  f.printf("GATT Notifications Enabled: %s\n",
           connectionDiagnostics.gattNotificationsEnabled ? "yes" : "no");
  f.printf("Transport State: active=%u tx=%u tx_pending=%s tx_requested=%s "
           "rx_overflow=%s pending_age_us=%lu last_pending_us=%lu "
           "max_pending_us=%lu\n",
           connectionDiagnostics.activeTransport,
           connectionDiagnostics.txTransport,
           connectionDiagnostics.txPending ? "yes" : "no",
           connectionDiagnostics.txCanSendRequested ? "yes" : "no",
           connectionDiagnostics.rxOverflow ? "yes" : "no",
           static_cast<unsigned long>(connectionDiagnostics.txPendingAgeUs),
           static_cast<unsigned long>(
               connectionDiagnostics.lastTxPendingDurationUs),
           static_cast<unsigned long>(
               connectionDiagnostics.maxTxPendingDurationUs));
  f.printf("Transport Send Counters: attempts=%lu busy_drops=%lu "
           "no_transport_drops=%lu invalid_size_drops=%lu last_size=%u\n",
           static_cast<unsigned long>(connectionDiagnostics.statusSendAttempts),
           static_cast<unsigned long>(
               connectionDiagnostics.statusSendBusyDrops),
           static_cast<unsigned long>(
               connectionDiagnostics.statusSendNoTransportDrops),
           static_cast<unsigned long>(
               connectionDiagnostics.statusSendInvalidSizeDrops),
           connectionDiagnostics.lastStatusPacketSize);
  f.printf("RX Queue: used=%u/%u max_used=%u queued=%lu dropped=%lu\n",
           connectionDiagnostics.rxQueueUsed,
           connectionDiagnostics.rxQueueDepth,
           connectionDiagnostics.rxQueueMaxUsed,
           static_cast<unsigned long>(connectionDiagnostics.rxPacketsQueued),
           static_cast<unsigned long>(connectionDiagnostics.rxPacketsDropped));
  f.printf("GATT Packet Counters: control_rx=%lu cccd_writes=%lu "
           "status_queued=%lu status_blocked_notify=%lu "
           "status_blocked_mtu=%lu notify_requests=%lu "
           "notify_callbacks=%lu notify_immediate=%lu notify_sent=%lu "
           "notify_drops=%lu last_status_size=%u\n",
           static_cast<unsigned long>(
               connectionDiagnostics.gattControlPacketsReceived),
           static_cast<unsigned long>(connectionDiagnostics.gattCccdWrites),
           static_cast<unsigned long>(
               connectionDiagnostics.gattStatusPacketsQueued),
           static_cast<unsigned long>(
               connectionDiagnostics.gattStatusPacketsBlockedNotifications),
           static_cast<unsigned long>(
               connectionDiagnostics.gattStatusPacketsBlockedMtu),
           static_cast<unsigned long>(
               connectionDiagnostics.gattNotificationRequests),
           static_cast<unsigned long>(
               connectionDiagnostics.gattNotificationCallbacks),
           static_cast<unsigned long>(
               connectionDiagnostics.gattNotificationImmediateSends),
           static_cast<unsigned long>(
               connectionDiagnostics.gattNotificationsSent),
           static_cast<unsigned long>(
               connectionDiagnostics.gattNotificationDrops),
           connectionDiagnostics.lastGattStatusPacketSize);
  f.printf("GATT Last Notify Results: request=0x%02x notify=0x%02x\n",
           connectionDiagnostics.lastGattNotifyRequestResult,
           connectionDiagnostics.lastGattNotifyResult);
  f.printf("Rejected Connections: le=%lu gatt=%lu l2cap=%lu\n",
           static_cast<unsigned long>(connectionDiagnostics.rejectedLeConnections),
           static_cast<unsigned long>(
               connectionDiagnostics.rejectedGattConnections),
           static_cast<unsigned long>(
               connectionDiagnostics.rejectedL2capConnections));
  f.printf("Advertising Data Applied After HCI Working: %s\n",
           advertisementDiagnostics.appliedAfterHciWorking ? "yes" : "no");
  f.printf("Advertising Data Length: %u/31\n",
           advertisementDiagnostics.advertisingDataLength);
  f.printf("Advertising Data Overflow: %s\n",
           advertisementDiagnostics.advertisingDataOverflow ? "yes" : "no");
  f.print("Advertising Data Hex: ");
  writeHexData(f, advertisementDiagnostics.advertisingData,
               advertisementDiagnostics.advertisingDataLength);
  f.print("\n");
  writeAdvertisementFields(f, "Advertising Data",
                           advertisementDiagnostics.advertisingData,
                           advertisementDiagnostics.advertisingDataLength);
  f.printf("Scan Response Length: %u/31\n",
           advertisementDiagnostics.scanResponseDataLength);
  f.printf("Scan Response Overflow: %s\n",
           advertisementDiagnostics.scanResponseDataOverflow ? "yes" : "no");
  f.print("Scan Response Hex: ");
  writeHexData(f, advertisementDiagnostics.scanResponseData,
               advertisementDiagnostics.scanResponseDataLength);
  f.print("\n");
  writeAdvertisementFields(f, "Scan Response",
                           advertisementDiagnostics.scanResponseData,
                           advertisementDiagnostics.scanResponseDataLength);
  f.close();
}

bool writeStatusToDiskSafely(const char* diagnosticsSnapshot) {
  bool wroteStatus = false;
  noInterrupts();
  if (!_statusDriveMounted) {
    writeStatusToDisk(CHIP_ID, diagnosticsSnapshot);
    wroteStatus = true;
  }
  interrupts();
  _lastStatusDiskWrite = millis();
  return wroteStatus;
}

// ==================================================
// Bluetooth Transport Functions
// ==================================================

void sendData() {
  unsigned long statusBuildMs = millis();
  if (_lastStatusBuildMs != 0) {
    _lastStatusBuildIntervalMs = statusBuildMs - _lastStatusBuildMs;
    if (_lastStatusBuildIntervalMs > _maxStatusBuildIntervalMs) {
      _maxStatusBuildIntervalMs = _lastStatusBuildIntervalMs;
    }
  }
  _lastStatusBuildMs = statusBuildMs;
  _statusPacketBuildCount++;

  int size = 0;
  char buffer[512];
  int ptr = 0;
  uint16_t fieldMask = 0;

  uint16ToNetwork(seq, buffer);
  buffer[2] = wpilibudp::lastControlByteReceived();
  ptr = wpilibudp::PACKET_HEADER_SIZE;

  // Encoders
  static constexpr uint divisor = xrp::Encoder::getDivisor();
  for (int i = 0; i < 4; i++) {
    int encoderValue = xrp::readEncoderRaw(i);
    uint encoderPeriod = xrp::readEncoderPeriod(i);

    // We want to flip the encoder 0 value (left motor encoder) so that this returns
    // positive values when moving forward.
    if (i == 0) {
      encoderValue = -encoderValue;
      encoderPeriod ^= 1;  // Last bit is direction bit; Flip it.
    }

    fieldMask |= wpilibudp::STATUS_ENCODER_0 << i;
    ptr += wpilibudp::writeEncoderData(encoderValue, encoderPeriod, divisor,
                                       buffer, ptr);
  }

  // DIO (currently just the button)
  fieldMask |= wpilibudp::STATUS_DIO;
  ptr += wpilibudp::writeDIOData(0x01, xrp::isUserButtonPressed() ? 0x01 : 0x00,
                                 buffer, ptr);

  // Gyro and accel data
  float gyroRates[3] = {
      xrp::imuGetGyroRateX(),
      xrp::imuGetGyroRateY(),
      xrp::imuGetGyroRateZ(),
  };

  float gyroAngles[3] = {
      xrp::imuGetRoll(),
      xrp::imuGetPitch(),
      xrp::imuGetYaw(),
  };

  float accels[3] = {
      xrp::imuGetAccelX(),
      xrp::imuGetAccelY(),
      xrp::imuGetAccelZ(),
  };

  fieldMask |= wpilibudp::STATUS_GYRO;
  ptr += wpilibudp::writeGyroData(gyroRates, gyroAngles, buffer, ptr);
  fieldMask |= wpilibudp::STATUS_ACCEL;
  ptr += wpilibudp::writeAccelData(accels, buffer, ptr);

  if (xrp::reflectanceInitialized()) {
    fieldMask |= wpilibudp::STATUS_ANALOG_0;
    ptr += wpilibudp::writeAnalogData(xrp::getReflectanceLeft5V(), buffer, ptr);
    fieldMask |= wpilibudp::STATUS_ANALOG_1;
    ptr += wpilibudp::writeAnalogData(xrp::getReflectanceRight5V(), buffer, ptr);
  }

  if (xrp::rangefinderInitialized()) {
    fieldMask |= wpilibudp::STATUS_ANALOG_2;
    ptr += wpilibudp::writeAnalogData(xrp::getRangefinderDistance5V(), buffer, ptr);
  }

  fieldMask |= wpilibudp::STATUS_TIMING;
  ptr += wpilibudp::writeTimingData(buffer, ptr);
  uint16ToNetwork(fieldMask, buffer, 3);

  // ptr should now point to 1 past the last byte
  size = ptr;

  if (bluetooth_transport::sendPacket(buffer, size)) {
    unsigned long statusAcceptedMs = millis();
    if (_lastStatusAcceptedMs != 0) {
      _lastStatusAcceptedIntervalMs =
          statusAcceptedMs - _lastStatusAcceptedMs;
      if (_lastStatusAcceptedIntervalMs > _maxStatusAcceptedIntervalMs) {
        _maxStatusAcceptedIntervalMs = _lastStatusAcceptedIntervalMs;
      }
    }
    _lastStatusAcceptedMs = statusAcceptedMs;
    _statusPacketAcceptedCount++;
    seq++;
  } else {
    _statusPacketRejectedCount++;
  }
}

void checkPrintStatus() {
  if (millis() - _lastStatusDiskWrite >= STATUS_DISK_UPDATE_INTERVAL_MS) {
    writeStatusToDiskSafely("periodic refresh");
  }

  if (millis() - _lastMessageStatusPrint > 5000) {
    int usedHeap = rp2040.getUsedHeap();
    const auto& btDiag = bluetooth_transport::connectionDiagnostics();
    Serial.printf("t(ms):%u h:%d bt:%d lt(us):%u gatt:%d notify:%d "
                  "mtu:%u size:%u ctrl:%lu cccd:%lu q:%lu bN:%lu bM:%lu "
                  "req:%lu cb:%lu imm:%lu sent:%lu drop:%lu rx:%u/%u "
                  "rxmax:%u rxdrop:%lu l2q:%lu l2i:%lu l2s:%lu l2d:%lu "
                  "rej:%lu/%lu/%lu last:%02x/%02x/%02x\n",
                  millis(),
                  usedHeap,
                  bluetooth_transport::connected() ? 1 : 0,
                  _avgLoopTimeUs,
                  btDiag.gattConnected ? 1 : 0,
                  btDiag.gattNotificationsEnabled ? 1 : 0,
                  btDiag.gattPayloadMtu,
                  btDiag.lastGattStatusPacketSize,
                  static_cast<unsigned long>(btDiag.gattControlPacketsReceived),
                  static_cast<unsigned long>(btDiag.gattCccdWrites),
                  static_cast<unsigned long>(btDiag.gattStatusPacketsQueued),
                  static_cast<unsigned long>(
                      btDiag.gattStatusPacketsBlockedNotifications),
                  static_cast<unsigned long>(
                      btDiag.gattStatusPacketsBlockedMtu),
                  static_cast<unsigned long>(btDiag.gattNotificationRequests),
                  static_cast<unsigned long>(btDiag.gattNotificationCallbacks),
                  static_cast<unsigned long>(
                      btDiag.gattNotificationImmediateSends),
                  static_cast<unsigned long>(btDiag.gattNotificationsSent),
                  static_cast<unsigned long>(btDiag.gattNotificationDrops),
                  btDiag.rxQueueUsed,
                  btDiag.rxQueueDepth,
                  btDiag.rxQueueMaxUsed,
                  static_cast<unsigned long>(btDiag.rxPacketsDropped),
                  static_cast<unsigned long>(btDiag.l2capStatusPacketsQueued),
                  static_cast<unsigned long>(btDiag.l2capImmediateSends),
                  static_cast<unsigned long>(btDiag.l2capPacketsSent),
                  static_cast<unsigned long>(btDiag.l2capSendDrops),
                  static_cast<unsigned long>(btDiag.rejectedLeConnections),
                  static_cast<unsigned long>(btDiag.rejectedGattConnections),
                  static_cast<unsigned long>(btDiag.rejectedL2capConnections),
                  btDiag.lastL2capSendResult,
                  btDiag.lastGattNotifyRequestResult,
                  btDiag.lastGattNotifyResult);
    _lastMessageStatusPrint = millis();
  }
}

void updateLoopTime(unsigned long loopStart) {
  unsigned long loopTime = micros() - loopStart;
  if (loopTime > _maxLoopTimeUs) {
    _maxLoopTimeUs = loopTime;
  }
  unsigned long totalTime = _avgLoopTimeUs * _loopTimeMeasurementCount;
  _loopTimeMeasurementCount++;

  _avgLoopTimeUs = (totalTime + loopTime) / _loopTimeMeasurementCount;
}

void setupBluetoothTransport() {
  bluetooth_transport::begin(BLUETOOTH_DEVICE_NAME);

  Serial.println("[BT] Bluetooth transport ready");
}

void setup() {
  // Start Serial port for logging
  Serial.begin(115200);

  // Start LittleFS for read/write from disk
  LittleFS.begin();

  // Set up the I2C pins
  MYWIRE.setSCL(I2C_SCL_1);
  MYWIRE.setSDA(I2C_SDA_1);
  MYWIRE.begin();

  // Give a few seconds if attaching a Serial port listener
  delay(2000);

  // Generate the default Bluetooth name suffix using the flash ID
  pico_unique_board_id_t id_out;
  pico_get_unique_board_id(&id_out);
  snprintf(CHIP_ID, sizeof(CHIP_ID), "%02x%02x-%02x%02x", id_out.id[4],
           id_out.id[5], id_out.id[6], id_out.id[7]);
  char defaultBluetoothNameSuffix[20];
  snprintf(defaultBluetoothNameSuffix, sizeof(defaultBluetoothNameSuffix), "%s",
           CHIP_ID);

  XRPConfiguration config = loadConfiguration(defaultBluetoothNameSuffix);
  std::string bluetoothDeviceName =
      buildBluetoothDeviceName(config.bluetoothConfig.deviceNameSuffix);
  strncpy(BLUETOOTH_DEVICE_NAME, bluetoothDeviceName.c_str(),
          sizeof(BLUETOOTH_DEVICE_NAME) - 1);
  BLUETOOTH_DEVICE_NAME[sizeof(BLUETOOTH_DEVICE_NAME) - 1] = '\0';

  // MUST BE BEFORE imuCalibrate (has digitalWrites) and Bluetooth startup
  xrp::robotInit();

  // Initialize IMU
  Serial.println("[IMU] Initializing IMU");
  xrp::imuInit(IMU_I2C_ADDR, &MYWIRE);

  Serial.println("[IMU] Beginning IMU calibration");
  xrp::imuCalibrate(5000);

  // Setup Bluetooth transport
  setupBluetoothTransport();

  // Write current status file
  writeStatusToDiskSafely("startup");

  // NOTE: For now, we'll force init the reflectance sensor
  // TODO Enable this via configuration
  xrp::reflectanceInit();

  // NOTE: For now we'll force init the rangefinder
  // TODO enable this via configuration
  xrp::rangefinderInit();

  _lastMessageStatusPrint = millis();
  // Emulates a FAT-formatted USB stick
  // to allow txt file to be read if USB connected
  singleFileDrive.onPlug(statusDrivePlugged);
  singleFileDrive.onUnplug(statusDriveUnplugged);
  singleFileDrive.begin("status.txt", "XRP-Status.txt");
}

void loop() {
  unsigned long loopStartTime = micros();

  // Check for data from the Bluetooth transport.
  size_t packetSize = 0;
  while (bluetooth_transport::readPacket(
      transportPacketBuf, sizeof(transportPacketBuf), &packetSize)) {
    wpilibudp::processPacket(transportPacketBuf, static_cast<int>(packetSize));
  }

  xrp::imuPeriodic();
  xrp::rangefinderPollForData();

  // Disable the robot when the driver station watchdog times out.
  // Also reset the max sequence number so we can handle reconnects.
  if (!wpilibudp::dsWatchdogActive()) {
    wpilibudp::resetState();
    xrp::robotSetEnabled(false);
    xrp::imuSetEnabled(false);
  }

  if (xrp::robotPeriodic()) {
    // Package up and send all the data to the Bluetooth client.
    sendData();
  }

  updateLoopTime(loopStartTime);
  checkPrintStatus();
}

void loop1() {
  if (xrp::rangefinderInitialized()) {
    xrp::rangefinderPeriodic();
  }

  delay(50);
}
