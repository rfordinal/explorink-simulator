#pragma once
#include <cstdio>
#include <iostream>

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"
#include "WString.h"
class HWCDC : public Stream {
public:
  void begin(unsigned long baud) {}
  void setTxTimeoutMs(uint32_t timeoutMs) {}
  size_t write(uint8_t c) override {
    std::cerr << (char)c;
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    std::cerr.write((const char *)buffer, size);
    return size;
  }
  int available() override { return 0; }
  // stderr never applies back-pressure, so the TX buffer is always empty.
  // Firmware that chunks a long reply against availableForWrite() needs a
  // positive number here or it stalls until its own timeout.
  int availableForWrite() { return 1024; }
  int read() override { return -1; }
  int peek() override { return -1; }
  template <typename... Args> void printf(const char *format, Args... args) {
    if constexpr (sizeof...(Args) == 0) {
      std::cerr << format;
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), format, args...);
      std::cerr << buf;
    }
  }
  operator bool() const { return true; }
};

// CrossPoint uses HardwareSerial when ARDUINO_USB_CDC_ON_BOOT is not defined.
// The simulator has a single stderr-backed serial endpoint, so both Arduino
// serial types intentionally resolve to the same host implementation.
using HardwareSerial = HWCDC;

extern HWCDC Serial;
