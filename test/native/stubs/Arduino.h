#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
using uint = unsigned;
using boolean = bool;
inline uint32_t testMicros = 1000000;
inline uint32_t micros() { return testMicros; }
inline uint32_t millis() { return testMicros / 1000; }
inline bool testInterruptsEnabled = true;
inline void noInterrupts() { testInterruptsEnabled = false; }
inline void interrupts() { testInterruptsEnabled = true; }
inline void delay(unsigned long) {}
struct TestRP2040 {
  unsigned restarts = 0;
  void restart() { ++restarts; }
  int getUsedHeap() { return 0; }
};
inline TestRP2040 rp2040;
struct pico_unique_board_id_t { uint8_t id[8]{}; };
inline void pico_get_unique_board_id(pico_unique_board_id_t*) {}
struct TestSerial {
  void begin(unsigned) {}
  template <typename... T>
  void printf(const char*, T...) {}
  void print(const char*) {}
  void println(const char*) {}
};
inline TestSerial Serial;
