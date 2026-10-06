#include "config.h"

#include <cassert>
#include <cstdio>

#include "LittleFS.h"

const std::string defaultName = "AAAA-BBBB";
std::string loadName(const std::string& contents) {
  LittleFS.files[XRP_CONFIG_PATH] = contents;
  return loadConfiguration(defaultName).bluetoothConfig.deviceNameSuffix;
}
int main() {
  assert(loadConfiguration(defaultName).bluetoothConfig.deviceNameSuffix ==
         defaultName);
  assert(loadName("config_version = 2\r\n[Bluetooth]\r\ndeviceName = 'Bot # 2' "
                  "; note\r\n") == "Bot # 2");
  assert(loadName("config_version = 2\n[bluetooth]\ndeviceName = WPIXRP-A # "
                  "comment\n") == "A");
  for (const std::string version :
       {"2147483648", "4294967298", "999999999999999999999999", "-2", "3"}) {
    assert(loadName("config_version = " + version +
                    "\n[bluetooth]\ndeviceName = Custom") == defaultName);
  }
  assert(
      loadName(
          "config_version = 2\n[bluetooth]\ndeviceName = \"Bot\" junk\"\n") ==
      defaultName);
  assert(loadName("config_version = 2\n[bluetooth]\ndeviceName = \"Bot\n") ==
         defaultName);
  assert(loadName("config_version = 2\n[bluetooth]\ndeviceName = WPIXRP-\n") ==
         defaultName);
  std::puts("configuration tests passed");
}
