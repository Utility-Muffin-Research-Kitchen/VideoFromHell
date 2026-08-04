#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VFH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="$(cd "$VFH_DIR/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"

# The package and the test probe both use generated SONAME-only link stubs.
# Keep direct script use reproducible after a clean checkout.
if [ "${VFH_SKIP_LINK_STUBS:-0}" != "1" ]; then
  "$SCRIPT_DIR/build-ffmpeg-link-stubs.sh"
fi

targets=("$@")
if [ "${#targets[@]}" -eq 0 ]; then
  targets=(all)
fi

echo "=== Building Video From Hell for MLP1 (workspace: $WORKSPACE; targets: ${targets[*]}) ==="
docker run --rm -v "$WORKSPACE":/workspace -w /workspace/VideoFromHell "$IMAGE" \
  make -C ports/mlp1 "${targets[@]}"
echo "=== MLP1 build complete ==="
