# The simulator as an Android app

The firmware compiled for arm64 and hosted in an `SDLActivity`, so the UI runs
on a phone with no reader hardware in reach.

**Not the companion app.** `org.explorink.gpsbridge` (repo
`rfordinal/explorink-android`) talks to a reader over BLE. This is
`org.explorink.simulator`: it *is* the reader. Two package ids, both install
side by side, neither replaces the other.

Not an emulator either. No ESP32 `.bin` is loaded -- the APK carries the
firmware compiled as native arm64 code, so new firmware means a new APK.

## Layout

```
android/                     gradle project, AGP 8.13.2 / gradle 8.14.3 / JDK 17
  app/src/main/java/org/explorink/simulator/SimulatorActivity.java
  app/src/main/java-sdl/     SDL's own Java classes    (gitignored, copied in)
  app/src/main/jniLibs/      libSDL2.so + libmain.so   (gitignored, prebuilt)
tools/android/fetch_sdl2.sh  fetch SDL2, build libSDL2.so, install both above
tools/android/build_stub.sh  smoke-test libmain.so (tools/android/stub_main.c)
```

**Gradle compiles no C++.** The firmware is cross-compiled by PlatformIO and
`libSDL2.so` by `ndk-build`; both are prebuilt `.so` files that gradle only
packages. That keeps one source of truth for the file list -- the firmware's
`build_src_filter` and `lib_deps` -- instead of a CMake copy of it.

SDL is not vendored. `fetch_sdl2.sh` clones a pinned tag into
`~/.cache/explorink/sdl2/`, outside every checkout, so worktrees stay small and
share one copy.

## Build and run

```bash
tools/android/fetch_sdl2.sh          # once: SDL2 source + libSDL2.so
tools/android/build_stub.sh          # smoke test, until the firmware links
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n org.explorink.simulator/.SimulatorActivity
```

`android/local.properties` needs `sdk.dir=<Android SDK>`; it is gitignored.

## Verified 2026-08-23

Samsung SM-S928B, Android 16 (API 36), arm64-v8a, NDK r27.3.13750724,
SDL2 2.32.10. The smoke test installs, launches and draws: white screen, one
dark stripe scrolling top to bottom, fullscreen at 720x1560. Confirmed on the
phone by eye and by `adb exec-out screencap`.

What that pass establishes:

- gradle, `jniLibs`, the manifest, `SDLActivity`, `adb install` and launch all
  work as a chain.
- **The entry point needs no source change.** `src/simulator_main.cpp` includes
  `<SDL.h>` before defining `main`, and SDL's macro renames it to `SDL_main` on
  Android too. logcat: `Running main function SDL_main from library
  .../libmain.so`.
- Renderer size is the phone's full screen, not the panel's 480x800. Scaling the
  panel into it is still open.

## Two Android facts that cost time

### 16 kB page alignment is mandatory, and the warning lies about being current

Android 15 and newer run on 16 kB memory pages and refuse a `.so` whose ELF
`LOAD` segments are aligned to the old 4 kB. The app still starts, but the
system puts a dialog over it: *"not compatible with 16 kB mode ... segment LOAD
is not aligned"*.

Two different alignments have to be right, and they are fixed in two places:

- **ELF segments.** `APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true` in
  `Application.mk` for SDL (`tools/android/fetch_sdl2.sh`), and
  `-Wl,-z,max-page-size=16384` on our own link (`tools/android/build_stub.sh`).
- **Zip entries.** AGP 8.13.2 already page-aligns them; nothing to do, but
  check it rather than assume.

**The dialog is not a live indicator.** It persists on screen from the install
that caused it, so it is still there after a fixed build is installed and
launched. Do not judge the fix by whether it appears. Check the APK:

```bash
unzip -p app-debug.apk lib/arm64-v8a/libmain.so > /tmp/x.so
llvm-readelf -l /tmp/x.so | grep LOAD          # align must be 0x4000
zipalign -c -P 16 -v 4 app-debug.apk | tail -1 # "Verification successful"
```

### SDL decides fullscreen, not the manifest

`SDL_WINDOW_FULLSCREEN` in the `SDL_CreateWindow` call is what hides the status
bar on Android. Changing the activity's theme away from
`Theme.NoTitleBar.Fullscreen` does nothing while that flag is set. Verified
both ways on the phone.

It matters beyond looks: a white screen with a moving dark stripe and no system
bars reads as a dead phone, not as a running app. The stub now asks for a plain
window (`tools/android/stub_main.c`), and the panel will be letterboxed inside
it rather than stretched.

The requested window size is ignored on Android -- the window is whatever the
activity got, 720x1560 here -- so the 480x800 in that call is documentation, not
a request.

### Java cannot set an environment variable before the native code loads

The obvious way to point the firmware at its simulated SD card is
`setenv("CROSSPOINT_SIM_SD", ...)` from `onCreate`. It cannot work:
`SDLActivity.onCreate` is where the native libraries are loaded, so any native
call ahead of `super.onCreate()` throws `UnsatisfiedLinkError`, and after it the
SDL thread may already be running.

So the storage root has to be resolved natively, from
`SDL_AndroidGetInternalStoragePath()` inside `HalStorage`
(`src/HalStorage.cpp:20-25` picks up `CROSSPOINT_SIM_SD` today). Not written
yet.

`SimulatorActivity` is deliberately almost empty for the same reason: anything
it could do, the native side can do without an ordering problem.

## Open

- **Linking the firmware** into `libmain.so`. Compiling is proven (208 of 208
  translation units for `aarch64-linux-android24`, see the firmware repo's
  `docs/simulator-android.md`); linking is not. Expect undefined symbols for
  whatever the HAL assumes a host provides. One is known: OpenSSL, needed only
  by `src/MD5Builder_linux.h:12`, which a bundled MD5 would remove.
- **The recorded main thread is the wrong one.**
  `src/HalDisplay.cpp:66` captures it in a global initialiser, which on Android
  runs when `System.loadLibrary` maps `libmain.so` -- the activity's UI thread.
  `SDL_main` runs on another. Confirmed in this run: `onCreate` logged from tid
  23021, `SDL_main` from tid 23127. So the `displayBuffer` check at
  `src/HalDisplay.cpp:389` never matches and it never presents. Frames still
  appear, one loop iteration late, via `simulator_main.cpp`'s own
  `presentIfNeeded()`. Fix: capture the id at the top of `main()`. That is a
  latent bug anywhere the two threads differ, so it belongs upstream.
- **Panel geometry.** 480x800 panel into a 720x1560 window.
- **Buttons.** On-screen, in Java, injecting key events -- so `HalGPIO` stays
  untouched and the fork's diff does not grow.
- **The simulated SD card**: tiles and settings pushed into app-private storage.
