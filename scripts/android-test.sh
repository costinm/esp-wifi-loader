#!/usr/bin/env bash
# Drive the flasher app on connected phones over adb (see debugging.md).
#
#   scripts/android-test.sh devices                 # model / Android version of every device
#   scripts/android-test.sh install  [SERIAL...]    # install the debug APK (grants Wi-Fi permission)
#   scripts/android-test.sh run ACTION [SERIAL...]  # hotspot | hotspot-wpa2 | rescue | stop
#   scripts/android-test.sh log [SERIAL]            # app log + the framework's hotspot lines
#
# No SERIAL = every attached device. Uses adb from PATH, else the DMesh environment.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="${APK:-$ROOT/target/android-build/app/outputs/apk/debug/app-debug.apk}"
PKG=com.github.costinm.espwifiloader
command -v adb >/dev/null 2>&1 || { [[ -f "$ROOT/env.sh" ]] && . "$ROOT/env.sh" >/dev/null 2>&1 || true; }
command -v adb >/dev/null 2>&1 || [[ ! -f /ws/dmesh/env.sh ]] || . /ws/dmesh/env.sh >/dev/null 2>&1
command -v adb >/dev/null 2>&1 || { echo "adb not found" >&2; exit 1; }

serials() { adb devices | awk 'NR>1 && $2=="device"{print $1}'; }
targets() { if [[ $# -gt 0 ]]; then printf '%s\n' "$@"; else serials; fi; }
prop() { adb -s "$1" shell getprop "$2" | tr -d '\r'; }

cmd="${1:-devices}"; shift || true
case "$cmd" in
devices)
    for d in $(serials); do
        printf '%s\t%s %s\tAndroid %s (sdk %s, full %s)\tpatch %s\n' "$d" "$(prop "$d" ro.product.manufacturer)" \
            "$(prop "$d" ro.product.model)" "$(prop "$d" ro.build.version.release)" "$(prop "$d" ro.build.version.sdk)" \
            "$(prop "$d" ro.build.version.sdk_full)" "$(prop "$d" ro.build.version.security_patch)"
    done ;;
install)
    [[ -f "$APK" ]] || { echo "build first: cd android && ./gradlew :app:assembleDebug" >&2; exit 1; }
    for d in $(targets "$@"); do
        echo "== $d"; adb -s "$d" install -r "$APK" | tail -1
        for perm in NEARBY_WIFI_DEVICES POST_NOTIFICATIONS; do adb -s "$d" shell pm grant $PKG android.permission.$perm || true; done
    done ;;
run)
    action="${1:?action: hotspot|hotspot-wpa2|rescue|stop}"; shift
    for d in $(targets "$@"); do
        echo "== $d ($(prop "$d" ro.product.model), Android $(prop "$d" ro.build.version.release))"
        adb -s "$d" shell input keyevent KEYCODE_WAKEUP
        adb -s "$d" shell am force-stop $PKG
        sleep 1; adb -s "$d" logcat -c
        adb -s "$d" shell am start -S -n $PKG/.MainActivity --es action "$action" >/dev/null
        sleep "${WAIT:-8}"
        adb -s "$d" logcat -d -s wl-flasher:I | grep -v "^---" | cut -c1-200
    done ;;
log)
    d="${1:-$(serials | head -1)}"
    adb -s "$d" logcat -d | grep -i "wl-flasher\|lohs\|LocalOnly\|SoftApManager\|WifiNetworkSpecifier\|UiRequestManager" | tail -${LINES_MAX:-40} | cut -c1-240 ;;
*)  sed -n '2,10p' "$0"; exit 2 ;;
esac
