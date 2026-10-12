#!/usr/bin/env bash
# Create the keystore that signs the Android app (once), from a PEM private key.
#
#   scripts/android-keystore.sh [--key KEY.pem] [OUT_DIR]
#
# Without --key a new P-256 key is generated: recommended, because the app's signing certificate is its identity for
# every future update (a phone refuses an update signed by another certificate) and is unrelated to the firmware
# signing keys. Any PEM key can be used, the firmware key included (APK signature schemes v2+ accept ECDSA P-256),
# but then one leaked secret compromises both.
# Writes OUT_DIR/android.p12 (alias "flasher", self-signed certificate valid for 100 years) and android.pass, and
# prints the commands that store them as GitHub secrets. Keep the files; never commit them.
set -euo pipefail
KEY=""
if [[ ${1:-} == --key ]]; then KEY="$2"; shift 2; fi
OUT="${1:-$HOME/.ssh/esp-wifi-loader}"
mkdir -p "$OUT"; chmod 700 "$OUT"   # under ~/.ssh: keep secrets out of config directories
[[ -n $KEY ]] || { KEY="$OUT/android-signing-key.pem"; [[ -f $KEY ]] || openssl ecparam -name prime256v1 -genkey -noout -out "$KEY"; }
chmod 600 "$KEY"
[[ -f "$OUT/android.p12" ]] && { echo "$OUT/android.p12 exists: keep it, the certificate must not change" >&2; exit 1; }
openssl rand -hex 16 > "$OUT/android.pass"; chmod 600 "$OUT/android.pass"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
openssl req -new -x509 -key "$KEY" -sha256 -days 36500 -subj "/CN=esp-wifi-loader flasher" -out "$tmp/cert.pem"
openssl pkcs12 -export -inkey "$KEY" -in "$tmp/cert.pem" -name flasher -passout "file:$OUT/android.pass" -out "$OUT/android.p12"
chmod 600 "$OUT/android.p12"
echo "wrote $OUT/android.p12 and $OUT/android.pass; certificate SHA-256:"
openssl x509 -in "$tmp/cert.pem" -noout -fingerprint -sha256
cat <<MSG
Store them (once per repository):
  base64 -w0 $OUT/android.p12 | gh secret set ANDROID_KEYSTORE_B64 --repo OWNER/REPO
  gh secret set ANDROID_KEYSTORE_PASSWORD --repo OWNER/REPO < $OUT/android.pass
MSG
