#include "config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {

constexpr const char* CONFIG_PATH = "/config.json";

void writeConfigToDisk(const XRPConfiguration& config) {
  File f = LittleFS.open(CONFIG_PATH, "w");
  f.print(config.toJsonString().c_str());
  f.close();
}

XRPConfiguration resetToDefaultConfig(
    const std::string& defaultBluetoothNameSuffix) {
  XRPConfiguration config = generateDefaultConfig(defaultBluetoothNameSuffix);
  writeConfigToDisk(config);
  return config;
}

}  // namespace

std::string buildBluetoothDeviceName(const std::string& deviceNameSuffix) {
  return std::string(XRP_BLUETOOTH_NAME_PREFIX) + deviceNameSuffix;
}

std::string normalizeBluetoothDeviceNameSuffix(
    const std::string& deviceNameOrSuffix) {
  if (deviceNameOrSuffix.rfind(XRP_BLUETOOTH_NAME_PREFIX, 0) == 0) {
    return deviceNameOrSuffix.substr(XRP_BLUETOOTH_NAME_PREFIX_LENGTH);
  }

  return deviceNameOrSuffix;
}

bool isValidBluetoothDeviceNameSuffix(const std::string& deviceNameSuffix) {
  if (deviceNameSuffix.empty() ||
      deviceNameSuffix.length() > XRP_BLUETOOTH_NAME_SUFFIX_MAX_LENGTH) {
    return false;
  }

  for (char c : deviceNameSuffix) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }

  return true;
}

XRPConfiguration generateDefaultConfig(
    const std::string& defaultBluetoothNameSuffix) {
  XRPConfiguration defaultConfig;
  defaultConfig.bluetoothConfig.deviceNameSuffix = defaultBluetoothNameSuffix;
  return defaultConfig;
}

std::string XRPConfiguration::toJsonString() const {
  JsonDocument config;

  config["configVersion"] = XRP_CONFIG_VERSION;

  JsonObject bluetooth = config["bluetooth"].to<JsonObject>();
  bluetooth["deviceName"] = buildBluetoothDeviceName(
      bluetoothConfig.deviceNameSuffix);

  std::string ret;
  serializeJsonPretty(config, ret);
  return ret;
}

XRPConfiguration loadConfiguration(
    const std::string& defaultBluetoothNameSuffix) {
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("[CONFIG] No config file found. Creating default");
    return resetToDefaultConfig(defaultBluetoothNameSuffix);
  }

  JsonDocument configJson;
  auto jsonErr = deserializeJson(configJson, f);
  f.close();

  if (jsonErr) {
    Serial.print("[CONFIG] Deserialization failed: ");
    Serial.println(jsonErr.f_str());
    Serial.println("[CONFIG] Using default");
    return resetToDefaultConfig(defaultBluetoothNameSuffix);
  }

  if (configJson["configVersion"] != XRP_CONFIG_VERSION) {
    Serial.println("[CONFIG] Configuration version mismatch. Using default");
    return resetToDefaultConfig(defaultBluetoothNameSuffix);
  }

  if (!configJson["bluetooth"].is<JsonObject>()) {
    Serial.println("[CONFIG] No Bluetooth information specified. Using default");
    return resetToDefaultConfig(defaultBluetoothNameSuffix);
  }

  XRPConfiguration config = generateDefaultConfig(defaultBluetoothNameSuffix);
  bool shouldWrite = false;

  JsonObject bluetoothInfo = configJson["bluetooth"].as<JsonObject>();
  if (bluetoothInfo["deviceName"].is<const char*>()) {
    std::string configuredDeviceNameOrSuffix =
        bluetoothInfo["deviceName"].as<std::string>();
    std::string configuredDeviceNameSuffix =
        normalizeBluetoothDeviceNameSuffix(configuredDeviceNameOrSuffix);
    if (isValidBluetoothDeviceNameSuffix(configuredDeviceNameSuffix)) {
      config.bluetoothConfig.deviceNameSuffix = configuredDeviceNameSuffix;
      if (buildBluetoothDeviceName(configuredDeviceNameSuffix) !=
          configuredDeviceNameOrSuffix) {
        shouldWrite = true;
      }
    } else {
      Serial.println("[CONFIG] Invalid Bluetooth device name. Using default");
      shouldWrite = true;
    }
  } else {
    Serial.println("[CONFIG] Bluetooth device name missing. Using default");
    shouldWrite = true;
  }

  if (shouldWrite) {
    writeConfigToDisk(config);
  }

  return config;
}
