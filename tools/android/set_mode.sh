#!/usr/bin/env bash
# Set the simulator's panel size mode on a phone and restart the activity.
#
#   tools/android/set_mode.sh FIT|ONE_TO_ONE|REAL [serial]
#
# The mode is also switchable by hand from the app's own menu; this exists so
# several phones can be put in the same mode at once for comparison.
set -euo pipefail

PKG=org.explorink.simulator
MODE="${1:?usage: set_mode.sh FIT|ONE_TO_ONE|REAL [serial]}"
SERIAL="${2:-${ANDROID_SERIAL:-}}"
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

case "$MODE" in
  FIT|ONE_TO_ONE|REAL) ;;
  *) echo "unknown mode $MODE (FIT, ONE_TO_ONE, REAL)" >&2; exit 1 ;;
esac

# SharedPreferences is read in onCreate, so the activity has to be restarted
# for the change to take. Written through run-as, which needs a debuggable
# build, the same way provision_sd.sh gets at app-private storage.
#
# Piped in rather than written with a heredoc on the device: the shell there
# wants a temporary file for one, and an app uid may not create it
# ("can't create temporary file /data/local/...: Permission denied").
printf '%s' \
  "<?xml version='1.0' encoding='utf-8' standalone='yes' ?><map><string name=\"display_mode\">$MODE</string></map>" \
  | "${ADB[@]}" shell "run-as $PKG sh -c 'mkdir -p shared_prefs && cat > shared_prefs/simulator.xml'"
"${ADB[@]}" shell am force-stop "$PKG"
"${ADB[@]}" shell am start -n "$PKG/.SimulatorActivity" >/dev/null
echo "$MODE"
