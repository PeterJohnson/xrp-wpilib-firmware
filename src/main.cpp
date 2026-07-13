#include <Arduino.h>
#include <LittleFS.h>
#include <SingleFileDrive.h>
#include <Wire.h>

#include <string>

#include "bluetooth_transport.h"
#include "byteutils.h"
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

char DEFAULT_BT_NAME[32];

char transportPacketBuf[bluetooth_transport::kMaxPacketSize];

// TEMP: Status
unsigned long _lastMessageStatusPrint = 0;

unsigned long _avgLoopTimeUs = 0;
unsigned long _loopTimeMeasurementCount = 0;

uint16_t seq = 0;

// Generate the status text file
void writeStatusToDisk(const char* chipID) {
  File f = LittleFS.open("/status.txt", "w");

  size_t len;
  std::string versionString{reinterpret_cast<const char*>(GetResource_VERSION(&len)),
                            len};
  f.printf("Version: %s\n", versionString.c_str());
  f.printf("Chip ID: %s\n", chipID);
  f.printf("Transport: Bluetooth LE GATT + L2CAP Credit-Based Mode\n");
  f.printf("Bluetooth Name: %s\n", DEFAULT_BT_NAME);
  f.printf("GATT Service UUID: %s\n", bluetooth_transport::kGattServiceUuid);
  f.printf("GATT Control Characteristic UUID: %s\n",
           bluetooth_transport::kGattControlCharacteristicUuid);
  f.printf("GATT Status Characteristic UUID: %s\n",
           bluetooth_transport::kGattStatusCharacteristicUuid);
  f.printf("LE PSM: 0x%04x\n", bluetooth_transport::kLePsm);
  f.printf("Preferred Connection Interval: 7.5-15 ms, latency 0\n");
  f.printf("Packet Framing: one BLE packet per WPILib XRP payload\n");
  f.close();
}

// ==================================================
// Bluetooth Transport Functions
// ==================================================

void sendData() {
  int size = 0;
  char buffer[512];
  int ptr = 0;

  uint16ToNetwork(seq, buffer);
  buffer[2] = 0;  // Unset the control byte
  ptr = 3;

  // Encoders
  for (int i = 0; i < 4; i++) {
    int encoderValue = xrp::readEncoderRaw(i);
    uint encoderPeriod = xrp::readEncoderPeriod(i);

    // We want to flip the encoder 0 value (left motor encoder) so that this returns
    // positive values when moving forward.
    if (i == 0) {
      encoderValue = -encoderValue;
      encoderPeriod ^= 1;  // Last bit is direction bit; Flip it.
    }

    static constexpr uint divisor = xrp::Encoder::getDivisor();

    ptr += wpilibudp::writeEncoderData(i, encoderValue, encoderPeriod, divisor,
                                       buffer, ptr);
  }  // 4x 15 bytes

  // DIO (currently just the button)
  ptr += wpilibudp::writeDIOData(0, xrp::isUserButtonPressed(), buffer, ptr);
  // 1x 4 bytes

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

  ptr += wpilibudp::writeGyroData(gyroRates, gyroAngles, buffer, ptr);
  // 1x 26 bytes
  ptr += wpilibudp::writeAccelData(accels, buffer, ptr);
  // 1x 14 bytes

  if (xrp::reflectanceInitialized()) {
    ptr += wpilibudp::writeAnalogData(0, xrp::getReflectanceLeft5V(), buffer, ptr);
    ptr += wpilibudp::writeAnalogData(1, xrp::getReflectanceRight5V(), buffer, ptr);
  }

  if (xrp::rangefinderInitialized()) {
    ptr += wpilibudp::writeAnalogData(2, xrp::getRangefinderDistance5V(), buffer, ptr);
  }

  // ptr should now point to 1 past the last byte
  size = ptr;

  if (bluetooth_transport::sendPacket(buffer, size)) {
    seq++;
  }
}

void checkPrintStatus() {
  if (millis() - _lastMessageStatusPrint > 5000) {
    int usedHeap = rp2040.getUsedHeap();
    Serial.printf("t(ms):%u h:%d bt:%d lt(us):%u\n",
                  millis(),
                  usedHeap,
                  bluetooth_transport::connected() ? 1 : 0,
                  _avgLoopTimeUs);
    _lastMessageStatusPrint = millis();
  }
}

void updateLoopTime(unsigned long loopStart) {
  unsigned long loopTime = micros() - loopStart;
  unsigned long totalTime = _avgLoopTimeUs * _loopTimeMeasurementCount;
  _loopTimeMeasurementCount++;

  _avgLoopTimeUs = (totalTime + loopTime) / _loopTimeMeasurementCount;
}

void setupBluetoothTransport() {
  bluetooth_transport::begin(DEFAULT_BT_NAME);

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

  // Generate the default Bluetooth name using the flash ID
  pico_unique_board_id_t id_out;
  pico_get_unique_board_id(&id_out);
  char chipID[20];
  snprintf(chipID, sizeof(chipID), "%02x%02x-%02x%02x", id_out.id[4],
           id_out.id[5], id_out.id[6], id_out.id[7]);
  snprintf(DEFAULT_BT_NAME, sizeof(DEFAULT_BT_NAME), "WPIXRP-%s", chipID);

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
  writeStatusToDisk(chipID);

  // NOTE: For now, we'll force init the reflectance sensor
  // TODO Enable this via configuration
  xrp::reflectanceInit();

  // NOTE: For now we'll force init the rangefinder
  // TODO enable this via configuration
  xrp::rangefinderInit();

  _lastMessageStatusPrint = millis();
  // Emulates a FAT-formatted USB stick
  // to allow txt file to be read if USB connected
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
