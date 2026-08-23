#include "SimulatorLifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#if defined(__ANDROID__)
#include <SDL.h>
#include <jni.h>

#include <string>
#endif

namespace {

constexpr const char *kWakeReasonEnv = "CROSSPOINT_SIM_WAKE_REASON";
constexpr const char *kInputScriptEnv = "CROSSPOINT_SIM_INPUT_SCRIPT";
constexpr const char *kInputScriptAfterWakeEnv =
    "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE";
constexpr const char *kScreenshotsEnv = "CROSSPOINT_SIM_SCREENSHOTS";
constexpr const char *kScreenshotsAfterWakeEnv =
    "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE";
char **gArgv = nullptr;

#if defined(__ANDROID__)
// A relaunched process on Android does not inherit the environment, so the wake
// reason cannot travel in a variable the way it does on a desktop host. It goes
// through a marker file in app-private storage instead.
std::string wakeMarkerPath() {
  const char *base = SDL_AndroidGetInternalStoragePath();
  if (!base || !*base)
    return std::string();
  return std::string(base) + "/sim-wake-reason";
}

// Android has no execvp story: SDL sets argv[0] to "app_process", so the
// desktop path replaces the app with a bare system binary that immediately
// dies -- which looked exactly like the app crashing on wake. Ask Java to
// start a fresh activity, then leave. A new process comes up because this one
// is gone by the time the intent is handled.
//
// The firmware sleeps on its own after an idle timeout, so this path runs
// whether or not anyone pressed anything.
[[noreturn]] void relaunchThroughJava() {
  const std::string marker = wakeMarkerPath();
  if (!marker.empty()) {
    if (FILE *f = std::fopen(marker.c_str(), "w")) {
      std::fputs("power", f);
      std::fclose(f);
    }
  }

  JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env && activity) {
    jclass cls = env->GetObjectClass(activity);
    // Blocks until the intent has been submitted on the UI thread, so _exit
    // below cannot beat it.
    jmethodID mid = env->GetMethodID(cls, "relaunchForWake", "()V");
    if (mid) {
      env->CallVoidMethod(activity, mid);
      if (env->ExceptionCheck())
        env->ExceptionClear();
    } else {
      std::fputs("SimulatorLifecycle: no relaunchForWake on the activity\n",
                 stderr);
      if (env->ExceptionCheck())
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
  } else {
    std::fputs("SimulatorLifecycle: no JNI env or activity for relaunch\n",
               stderr);
  }
  _exit(0);
}
#endif

void promoteAfterWakeValue(const char *target, const char *afterWake) {
  const char *value = std::getenv(afterWake);
  if (value) {
    setenv(target, value, 1);
  } else {
    unsetenv(target);
  }
  unsetenv(afterWake);
}

} // namespace

namespace SimulatorLifecycle {

void initProcessArgs(char **argv) { gArgv = argv; }

WakeReason consumeWakeReason() {
  const char *value = std::getenv(kWakeReasonEnv);
#if defined(__ANDROID__)
  // The marker file, written by relaunchThroughJava(), stands in for the
  // environment variable a desktop relaunch would have carried.
  std::string marker;
  std::string fromFile;
  if (!value) {
    marker = wakeMarkerPath();
    if (!marker.empty()) {
      if (FILE *f = std::fopen(marker.c_str(), "r")) {
        char buf[16] = {0};
        if (std::fgets(buf, sizeof(buf), f))
          fromFile = buf;
        std::fclose(f);
      }
      std::remove(marker.c_str());
    }
    if (!fromFile.empty())
      value = fromFile.c_str();
  }
#endif
  if (!value) {
    return WakeReason::None;
  }

  unsetenv(kWakeReasonEnv);
  if (std::strcmp(value, "power") == 0) {
    return WakeReason::PowerButton;
  }
  return WakeReason::None;
}

[[noreturn]] void rebootAsPowerWake() {
#if defined(__ANDROID__)
  relaunchThroughJava();
#endif
  if (!gArgv || !gArgv[0]) {
    std::fputs("SimulatorLifecycle: missing argv for reboot\n", stderr);
    _exit(1);
  }

  setenv(kWakeReasonEnv, "power", 1);
  // A deep-sleep wake is a fresh process. Do not replay the pre-sleep script,
  // which would otherwise put every relaunched process back to sleep forever.
  // Tests can provide an explicit post-wake schedule when they need to capture
  // or terminate the relaunched instance.
  promoteAfterWakeValue(kInputScriptEnv, kInputScriptAfterWakeEnv);
  promoteAfterWakeValue(kScreenshotsEnv, kScreenshotsAfterWakeEnv);
  execvp(gArgv[0], gArgv);

  std::perror("execvp");
  _exit(1);
}

} // namespace SimulatorLifecycle
