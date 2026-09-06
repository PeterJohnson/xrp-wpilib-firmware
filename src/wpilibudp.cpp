#include <Arduino.h>

#include "byteutils.h"
#include "wpilibudp.h"
#include "robot.h"
#include "watchdog.h"
#include "imu.h"

// Since we might (nay, will) rollover, the fudge factor lets us deal with cases
// like 65532, 65533, 0, 65534, 65535 by taking 0 as the new highest seq number
#define SEQ_FUDGE_FACTOR 5
#define SEQ_MAX 65535

namespace wpilibudp {

uint16_t currMaxSeq = 0;
uint32_t lastControlPacketMicros = 0;
uint8_t lastControlByte = 0;
bool receivedControlPacket = false;
DeviceNameHandler deviceNameHandler = nullptr;
xrp::Watchdog _dsWatchdog{"status"};

bool hasField(uint16_t mask, uint16_t field) { return (mask & field) != 0; }

int16_t clampMotorPwm(int16_t pwm) {
  if (pwm > MOTOR_MAX_PWM) {
    return MOTOR_MAX_PWM;
  }
  if (pwm < -MOTOR_MAX_PWM) {
    return -MOTOR_MAX_PWM;
  }
  return pwm;
}

uint8_t clampServoDegrees(uint8_t degrees) {
  if (degrees > SERVO_MAX_DEGREES) {
    return SERVO_MAX_DEGREES;
  }
  return degrees;
}

uint16_t voltageToAnalogValue(float voltage) {
  if (voltage <= 0.0f) {
    return 0;
  }
  if (voltage >= ANALOG_MAX_VOLTAGE) {
    return ANALOG_MAX_VALUE;
  }
  return static_cast<uint16_t>(
      (voltage * ANALOG_MAX_VALUE / ANALOG_MAX_VOLTAGE) + 0.5f);
}

int expectedControlPacketSize(uint16_t mask) {
  int size = PACKET_HEADER_SIZE;

  for (int channel = 0; channel < 4; channel++) {
    if (hasField(mask, CONTROL_MOTOR_0 << channel)) {
      size += sizeof(int16_t);
    }
  }

  for (int channel = 4; channel < 8; channel++) {
    if (hasField(mask, 1u << channel)) {
      size += sizeof(uint8_t);
    }
  }

  if (hasField(mask, CONTROL_DIO)) {
    size += 2;
  }

  return size;
}

uint32_t normalizeEncoderPeriod(uint32_t period, uint32_t divisor) {
  if (period == UINT32_MAX || divisor == 0) {
    return UINT32_MAX;
  }

  uint32_t direction = period & 1u;
  uint32_t ticks = period >> 1;
  uint64_t periodUs =
      (static_cast<uint64_t>(ticks) * ENCODER_PERIOD_DENOMINATOR + divisor / 2) /
      divisor;
  if (periodUs > (UINT32_MAX >> 1)) {
    return UINT32_MAX;
  }

  return (static_cast<uint32_t>(periodUs) << 1) | direction;
}

uint16_t encodeControlRxAge10Us() {
  if (!receivedControlPacket) {
    return INVALID_CONTROL_RX_AGE_10_US;
  }

  uint32_t ageUs = static_cast<uint32_t>(micros() - lastControlPacketMicros);
  uint32_t age10Us =
      (ageUs + (CONTROL_RX_AGE_UNIT_US / 2)) / CONTROL_RX_AGE_UNIT_US;
  if (age10Us >= INVALID_CONTROL_RX_AGE_10_US) {
    return INVALID_CONTROL_RX_AGE_10_US;
  }
  return static_cast<uint16_t>(age10Us);
}

bool acceptSequence(uint16_t seq) {
  if (seq > currMaxSeq) {
    currMaxSeq = seq;
    return true;
  }

  if (SEQ_MAX - seq < SEQ_FUDGE_FACTOR) {
    // Rollover
    currMaxSeq = seq;
    return true;
  }

  return false;
}

bool processDeviceNamePacket(char* buffer, int size, uint16_t seq) {
  if (deviceNameHandler == nullptr ||
      size < PACKET_HEADER_SIZE + static_cast<int>(sizeof(uint8_t))) {
    return false;
  }

  uint8_t deviceNameLength =
      static_cast<uint8_t>(buffer[PACKET_HEADER_SIZE]);
  if (deviceNameLength == 0 ||
      deviceNameLength > CONTROL_DEVICE_NAME_MAX_LENGTH ||
      size != PACKET_HEADER_SIZE + 1 + deviceNameLength) {
    return false;
  }

  if (!acceptSequence(seq)) {
    return false;
  }

  return deviceNameHandler(&buffer[PACKET_HEADER_SIZE + 1], deviceNameLength);
}

bool dsWatchdogActive() { return _dsWatchdog.satisfied(); }

void setDeviceNameHandler(DeviceNameHandler handler) {
  deviceNameHandler = handler;
}

void resetState() {
  currMaxSeq = 0;
  lastControlPacketMicros = 0;
  lastControlByte = 0;
  receivedControlPacket = false;
}

uint8_t lastControlByteReceived() { return lastControlByte; }

bool processPacket(char* buffer, int size) {
  if (size < PACKET_HEADER_SIZE) {
    return false;
  }

  // Overall packet format is
  //       2           1           2              n
  // [    seq    ] [ ctrl ] [ field mask ] [ field data ]

  uint16_t seq = networkToUInt16(buffer);
  uint8_t ctrl = buffer[2];
  uint16_t fieldMask = networkToUInt16(buffer, 3);
  if ((fieldMask & ~CONTROL_ALL_FIELDS) != 0) {
    return false;
  }

  if (fieldMask == CONTROL_DEVICE_NAME) {
    return processDeviceNamePacket(buffer, size, seq);
  }

  if (hasField(fieldMask, CONTROL_DEVICE_NAME) ||
      size != expectedControlPacketSize(fieldMask)) {
    return false;
  }

  // Check if the sequence number exceeds our latest seen seq number
  if (!acceptSequence(seq)) {
    // Not processing this
    return false;
  }
  lastControlPacketMicros = micros();
  lastControlByte = ctrl;
  receivedControlPacket = true;

  // Control byte essentially encodes the enabled/disabled state
  xrp::robotSetEnabled(ctrl == 1);

  // Feed the watchdog
  _dsWatchdog.feed();

  int ptr = PACKET_HEADER_SIZE;
  for (int channel = 0; channel < 4; channel++) {
    if (hasField(fieldMask, CONTROL_MOTOR_0 << channel)) {
      int16_t pwm = clampMotorPwm(networkToInt16(buffer, ptr));
      ptr += sizeof(int16_t);
      double value = static_cast<double>(pwm) / MOTOR_MAX_PWM;
      xrp::setPwmValue(channel, value);
    }
  }

  for (int channel = 4; channel < 8; channel++) {
    if (hasField(fieldMask, 1u << channel)) {
      uint8_t degrees = clampServoDegrees(buffer[ptr++]);

      // Servo position info comes as degrees; convert to the -1 to 1 range.
      double value = (static_cast<double>(degrees) / 90.0) - 1.0;
      xrp::setPwmValue(channel, value);
    }
  }

  if (hasField(fieldMask, CONTROL_DIO)) {
    uint8_t presentMask = buffer[ptr++];
    uint8_t valueMask = buffer[ptr++];
    for (int channel = 0; channel < 8; channel++) {
      uint8_t bit = 1u << channel;
      if ((presentMask & bit) != 0) {
        xrp::setDigitalOutput(channel, (valueMask & bit) != 0);
      }
    }
  }

  return true;
}

// ===================
// Message Encoders
// ===================

int writeEncoderData(int count, uint period, uint divisor, char* buffer,
                     int offset) {
  // Encoder data is count(4) + normalizedPeriod(4).
  int i = offset;
  int32ToNetwork(count, buffer, i);
  i += sizeof(int);
  uint32ToNetwork(normalizeEncoderPeriod(period, divisor), buffer, i);
  i += sizeof(uint);
  return i - offset;
}

int writeDIOData(uint8_t presentMask, uint8_t valueMask, char* buffer,
                 int offset) {
  buffer[offset] = presentMask;
  buffer[offset + 1] = valueMask;
  return 2;
}

int writeGyroData(float rates[3], float angles[3], char* buffer, int offset) {
  // Gyro data is rateX(4) rateY(4) rateZ(4) angleX(4) angleY(4) angleZ(4).
  int ratePtr = offset;
  int anglePtr = ratePtr + 12;
  for (int i = 0; i < 3; i++) {
    floatToNetwork(rates[i], buffer, ratePtr);
    floatToNetwork(angles[i], buffer, anglePtr);

    ratePtr += 4;
    anglePtr += 4;
  }
  return 24;
}

int writeAccelData(float accels[3], char* buffer, int offset) {
  // Accel data is accX(4) accY(4) accZ(4).
  int ptr = offset;

  for (int i = 0; i < 3; i++) {
    floatToNetwork(accels[i], buffer, ptr);
    ptr += 4;
  }

  return 12;
}

int writeAnalogData(float voltage, char* buffer, int offset) {
  uint16ToNetwork(voltageToAnalogValue(voltage), buffer, offset);

  return sizeof(uint16_t);
}

int writeTimingData(char* buffer, int offset) {
  // Timing data is lastControlSeq(2) + controlRxAge10Us(2).
  uint16ToNetwork(currMaxSeq, buffer, offset);
  uint16ToNetwork(encodeControlRxAge10Us(), buffer, offset + 2);

  return 4;
}

} // namespace wpilibudp
