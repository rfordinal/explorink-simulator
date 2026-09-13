#pragma once

// Simulator shim for the SDK's FrontlightManager.
//
// The desktop build has no PWM and no panel light, so every method here is a
// no-op and present() is false. That is not a stub standing in for something
// unfinished: it is the same answer the real class gives on a board whose
// profile carries no frontlight (an X4 or an X3), so main.cpp takes exactly the
// path it takes there. Every call site already guards on present().
//
// Why a shim rather than the SDK library itself: the real header includes
// <BoardConfig.h> and reads BoardConfig::ACTIVE.frontlight, and the simulator
// ships its own reduced BoardConfig.h with no such member -- deliberately, so
// the native build never pulls in ESP32 GPIO headers. Adding the SDK library to
// the simulator's lib_deps would drag that dependency back in.
//
// Keep this in step with freeink-sdk/libs/hardware/FrontlightManager: a method
// main.cpp starts calling has to appear here too, or the simulator stops
// building. That is the cost of the split, and it is the same cost every other
// shim in this directory pays.

#include <cstdint>

class FrontlightManager {
 public:
  void begin() {}
  void setBrightness(uint8_t percent) { (void)percent; }
  void setBrightnessLevel(uint8_t level) { (void)level; }
  void off() {}
  void on() {}
  void park() {}
  void releaseOnWake() {}
  void setColorTemperature(uint8_t warmPercent) { (void)warmPercent; }

  bool present() const { return false; }
  bool hasColorTemperature() const { return false; }

  uint8_t brightness() const { return 0; }
  uint8_t brightnessLevel() const { return 0; }
  uint8_t colorTemperature() const { return 0; }
};
