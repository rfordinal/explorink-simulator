#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Host stand-in for Arduino's Print. The Arduino base class carries the whole
// print/println overload set plus printf(); firmware calls any of them, so the
// emulation layer has to offer the same surface or the build breaks on an
// overload nobody thought about. Numbers are formatted with printf rather than
// Arduino's own itoa path -- same output for base 10, which is all firmware
// uses here.
class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *buffer, size_t size) {
    size_t n = 0;
    while (size--) {
      n += write(*buffer++);
    }
    return n;
  }
  virtual void flush() {}

  size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }

  size_t print(char c) { return write((uint8_t)c); }
  size_t print(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
  size_t print(unsigned char n) { return printNumber("%u", (unsigned int)n); }
  size_t print(int n) { return printNumber("%d", n); }
  size_t print(unsigned int n) { return printNumber("%u", n); }
  size_t print(long n) { return printNumber("%ld", n); }
  size_t print(unsigned long n) { return printNumber("%lu", n); }
  size_t print(long long n) { return printNumber("%lld", n); }
  size_t print(unsigned long long n) { return printNumber("%llu", n); }
  size_t print(double n) { return printNumber("%.2f", n); }
  size_t print(float n) { return print((double)n); }

  size_t println() { return print("\r\n"); }
  template <typename T> size_t println(T v) {
    size_t n = print(v);
    n += print("\r\n");
    return n;
  }

  size_t printf(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    va_list args;
    va_start(args, format);
    char stack[256];
    va_list probe;
    va_copy(probe, args);
    const int needed = vsnprintf(stack, sizeof(stack), format, probe);
    va_end(probe);
    if (needed < 0) {
      va_end(args);
      return 0;
    }
    size_t written = 0;
    if ((size_t)needed < sizeof(stack)) {
      written = write((const uint8_t *)stack, (size_t)needed);
    } else {
      // Long lines happen (the power log's CSV row), so grow instead of
      // truncating: a silently clipped line reads as a firmware bug.
      char *heap = (char *)malloc((size_t)needed + 1);
      if (heap) {
        vsnprintf(heap, (size_t)needed + 1, format, args);
        written = write((const uint8_t *)heap, (size_t)needed);
        free(heap);
      }
    }
    va_end(args);
    return written;
  }

private:
  template <typename T> size_t printNumber(const char *format, T value) {
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), format, value);
    return n > 0 ? write((const uint8_t *)buf, (size_t)n) : 0;
  }
};
