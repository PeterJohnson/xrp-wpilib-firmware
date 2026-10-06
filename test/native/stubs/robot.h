#pragma once
#include <cstdint>
namespace xrp {
inline bool testRobotEnabled = false;
inline double testPwm[8]{};
inline bool testDio[8]{};
inline void robotSetEnabled(bool enabled) { testRobotEnabled = enabled; }
inline void setPwmValue(int channel, double value) { testPwm[channel] = value; }
inline void setDigitalOutput(int channel, bool value) {
  testDio[channel] = value;
}
}  // namespace xrp
