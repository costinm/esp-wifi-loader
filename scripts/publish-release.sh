#!/usr/bin/env bash
# Sign the built firmware and collect everything the rolling release `latest` carries into REL (default target/rel).
#   WL_SIGNING_KEY=<PEM text>  scripts/publish-release.sh            # CI: the key from a secret
#   scripts/publish-release.sh KEY.pem                               # locally
# Needs target/out/<cpu>/*.bin (scripts/build.sh) and optionally APK=<file> (scripts/android-release.sh).
# The key must be the one whose public half is keys/loader.pub: it is embedded in every Stage2 built here, and a
# device only takes a loader or Stage2 signed with it. Prints version=<yymmddHHMM> (also written to $GITHUB_ENV).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="${REL:-$ROOT/target/rel}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
key="${1:-}"
if [[ -z $key ]]; then
    [[ -n ${WL_SIGNING_KEY:-} ]] || { echo "::error::secret WL_SIGNING_KEY is not set (docs/building.md)"; exit 1; }
    key="$tmp/loader-key.pem"; install -m 600 /dev/null "$key"; printf '%s\n' "$WL_SIGNING_KEY" > "$key"
fi
[[ "$(python3 "$ROOT/tools/sign.py" pubkey "$key")" == "$(cat "$ROOT/keys/loader.pub")" ]] ||
    { echo "::error::the signing key does not match keys/loader.pub"; exit 1; }
version="${VERSION:-$(date -u +%y%m%d%H%M)}"      # monotonic: devices refuse older versions
rm -rf "$REL"; mkdir -p "$REL"
VERSION="$version" OUT_DIR="$REL" "$ROOT/scripts/pack-android-assets.sh" "$key" >/dev/null
# <chip>.wlpkg is the Main package of the repository: here the bootstrap build
for f in "$REL"/*-main.wlpkg; do cp "$f" "$REL/$(basename "${f%-main.wlpkg}").wlpkg"; done
# merged images for a first USB or web flash (not signed packages: written with espflash or from the web page)
for d in "$ROOT"/target/out/*/; do
    cpu="$(basename "$d")"
    for f in "$d"/factory-*.bin; do [[ -f $f ]] && cp "$f" "$REL/$cpu-$(basename "$f")"; done
done
[[ -z ${APK:-} ]] || cp "$APK" "$REL/esp-wifi-loader.apk"
(cd "$REL" && sha256sum * > SHA256SUMS)
echo "version=$version"
[[ -z ${GITHUB_ENV:-} ]] || echo "version=$version" >> "$GITHUB_ENV"
