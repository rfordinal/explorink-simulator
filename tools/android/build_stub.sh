#!/usr/bin/env bash
# Build the smoke-test libmain.so. Run tools/android/fetch_sdl2.sh first.
set -euo pipefail

SDL_TAG="release-2.32.10"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/explorink/sdl2"
SRC="$CACHE/SDL-${SDL_TAG#release-}"
ABI="arm64-v8a"
API=24
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.3.13750724}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jni_libs="$repo_root/android/app/src/main/jniLibs/$ABI"

[ -f "$jni_libs/libSDL2.so" ] || { echo "run tools/android/fetch_sdl2.sh first" >&2; exit 1; }
[ -f "$SRC/include/SDL.h" ] || { echo "no SDL2 headers at $SRC -- run tools/android/fetch_sdl2.sh" >&2; exit 1; }
[ -f "$repo_root/tools/android/stub_main.c" ] || { echo "missing $repo_root/tools/android/stub_main.c" >&2; exit 1; }
[ -x "$CC" ] || { echo "no NDK compiler at $CC (set ANDROID_NDK_HOME)" >&2; exit 1; }

# -z max-page-size=16384: Android 15+ rejects 4 kB-aligned LOAD segments.
"$CC" -shared -fPIC -O2 \
  -Wl,-z,max-page-size=16384 \
  -I"$SRC/include" \
  -o "$jni_libs/libmain.so" \
  "$repo_root/tools/android/stub_main.c" \
  -L"$jni_libs" -lSDL2

echo "built $jni_libs/libmain.so"
