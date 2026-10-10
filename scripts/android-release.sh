#!/usr/bin/env bash
# Build the Android app and leave the APK at OUT (default target/release-apk/esp-wifi-loader.apk).
#   scripts/android-release.sh
# With ANDROID_KEYSTORE_B64 + ANDROID_KEYSTORE_PASSWORD (or ANDROID_KEYSTORE=FILE + the password; see
# scripts/android-keystore.sh) it builds and signs the release APK. Without them (a pull request, a fork) it builds
# the debug APK, which Android installs too. Run the unit tests separately: scripts/test-host.sh android.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/target/release-apk/esp-wifi-loader.apk}"
mkdir -p "$(dirname "$OUT")"
build="${GRADLE_BUILD_DIR:-$ROOT/target/android-build}"
export GRADLE_BUILD_DIR="$build"
if [[ -n ${ANDROID_KEYSTORE_B64:-} ]]; then
    ks="$(mktemp)"; trap 'rm -f "$ks"' EXIT
    printf '%s' "$ANDROID_KEYSTORE_B64" | base64 -d > "$ks"
    export ANDROID_KEYSTORE="$ks"
fi
if [[ -n ${ANDROID_KEYSTORE:-} ]]; then
    : "${ANDROID_KEYSTORE_PASSWORD:?ANDROID_KEYSTORE_PASSWORD is required with a keystore}"
    (cd "$ROOT/android" && ./gradlew ${GRADLE_ARGS:-} :app:assembleRelease)
    cp "$build"/app/outputs/apk/release/*.apk "$OUT"
else
    echo "no keystore: building the debug APK" >&2
    (cd "$ROOT/android" && ./gradlew ${GRADLE_ARGS:-} :app:assembleDebug)
    cp "$build"/app/outputs/apk/debug/*.apk "$OUT"
fi
ls -l "$OUT"
