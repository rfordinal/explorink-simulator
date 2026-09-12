#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "Print.h"
#include "WString.h"

class Stream;

// Derives from Print for the same reason the real one does: Arduino's
// NetworkClient is a Stream, and Stream is a Print, so firmware may hand a
// client to anything taking a `Print&`. WebDAVHandler::handleGet() does exactly
// that (readFileToStream(path, client)), and without this base the native build
// fails there with "cannot convert 'NetworkClient' to 'Print&'" while every
// device build is fine -- a divergence in the shim, not in the firmware.
class NetworkClient : public Print {
public:
  NetworkClient() {}
  explicit NetworkClient(int fd);
  virtual ~NetworkClient() {}
  virtual int connect(const char *host, uint16_t port);
  virtual size_t write(const uint8_t *buf, size_t size);
  virtual size_t write(const char *str) {
    return write((const uint8_t *)str, strlen(str));
  }
  virtual size_t write(uint8_t c) { return write(&c, 1); }
  virtual size_t write(Stream &stream);
  template <typename T> size_t write(T &streamLike) {
    uint8_t buffer[4096];
    size_t total = 0;
    while (streamLike.available() > 0) {
      const int count = streamLike.read(buffer, sizeof(buffer));
      if (count <= 0)
        break;
      const size_t written = write(buffer, static_cast<size_t>(count));
      total += written;
      if (written != static_cast<size_t>(count))
        break;
    }
    return total;
  }
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual void stop();
  virtual void clear() {}
  virtual uint8_t connected();
  operator bool() { return connected(); }

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

class NetworkClientSecure : public NetworkClient {
public:
  void setHandshakeTimeout(uint32_t seconds) { (void)seconds; }
  void setInsecure() {}
};
