#pragma once
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"
#include "WString.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

class HWCDC : public Stream {
public:
  void begin(unsigned long baud) {}
  void setTxTimeoutMs(uint32_t timeoutMs) {}
  size_t write(uint8_t c) override {
    emit(reinterpret_cast<const char *>(&c), 1);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    emit(reinterpret_cast<const char *>(buffer), size);
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
      emit(format, strlen(format));
    } else {
      char buf[256];
      const int len = snprintf(buf, sizeof(buf), format, args...);
      if (len > 0) emit(buf, static_cast<size_t>(len));
    }
  }
  operator bool() const { return true; }

private:
  // stderr from a native .so on Android never reaches `adb logcat` -- the
  // Zygote-forked app process has no terminal, and nothing redirects its fd 2
  // into the pipe logcat reads (confirmed 2026-08-24: LOG_ERR lines that must
  // have fired -- PinLog::append() only ever succeeded for one catalogue key
  // on a test device -- left zero trace, while an unrelated SDL_Log line from
  // Java-side code showed up fine under the "SDL/APP" tag). So route the same
  // bytes through __android_log_print too, buffered to whole lines because
  // logPrintf() writes one line per call but callers are free to write() a
  // single byte at a time (Stream's default multi-byte write loops one byte
  // per call).
#if defined(__ANDROID__)
  std::string androidLineBuf_;
#endif
  void emit(const char *data, size_t size) {
    std::cerr.write(data, static_cast<std::streamsize>(size));
#if defined(__ANDROID__)
    for (size_t i = 0; i < size; ++i) {
      if (data[i] == '\n') {
        __android_log_print(ANDROID_LOG_INFO, "explorink", "%s", androidLineBuf_.c_str());
        androidLineBuf_.clear();
      } else {
        androidLineBuf_ += data[i];
      }
    }
#endif
  }
};

// CrossPoint uses HardwareSerial when ARDUINO_USB_CDC_ON_BOOT is not defined.
// The simulator has a single stderr-backed serial endpoint, so both Arduino
// serial types intentionally resolve to the same host implementation.
using HardwareSerial = HWCDC;

extern HWCDC Serial;
