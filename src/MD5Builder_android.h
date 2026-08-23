#pragma once

// MD5 for Android. The Linux variant uses OpenSSL and the macOS one uses
// CommonCrypto; bionic ships neither, and OpenSSL was the ONLY reason the
// simulator linked libssl at all -- MD5Builder_linux.h:12 is its single
// consumer. A self-contained MD5 removes the whole cross-compile-OpenSSL
// problem for an Android build.
//
// Plain RFC 1321. The constants are generated rather than typed: K[i] is
// floor(abs(sin(i+1)) * 2^32).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WString.h"

// One sequence per object: begin(), then add() as often as needed, then
// calculate() once. calculate() finalises the state, so calling it twice, or
// add()ing after it, returns a digest of the wrong thing rather than failing.
// That is not an Android quirk -- OpenSSL's MD5_Final and CommonCrypto's
// CC_MD5_Final behave the same way, so the two other variants of this class
// have the identical contract, and a guard here would make Android the odd one
// out. Both callers (KOReaderDocumentId.cpp, KOReaderCredentialStore.cpp) use
// the sequence once.
class MD5Builder {
public:
  MD5Builder() { begin(); }

  void begin() {
    state_[0] = 0x67452301u;
    state_[1] = 0xefcdab89u;
    state_[2] = 0x98badcfeu;
    state_[3] = 0x10325476u;
    length_ = 0;
    buffered_ = 0;
    memset(digest_, 0, sizeof(digest_));
  }

  void add(const uint8_t *data, size_t len) {
    if (!data)
      return;
    length_ += len;
    absorb(data, len);
  }

  void add(const char *str) {
    if (str)
      add(reinterpret_cast<const uint8_t *>(str), strlen(str));
  }

  void calculate() {
    // 0x80, then zeros up to 56 bytes into the block, then the message length
    // in bits, little endian. length_ is frozen first: the padding must not
    // count towards it.
    const uint64_t bits = length_ * 8u;
    const uint8_t one = 0x80;
    absorb(&one, 1);
    static const uint8_t zeros[BLOCK] = {0};
    const size_t pad = (buffered_ <= 56) ? (56 - buffered_) : (56 + BLOCK - buffered_);
    if (pad > 0)
      absorb(zeros, pad);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i)
      tail[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu);
    absorb(tail, sizeof(tail));
    for (int i = 0; i < 4; ++i) {
      digest_[i * 4 + 0] = static_cast<uint8_t>(state_[i] & 0xFFu);
      digest_[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 8) & 0xFFu);
      digest_[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 16) & 0xFFu);
      digest_[i * 4 + 3] = static_cast<uint8_t>((state_[i] >> 24) & 0xFFu);
    }
  }

  String toString() const {
    char hex[33];
    for (int i = 0; i < 16; i++) {
      snprintf(hex + i * 2, 3, "%02x", digest_[i]);
    }
    return String(hex);
  }

private:
  static constexpr size_t BLOCK = 64;

  static uint32_t rotl(uint32_t v, uint32_t n) { return (v << n) | (v >> (32 - n)); }

  // Buffer and hash without touching length_, so calculate() can pad.
  void absorb(const uint8_t *data, size_t len) {
    while (len > 0) {
      const size_t room = BLOCK - buffered_;
      const size_t take = len < room ? len : room;
      memcpy(buffer_ + buffered_, data, take);
      buffered_ += take;
      data += take;
      len -= take;
      if (buffered_ == BLOCK) {
        transform(buffer_);
        buffered_ = 0;
      }
    }
  }

  void transform(const uint8_t block[BLOCK]) {
    static const uint32_t K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static const uint32_t R[64] = {
     7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
     7, 12, 17, 22,  5,  9, 14, 20,  5,  9, 14, 20,
     5,  9, 14, 20,  5,  9, 14, 20,  4, 11, 16, 23,
     4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
     6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,
     6, 10, 15, 21,
    };

    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
      m[i] = static_cast<uint32_t>(block[i * 4 + 0]) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (uint32_t i = 0; i < 64; ++i) {
      uint32_t f, g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const uint32_t tmp = d;
      d = c;
      c = b;
      b = b + rotl(f + a + K[i] + m[g], R[i]);
      a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  uint32_t state_[4]{};
  uint64_t length_ = 0;
  uint8_t buffer_[BLOCK]{};
  size_t buffered_ = 0;
  uint8_t digest_[16]{};
};
