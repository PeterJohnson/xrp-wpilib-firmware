#pragma once

#include <stddef.h>

#include <string>

// Increment this whenever the persisted config schema changes.
#define XRP_CONFIG_VERSION 2

constexpr size_t XRP_BLUETOOTH_NAME_MAX_LENGTH = 29;

class XRPBluetoothConfig {
 public:
  std::string deviceName{""};
};

class XRPConfiguration {
 public:
  XRPBluetoothConfig bluetoothConfig;

  std::string toJsonString() const;
};

XRPConfiguration generateDefaultConfig(const std::string& defaultBluetoothName);
XRPConfiguration loadConfiguration(const std::string& defaultBluetoothName);
bool isValidBluetoothDeviceName(const std::string& deviceName);
