#!/usr/bin/env bash
# Fetch SDL2 at a pinned version, build libSDL2.so for arm64-v8a, and install
# it plus SDL's Java classes into the gradle project.
#
# SDL is not vendored into this repo. It is fetched into a cache outside every
# checkout, so a worktree stays small and several worktrees share one copy.
set -euo pipefail

# 2.32 is the last SDL2 series. The Linux simulator runs against Debian's
# 2.30.0, so the two are not the same build -- the Android side gets the
# newer one because that is where SDL's Android fixes landed.
SDL_TAG="release-2.32.10"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/explorink/sdl2"
SRC="$CACHE/SDL-${SDL_TAG#release-}"
BUILD="$CACHE/build-android"
ABI="arm64-v8a"
# Must match minSdk in android/app/build.gradle.kts and the firmware's
# --target=aarch64-linux-androidNN.
API=24

NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.3.13750724}"
[ -x "$NDK/ndk-build" ] || { echo "no NDK at $NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jni_libs="$repo_root/android/app/src/main/jniLibs/$ABI"
java_sdl="$repo_root/android/app/src/main/java-sdl"

# Cloned to a scratch path and moved into place, so an interrupted clone cannot
# leave a half-tree that the guard below then happily skips. Checking for .git
# rather than the directory catches an older partial clone too.
if [ ! -d "$SRC/.git" ]; then
  echo "fetching SDL2 $SDL_TAG"
  rm -rf "$SRC" "$SRC.partial"
  mkdir -p "$CACHE"
  git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$SRC.partial"
  mv "$SRC.partial" "$SRC"
fi
[ -f "$SRC/include/SDL.h" ] || { echo "SDL2 source at $SRC has no include/SDL.h" >&2; exit 1; }
[ -d "$SRC/android-project/app/src/main/java/org" ] \
  || { echo "SDL2 source at $SRC has no Android Java sources" >&2; exit 1; }

# Same reasoning as the clone: the guard is the finished artefact, and a failed
# ndk-build leaves no libSDL2.so, so a retry starts over.
if [ ! -f "$BUILD/libs/$ABI/libSDL2.so" ]; then
  echo "building libSDL2.so for $ABI"
  rm -rf "$BUILD"
  mkdir -p "$BUILD/jni"
  ln -sfn "$SRC" "$BUILD/jni/SDL"
  # Android 15 and newer run on devices with 16 kB memory pages, and refuse a
  # .so whose LOAD segments are aligned to the old 4 kB. Without this the app
  # still starts, but every launch opens a system "not compatible with 16 kB
  # mode" dialog over it.
  printf 'APP_ABI := %s\nAPP_PLATFORM := android-%s\nAPP_OPTIM := release\nAPP_SUPPORT_FLEXIBLE_PAGE_SIZES := true\n' \
    "$ABI" "$API" > "$BUILD/jni/Application.mk"
  printf 'include $(call all-subdir-makefiles)\n' > "$BUILD/jni/Android.mk"
  "$NDK/ndk-build" NDK_PROJECT_PATH="$BUILD" \
    NDK_APPLICATION_MK="$BUILD/jni/Application.mk" -j"$(nproc)"
fi

mkdir -p "$jni_libs" "$java_sdl"
cp "$BUILD/libs/$ABI/libSDL2.so" "$jni_libs/"
# SDLActivity and friends. MIT, same licence as this repo.
cp -r "$SRC/android-project/app/src/main/java/org" "$java_sdl/"

echo "installed:"
echo "  $jni_libs/libSDL2.so"
echo "  $java_sdl/org/libsdl/app/ ($(find "$java_sdl" -name '*.java' | wc -l) files)"
