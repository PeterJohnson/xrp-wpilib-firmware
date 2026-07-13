#pragma once

#include <stddef.h>

namespace bluetooth_transport {

constexpr size_t kMaxPacketSize = 512;
constexpr unsigned kLePsm = 0x0081;
constexpr const char* kGattServiceUuid = "7d2ea28a-f7bd-485d-9d6a-2c3f0b214a3f";
constexpr const char* kGattControlCharacteristicUuid =
    "7d2ea28b-f7bd-485d-9d6a-2c3f0b214a3f";
constexpr const char* kGattStatusCharacteristicUuid =
    "7d2ea28c-f7bd-485d-9d6a-2c3f0b214a3f";
constexpr unsigned kPreferredConnectionIntervalMin = 6;   // 7.5 ms
constexpr unsigned kPreferredConnectionIntervalMax = 12;  // 15 ms
constexpr unsigned kPreferredSlaveLatency = 0;

void begin(const char* deviceName);
bool connected();
bool readPacket(char* buffer, size_t bufferSize, size_t* packetSize);
bool sendPacket(const char* buffer, size_t packetSize);

}  // namespace bluetooth_transport
