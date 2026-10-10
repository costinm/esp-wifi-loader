#!/usr/bin/env bash
# Copy signed packages (scripts/pack-android-assets.sh output, or any *.wlpkg) into the app's files
# directory on a phone, so the flasher uses them instead of the ones bundled in the APK.
#
#   scripts/android-push-packages.sh [-s SERIAL] [DIR]   # DIR default android/app/src/main/assets/packages
#
# Tries the app's external files dir (/sdcard/Android/data/<pkg>/files/packages) first and falls
# back to the internal one through run-as (debug builds). Packages found there win over assets.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG=com.github.costinm.espwifiloader
SERIAL=()
if [[ "${1:-}" == -s ]]; then SERIAL=(-s "$2"); shift 2; fi
DIR="${1:-$ROOT/android/app/src/main/assets/packages}"
command -v adb >/dev/null 2>&1 || . /ws/dmesh/env.sh >/dev/null 2>&1
shopt -s nullglob
files=("$DIR"/*.wlpkg)
[[ ${#files[@]} -gt 0 ]] || { echo "no .wlpkg in $DIR" >&2; exit 1; }
EXT=/sdcard/Android/data/$PKG/files/packages
if adb "${SERIAL[@]}" shell "mkdir -p $EXT" 2>/dev/null && adb "${SERIAL[@]}" push "${files[@]}" "$EXT/" >/dev/null 2>&1; then
    echo "pushed ${#files[@]} packages to $EXT"; exit 0
fi
echo "external files dir not writable; using the internal one (run-as)" >&2
adb "${SERIAL[@]}" shell "run-as $PKG mkdir -p files/packages"
for f in "${files[@]}"; do
    adb "${SERIAL[@]}" exec-out "run-as $PKG sh -c 'cat > files/packages/$(basename "$f")'" < "$f"
done
echo "pushed ${#files[@]} packages to the internal files/packages"
