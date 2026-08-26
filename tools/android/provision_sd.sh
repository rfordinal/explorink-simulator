#!/usr/bin/env bash
# Fill a phone's simulated SD card: map tiles and a persisted position.
#
# The firmware reads its card from the app's private files directory
# (HalStorage.cpp, SDL_AndroidGetInternalStoragePath). `adb push` cannot write
# there, and /data/local/tmp is not traversable by an app uid, so the tree goes
# in as a tar stream through `run-as`, which needs a debuggable build.
#
#   tools/android/provision_sd.sh <cdn-dir> [serial]
#
# <cdn-dir> is a tile mirror with base/ and points/ subdirectories -- the parent
# repo's mapbuilder/cdn, or a scratch root built with build_tiles.py --out.
#
# SIM_KEEP=1 merges into whatever tiles are already on the phone instead of
# replacing base/ and points/. Off by default: a merge leaves two areas on the
# card and the verification below then fails on the file count.
#
# SIM_LAT / SIM_LON override the persisted fix, in degrees:
#
#   SIM_LAT=50.074722 SIM_LON=14.502278 tools/android/provision_sd.sh <dir>
#
# Needed whenever the tiles are not Slovak: the default below is Trnava, and a
# fix outside the tile root draws an empty panel. The parent repo's reference
# views (docs/visual-refs.json) are the coordinates worth passing here.
set -euo pipefail

PKG=org.explorink.simulator
CDN="${1:?usage: provision_sd.sh <cdn-dir> [serial]}"
SERIAL="${2:-${ANDROID_SERIAL:-}}"
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

[ -d "$CDN/base" ] || { echo "no $CDN/base" >&2; exit 1; }

# Trnava by default. Deliberately not the maintainer's own area: a rendered map
# identifies a location precisely, and these screenshots leave the phone. That
# reasoning applies to whatever SIM_LAT/SIM_LON are set to as well -- picking a
# fix is picking what a screenshot discloses.
LAT_E7=${SIM_LAT:+$(printf '%.0f' "$(echo "${SIM_LAT} * 10000000" | bc -l)")}
LON_E7=${SIM_LON:+$(printf '%.0f' "$(echo "${SIM_LON} * 10000000" | bc -l)")}
LAT_E7=${LAT_E7:-483770000}
LON_E7=${LON_E7:-175880000}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/.crosspoint"
cat > "$tmp/.crosspoint/settings.json" <<JSON
{
  "mapHasLastFix": true,
  "mapLastLatE7": $LAT_E7,
  "mapLastLonE7": $LON_E7,
  "mapLastHeading": 0
}
JSON

# The extracting tar tries to restore ownership, which an app uid may not do, so
# it prints "chown ... Operation not permitted" per file and exits non-zero
# while still writing every file correctly. Seen on Android 9, whose toybox tar
# has neither --no-same-owner nor --numeric-owner. Dropping all of stderr would
# hide real failures, so only that one message is filtered and anything else is
# shown.
#
# $1 is the destination under the app's files directory.
# The filtering is done on a captured string, not in a pipeline: `set -o
# pipefail` reports a grep that matched nothing as a failure, so filtering
# inline killed the script exactly when there was nothing to report.
extract() {
  local dest="$1" out
  out=$("${ADB[@]}" shell "run-as $PKG sh -c 'mkdir -p $dest && tar -xf - -C $dest' 2>&1" \
        | grep -vE "Operation not permitted|^tar: chown" || true)
  [ -z "$out" ] || printf '%s\n' "$out"
  return 0
}

# Archived from inside $CDN, so the member names are already "base/..." and
# "points/...". An earlier version renamed a "cdn" prefix with tar's
# --transform, which broke on any directory name containing a sed metacharacter
# and needed GNU tar; this needs neither.
# base/ and points/ are REPLACED, not merged into. Extracting on top of an
# earlier tile set leaves the phone holding the union of two areas, and then
# this script's own verification fails on the file count -- which is what
# happened the first time a second area was pushed. Only those two trees go:
# the firmware writes its own files under trailink/ while running (power.csv,
# from PowerTelemetry) and those are not ours to delete. SIM_KEEP=1 skips it.
if [ -z "${SIM_KEEP:-}" ]; then
  echo "clearing base/ and points/ on the phone"
  "${ADB[@]}" shell "run-as $PKG sh -c 'rm -rf files/fs_/trailink/base files/fs_/trailink/points'" >/dev/null
fi

echo "pushing tiles from $CDN"
tar -cf - -C "$CDN" base points | extract files/fs_/trailink

echo "pushing settings"
tar -cf - -C "$tmp" .crosspoint | extract files/fs_

# Two numbers, not one. A count alone passes on a truncated file, and truncation
# is exactly what a half-finished transfer produces. Scoped to base/ and
# points/, because the firmware writes its own files under trailink/ while
# running (power.csv, from PowerTelemetry).
local_files=$(find "$CDN/base" "$CDN/points" -type f | wc -l)
local_bytes=$(find "$CDN/base" "$CDN/points" -type f -printf '%s\n' | awk '{s+=$1} END {print s+0}')
remote=$("${ADB[@]}" shell "run-as $PKG sh -c '
  cd files/fs_/trailink || exit 1
  find base points -type f | wc -l
  find base points -type f -exec stat -c %s {} + | awk \"{s+=\\\$1} END {print s+0}\"
'" | tr -d '\r')
remote_files=$(echo "$remote" | sed -n 1p)
remote_bytes=$(echo "$remote" | sed -n 2p)

echo "files: $local_files local, $remote_files on the phone"
echo "bytes: $local_bytes local, $remote_bytes on the phone"
[ "$local_files" = "$remote_files" ] || { echo "FAILED: file count differs" >&2; exit 1; }
[ "$local_bytes" = "$remote_bytes" ] || { echo "FAILED: total size differs" >&2; exit 1; }
"${ADB[@]}" shell "run-as $PKG sh -c 'cat files/fs_/.crosspoint/settings.json'" >/dev/null \
  || { echo "FAILED: settings.json missing" >&2; exit 1; }
echo "ok"
