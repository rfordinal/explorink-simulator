#!/usr/bin/env bash
# Set the simulator's environment variables on a phone.
#
#   tools/android/set_env.sh KEY=VALUE [KEY=VALUE ...] [--serial S]
#   tools/android/set_env.sh --clear [--serial S]
#
# Android gives Java no way to hand an environment variable to the native code
# (SDLActivity loads the libraries inside onCreate), so the native side reads
# them from a file instead -- src/SimulatorAndroidEnv.cpp. This writes it.
#
# The activity has to be restarted afterwards; that is done here.
set -euo pipefail

PKG=org.explorink.simulator
SERIAL="${ANDROID_SERIAL:-}"
CLEAR=0
PAIRS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --clear) CLEAR=1; shift ;;
    *=*) PAIRS+=("$1"); shift ;;
    *) echo "not a KEY=VALUE pair: $1" >&2; exit 1 ;;
  esac
done

ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

if [ "$CLEAR" = 1 ]; then
  "${ADB[@]}" shell "run-as $PKG sh -c 'rm -f files/sim-env'"
  echo "cleared"
else
  [ ${#PAIRS[@]} -gt 0 ] || { echo "nothing to set" >&2; exit 1; }
  # Piped in, not written with a heredoc on the device: the shell there wants a
  # temporary file for one and an app uid may not create it.
  printf '%s\n' "${PAIRS[@]}" \
    | "${ADB[@]}" shell "run-as $PKG sh -c 'mkdir -p files && cat > files/sim-env'"
  "${ADB[@]}" shell "run-as $PKG sh -c 'cat files/sim-env'"
fi

"${ADB[@]}" shell am force-stop "$PKG"
"${ADB[@]}" shell am start -n "$PKG/.SimulatorActivity" >/dev/null
