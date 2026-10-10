#!/usr/bin/env bash
# Replace the rolling GitHub release `latest` with the files in REL (default target/rel; scripts/publish-release.sh).
#   scripts/publish-latest.sh VERSION          (needs gh, authenticated: GH_TOKEN in CI)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="${REL:-$ROOT/target/rel}"
version="${1:?version}"
repo="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
sha="${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
gh release delete latest --repo "$repo" --cleanup-tag --yes || true
gh release create latest "$REL"/* --repo "$repo" --target "$sha" --title "latest ($version)" \
    --notes "Signed loader, bootstrap and Stage2 packages (version $version, commit $sha) and the flasher app (esp-wifi-loader.apk). factory-*.bin are unsigned merged images for a USB (espflash) or web flash. Short form for the app: $repo:latest."
