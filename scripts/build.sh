#!/usr/bin/env bash
# Build Stage2 and the loader. Uses idf.py from the PATH, else ./env.sh
# (scripts/esp-deps.sh installs it); in the container it is already set up.
#   scripts/build.sh [esp32|esp32s3|esp32c6|all] [4mb|8mb|all]
# The loader and bootstrap images use the 4 MiB app header on every board (they
# run unchanged on 8 MiB or larger parts); only Stage2 and its table differ by size.
# Output: target/out/<cpu>/{stage2-<size>,partition-table-<size>,factory-<size>,loader,bootstrap}.bin
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v idf.py >/dev/null 2>&1 || . "$ROOT/env.sh"
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found: run scripts/esp-deps.sh (or use the container)" >&2; exit 1; }
CPUS="${1:-all}"; SIZES="${2:-all}"
[[ $CPUS == all ]] && CPUS="esp32 esp32s3 esp32c6"
[[ $SIZES == all ]] && SIZES="4mb 8mb"

for cpu in $CPUS; do
    out="$ROOT/target/out/$cpu"; mkdir -p "$out"
    defaults="sdkconfig.defaults"
    [[ $cpu != esp32 ]] && defaults="sdkconfig.defaults;sdkconfig.$cpu.defaults"

    # loader (one image per CPU; independent of flash size)
    ldef="$ROOT/loader/sdkconfig.defaults;$ROOT/loader/sdkconfig.$cpu.defaults"
    rm -f "$out/loader.sdkconfig"
    IDF_TARGET=$cpu idf.py -C "$ROOT/loader" -B "$out/loader-build" \
        -D SDKCONFIG="$out/loader.sdkconfig" -D SDKCONFIG_DEFAULTS="$ldef" build
    grep -qxF 'CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' "$out/loader.sdkconfig" ||
        { echo "$cpu loader: flash driver must be DIO" >&2; exit 1; }
    cp "$out/loader-build/esp-wifi-loader.bin" "$out/loader.bin"

    # bootstrap: the same code built as Main (flashed at `main`); installs the
    # loader and Stage2 over Wi-Fi. Also one image per CPU.
    bdef="$ROOT/bootstrap/sdkconfig.defaults;$ROOT/bootstrap/sdkconfig.$cpu.defaults"
    rm -f "$out/bootstrap.sdkconfig"
    IDF_TARGET=$cpu idf.py -C "$ROOT/bootstrap" -B "$out/bootstrap-build" \
        -D SDKCONFIG="$out/bootstrap.sdkconfig" -D SDKCONFIG_DEFAULTS="$bdef" build
    cp "$out/bootstrap-build/esp-wifi-bootstrap.bin" "$out/bootstrap.bin"

    for size in $SIZES; do
        cfg="$out/stage2-$size.sdkconfig"
        {   # defaults without a size, then the size and its partition table
            if [[ $cpu == esp32 ]]; then cat "$ROOT/boot/sdkconfig.defaults"
            else cat "$ROOT/boot/sdkconfig.$cpu.defaults"; fi | sed '/FLASHSIZE_/d;/PARTITION_TABLE_CUSTOM_FILENAME/d'
            echo "CONFIG_ESPTOOLPY_FLASHSIZE_${size^^}=y"
            echo "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/$size.csv\""
        } > "$cfg"
        rm -f "$out/stage2-$size.sdkconfig.gen"
        IDF_TARGET=$cpu idf.py -C "$ROOT/boot" -B "$out/stage2-$size-build" \
            -D SDKCONFIG="$out/stage2-$size.sdkconfig.gen" -D SDKCONFIG_DEFAULTS="$cfg" build
        cp "$out/stage2-$size-build/bootloader/bootloader.bin" "$out/stage2-$size.bin"
        cp "$out/stage2-$size-build/partition_table/partition-table.bin" "$out/partition-table-$size.bin"
        # One image for a blank board: stage2 + table + loader merged at offset 0.
        boot_off=0x0; [[ $cpu == esp32 ]] && boot_off=0x1000
        [[ $cpu == esp32s3 || $cpu == esp32c6 ]] && boot_off=0x0
        python -m esptool --chip "$cpu" merge-bin -o "$out/factory-$size.bin" \
            --flash_mode dio --flash_size "${size^^}" \
            "$boot_off" "$out/stage2-$size.bin" 0x8000 "$out/partition-table-$size.bin" \
            0x10000 "$out/loader.bin"
    done
done
(cd "$ROOT/target/out" && find . -name '*.bin' -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
