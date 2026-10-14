#!/usr/bin/env bash
# Tests that need no board. CI runs the same.
#   scripts/test-host.sh [tools|rust|android|all]     (default: tools + rust)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
what="${1:-default}"
if [[ $what == tools || $what == default || $what == all ]]; then
    python3 tests/test_tools.py
    mkdir -p target
    gcc -Wall -Wextra -fsanitize=address,undefined -I components/esp_wifi_loader/include tests/test_state.c \
        components/esp_wifi_loader/src/state_core.c -o target/test_state
    target/test_state
fi
if [[ $what == rust || $what == default || $what == all ]]; then
    cargo test --locked --manifest-path modules/mod_flash/Cargo.toml
fi
if [[ $what == android || $what == all ]]; then
    (cd android && ./gradlew :app:testDebugUnitTest)
fi
