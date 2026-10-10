#!/usr/bin/env bash
# Sign the built images and bundle them in the Android app as assets, so a phone can
# bring up a blank board and cycle the loader without a download:
#   <cpu>-main.wlpkg        bootstrap build (loader code as Main), target main
#   <cpu>-request-loader.wlpkg  zero-length Main manifest: a running Main reboots into the loader
#   <cpu>-loader.wlpkg      loader, target loader (taken by the bootstrap build)
#   <cpu>-stage2-4mb.wlpkg  Stage2 (and -8mb), target stage2 (taken by the bootstrap build)
#
#   scripts/pack-android-assets.sh [KEY.pem]     # default target/dev-key.pem (created if missing)
# VERSION overrides the monotonic version (default yymmddHHMM). Release builds must pass a
# real signing key; the generated dev key is only for bench testing.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY="${1:-$ROOT/target/dev-key.pem}"
VERSION="${VERSION:-$(date +%y%m%d%H%M)}"
OUT="${OUT_DIR:-$ROOT/android/app/src/main/assets/packages}"
SIGN="python3 $ROOT/tools/sign.py"
[[ -f "$KEY" ]] || { $SIGN keygen "$KEY"; echo "generated dev key $KEY"; }
mkdir -p "$OUT"; rm -f "$OUT"/*.wlpkg
for dir in "$ROOT"/target/out/*/; do
    cpu="$(basename "$dir")"
    [[ -f "$dir/loader.bin" ]] || continue
    $SIGN sign --key "$KEY" --cpu "$cpu" --version "$VERSION" --target main   --image "$dir/bootstrap.bin" --out "$OUT/$cpu-main.wlpkg"
    $SIGN sign --key "$KEY" --cpu "$cpu" --version "$VERSION" --request-loader --out "$OUT/$cpu-request-loader.wlpkg"
    $SIGN sign --key "$KEY" --cpu "$cpu" --version "$VERSION" --target loader --image "$dir/loader.bin"    --out "$OUT/$cpu-loader.wlpkg"
    for size in 4mb 8mb; do
        [[ -f "$dir/stage2-$size.bin" ]] &&
        $SIGN sign --key "$KEY" --cpu "$cpu" --version "$VERSION" --target stage2 --image "$dir/stage2-$size.bin" --out "$OUT/$cpu-stage2-$size.wlpkg"
    done
done
ls -l "$OUT"
