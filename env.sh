# Source from the repository root before building firmware. All mutable build
# state (ESP-IDF, toolchains, Nix profile) stays under target/.

if [ -n "${BASH_SOURCE:-}" ]; then
    _wl_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    _wl_dir="$(pwd)"
fi
export WL_REPO="${WL_REPO:-$_wl_dir}"
_wl_target="$WL_REPO/target"
mkdir -p "$_wl_target" 2>/dev/null || true

export WL_IDF_VERSION="v6.0.2"
# Reuse an existing DMesh checkout's ESP-IDF (target/esp32-6.0) instead of
# downloading another copy. Override with WL_ESP_ROOT, or WL_DMESH_DIR=/path
# (set WL_DMESH_DIR=none to force a private install under target/).
if [ -z "${WL_ESP_ROOT:-}" ]; then
    for _wl_cand in "${WL_DMESH_DIR:-}" /ws/dmesh "$WL_REPO/../dmesh"; do
        [ -n "$_wl_cand" ] && [ "$_wl_cand" != none ] || continue
        _wl_root="$_wl_cand/target/esp32-6.0"
        if [ -f "$_wl_root/esp-idf/export.sh" ] && [ -d "$_wl_root/espressif/python_env" ]; then
            WL_ESP_ROOT="$(cd "$_wl_root" && pwd)"
            WL_DMESH_REPO="$(cd "$_wl_cand" && pwd)"
            break
        fi
    done
fi
export WL_ESP_ROOT="${WL_ESP_ROOT:-$_wl_target/esp-idf-6.0}"
export NIX_PROFILE="${WL_NIX_PROFILE:-$_wl_target/nix/profile}"
export NIX_CONFIG="${NIX_CONFIG:-experimental-features = nix-command flakes}"
export TMPDIR="${TMPDIR:-/tmp}"

# Host tools (cmake, ninja, git): own profile, else DMesh's ESP32 profile.
if [ -n "${WL_DMESH_REPO:-}" ] && [ -d "$WL_DMESH_REPO/target/nix/esp32-profile/bin" ]; then
    export PATH="$WL_DMESH_REPO/target/nix/esp32-profile/bin:$PATH"
fi
if [ ! -d "$NIX_PROFILE/bin" ] && [ -n "${WL_DMESH_REPO:-}" ] && [ -d "$WL_DMESH_REPO/target/nix/profile/bin" ]; then
    NIX_PROFILE="$WL_DMESH_REPO/target/nix/profile"
fi
if [ -d "$NIX_PROFILE/bin" ]; then
    export PATH="/usr/bin:/bin:$NIX_PROFILE/bin:$PATH"
fi

# ESP-IDF: installed by scripts/esp-deps.sh. Skipped when a container or an
# outer shell already provides idf.py.
if [ -z "${IDF_PATH:-}" ] && [ -f "$WL_ESP_ROOT/esp-idf/export.sh" ]; then
    export IDF_PATH="$WL_ESP_ROOT/esp-idf"
    export IDF_TOOLS_PATH="$WL_ESP_ROOT/espressif"
    # Put the install's own Python first: export.sh looks up the venv by the
    # running python3 version (the host python may be a different minor).
    for _wl_venv in "$IDF_TOOLS_PATH"/python_env/idf*_env; do
        if [ -x "$_wl_venv/bin/python" ]; then
            export IDF_PYTHON_ENV_PATH="$_wl_venv"
            export PATH="$_wl_venv/bin:$PATH"
        fi
    done
    # export.sh prints a banner; this fragment must stay silent.
    . "$IDF_PATH/export.sh" >/dev/null 2>&1
fi

unset _wl_venv _wl_dir _wl_target _wl_cand _wl_root
export WL_ENV_LOADED=1
