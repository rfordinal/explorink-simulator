plugins {
    id("com.android.application")
}

android {
    namespace = "org.explorink.simulator"
    compileSdk = 36

    defaultConfig {
        // Deliberately not org.explorink.gpsbridge. That is the companion app,
        // which talks to a device over BLE. This one *is* the device: it runs
        // the firmware. Both install side by side and never collide.
        applicationId = "org.explorink.simulator"
        // android-24 is what libSDL2.so and the firmware objects are built
        // against (tools/android/fetch_sdl2.sh, APP_PLATFORM).
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        ndk {
            // The firmware is cross-compiled for one ABI only.
            abiFilters += "arm64-v8a"
        }
    }

    // No externalNativeBuild: gradle never compiles C++ here. The firmware is
    // cross-compiled by PlatformIO and libSDL2.so by ndk-build, and both land
    // in jniLibs as prebuilt .so files. Gradle only packages them.
    sourceSets {
        named("main") {
            // SDL's own Java classes are copied in by
            // tools/android/fetch_sdl2.sh rather than vendored into this repo.
            java.srcDirs("src/main/java", "src/main/java-sdl")
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    buildTypes {
        named("debug") {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
