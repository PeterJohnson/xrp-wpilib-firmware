#pragma once

#include <stddef.h>

namespace bluetooth_transport {

constexpr size_t kMaxPacketSize = 512;
constexpr unsigned kLePsm = 0x0081;

void begin(const char* deviceName);
bool connected();
bool readPacket(char* buffer, size_t bufferSize, size_t* packetSize);
bool sendPacket(const char* buffer, size_t packetSize);

}  // namespace bluetooth_transport
