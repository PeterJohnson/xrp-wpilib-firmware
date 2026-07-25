#include "config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {

constexpr const char* kConfigPath = "/config.json";

void writeConfigToDisk(const XRPConfiguration& config) {
  File f = LittleFS.open(kConfigPath, "w");
  f.print(config.toJsonString().c_str());
  f.close();
}

XRPConfiguration resetToDefaultConfig(const std::string& defaultBluetoothName) {
  XRPConfiguration config = generateDefaultConfig(defaultBluetoothName);
  writeConfigToDisk(config);
  return config;
}

}  // namespace

bool isValidBluetoothDeviceName(const std::string& deviceName) {
  if (deviceName.empty() || deviceName.length() > XRP_BLUETOOTH_NAME_MAX_LENGTH) {
    return false;
  }

  for (char c : deviceName) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }

  return true;
}

XRPConfiguration generateDefaultConfig(const std::string& defaultBluetoothName) {
  XRPConfiguration defaultConfig;
  defaultConfig.bluetoothConfig.deviceName = defaultBluetoothName;
  return defaultConfig;
}

std::string XRPConfiguration::toJsonString() const {
  JsonDocument config;

  config["configVersion"] = XRP_CONFIG_VERSION;

  JsonObject bluetooth = config["bluetooth"].to<JsonObject>();
  bluetooth["deviceName"] = bluetoothConfig.deviceName;

  std::string ret;
  serializeJsonPretty(config, ret);
  return ret;
}

XRPConfiguration loadConfiguration(const std::string& defaultBluetoothName) {
  File f = LittleFS.open(kConfigPath, "r");
  if (!f) {
    Serial.println("[CONFIG] No config file found. Creating default");
    return resetToDefaultConfig(defaultBluetoothName);
  }

  JsonDocument configJson;
  auto jsonErr = deserializeJson(configJson, f);
  f.close();

  if (jsonErr) {
    Serial.print("[CONFIG] Deserialization failed: ");
    Serial.println(jsonErr.f_str());
    Serial.println("[CONFIG] Using default");
    return resetToDefaultConfig(defaultBluetoothName);
  }

  if (configJson["configVersion"] != XRP_CONFIG_VERSION) {
    Serial.println("[CONFIG] Configuration version mismatch. Using default");
    return resetToDefaultConfig(defaultBluetoothName);
  }

  if (!configJson["bluetooth"].is<JsonObject>()) {
    Serial.println("[CONFIG] No Bluetooth information specified. Using default");
    return resetToDefaultConfig(defaultBluetoothName);
  }

  XRPConfiguration config = generateDefaultConfig(defaultBluetoothName);
  bool shouldWrite = false;

  JsonObject bluetoothInfo = configJson["bluetooth"].as<JsonObject>();
  if (bluetoothInfo["deviceName"].is<const char*>()) {
    std::string configuredDeviceName =
        bluetoothInfo["deviceName"].as<std::string>();
    if (isValidBluetoothDeviceName(configuredDeviceName)) {
      config.bluetoothConfig.deviceName = configuredDeviceName;
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
