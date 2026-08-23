package org.explorink.simulator;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the ExplorInk firmware on a phone.
 *
 * This is not the companion app. org.explorink.gpsbridge talks to a reader over
 * BLE; this package runs the reader's own firmware, compiled for arm64 with the
 * simulator's SDL2 HAL in place of lib/hal.
 *
 * Deliberately almost empty. Everything the firmware needs is resolved on the
 * native side: the simulated SD card comes from
 * SDL_AndroidGetInternalStoragePath() inside HalStorage, not from an environment
 * variable set here. Java cannot set one before super.onCreate() anyway --
 * that is where SDLActivity loads the native libraries, so any native call
 * ahead of it throws UnsatisfiedLinkError.
 */
public class SimulatorActivity extends SDLActivity {

    /**
     * libSDL2.so is built by tools/android/fetch_sdl2.sh; libmain.so is the
     * firmware itself, cross-compiled by PlatformIO. Both are prebuilt and
     * packaged from jniLibs -- gradle compiles no native code here.
     */
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
