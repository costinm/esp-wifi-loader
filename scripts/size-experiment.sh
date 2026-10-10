#!/usr/bin/env bash
# Loader size experiment: cumulative sdkconfig overlays from docs/size-experiment/.
#   scripts/size-experiment.sh [cpu]      # default esp32c6; results in target/size/<cpu>/
# Prints image size per stage and the options that actually took effect (an
# overlay line naming a missing option is silently dropped by ESP-IDF).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v idf.py >/dev/null 2>&1 || . "$ROOT/env.sh"
cpu="${1:-esp32c6}"; out="$ROOT/target/size/$cpu"; mkdir -p "$out"
defaults="$ROOT/loader/sdkconfig.defaults;$ROOT/loader/sdkconfig.$cpu.defaults"
stages=(${STAGES:-s0-baseline s1-no-tls s2-no-wpa3 s3-trim s4-os s5-nocrypto s6-no-nvs-enc s7-prune-mbedtls})
# BUILD=sN builds only the listed stages but still applies every earlier overlay.
all=(s0-baseline s1-no-tls s2-no-wpa3 s3-trim s4-os s5-nocrypto s6-no-nvs-enc s7-prune-mbedtls)
for st in "${all[@]}"; do
    [[ $st != s0-baseline ]] && defaults="$defaults;$ROOT/docs/size-experiment/$st.defaults"
    # SKIP="s5-nocrypto s6-no-nvs-enc" leaves those overlays out of the chain.
    [[ " ${SKIP:-} " == *" $st "* ]] && { defaults="${defaults%;*}"; continue; }
    [[ " ${stages[*]} " == *" $st "* ]] || continue
    rm -f "$out/$st.sdkconfig"
    IDF_TARGET=$cpu idf.py -C "$ROOT/loader" -B "$out/$st-build" -D SDKCONFIG="$out/$st.sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$defaults" build > "$out/$st.log" 2>&1 || { echo "$st: build FAILED, see $out/$st.log"; tail -5 "$out/$st.log"; continue; }
    size=$(stat -c%s "$out/$st-build/esp-wifi-loader.bin")
    printf '%-12s %8d bytes\n' "$st" "$size" | tee -a "$out/results.txt"
done
echo "--- options in the last stage's sdkconfig"
grep -E "^(# )?CONFIG_(MBEDTLS_TLS_ENABLED|ESP_WIFI_ENTERPRISE_SUPPORT|ESP_WIFI_ENABLE_WPA3_SAE|ESP_WIFI_GMAC_SUPPORT|LWIP_DHCPS|MBEDTLS_CERTIFICATE_BUNDLE|ESP_WIFI_ENABLE_WPA3_OWE_STA)[= ]" "$out/${stages[-1]}.sdkconfig" || true
