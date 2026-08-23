#pragma once

// __ANDROID__ must come first: bionic defines __linux__ too, so an Android
// build would otherwise take the OpenSSL path and fail to find openssl/md5.h.
#if defined(__ANDROID__)
#include "MD5Builder_android.h"
#elif defined(__APPLE__)
#include "MD5Builder_mac.h"
#elif defined(__linux__)
#include "MD5Builder_linux.h"
#else
#error "Unsupported host OS for simulator MD5Builder"
#endif
