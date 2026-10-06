#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
using uint = unsigned;
using boolean = bool;
inline uint32_t testMicros = 1000000;
inline uint32_t micros() { return testMicros; }
inline uint32_t testMillisOffset = 0;
inline uint32_t millis() { return testMillisOffset + testMicros / 1000; }
