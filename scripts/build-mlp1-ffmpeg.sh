#!/usr/bin/env bash
# Build the FFmpeg runtime that ships inside VideoFromHell.pak/lib.
#
# Why bundle: LoongOS 1.3 ships FFmpeg 4.4 (avcodec 58), 1.4 replaced it with
# FFmpeg 6.1 (avcodec 60), so a binary linked against the stock libraries runs
# on one firmware only. The pak carries its own trimmed FFmpeg 4.4 instead and
# finds it through RUNPATH $ORIGIN/../lib, the way Leaf's RetroArch carries
# lib/ffmpeg. The SONAMEs stay the 4.4 ones the app was written against.
#
# Fetches the pinned FFmpeg and dav1d release tarballs (sha256-checked), then
# builds them inside the mlp1-toolchain image with ports/mlp1/build-ffmpeg.sh.
# dav1d is the AV1 decoder: FFmpeg 4.4's native av1 decoder needs a hardware
# accelerator, and the stock 4.4 decoded AV1 through libdav1d as well. The
# result is reused while the tarballs, the container script and the image are
# unchanged.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"

FFMPEG_VERSION=4.4.8
FFMPEG_SHA256=c73848c4ae283d9eaee7be3b276affbc3543380483555500d0dd2c9b7e1c39c3
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
DAV1D_VERSION=1.5.4
DAV1D_SHA256=686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5
DAV1D_URL="https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.xz"

SRC_DIR="$APP_DIR/build/ffmpeg-src"
TARBALL="$SRC_DIR/ffmpeg-$FFMPEG_VERSION.tar.xz"
DAV1D_TARBALL="$SRC_DIR/dav1d-$DAV1D_VERSION.tar.xz"
OUT="$APP_DIR/build/mlp1/ffmpeg"
INNER="$APP_DIR/ports/mlp1/build-ffmpeg.sh"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch() {  # fetch <file> <url> <sha256>
  if [ ! -f "$1" ] || [ "$(sha256_of "$1")" != "$3" ]; then
    echo "=== Fetching $2 ==="
    curl -fsSL --retry 3 -o "$1.part" "$2"
    mv "$1.part" "$1"
  fi
  local actual
  actual="$(sha256_of "$1")"
  if [ "$actual" != "$3" ]; then
    echo "checksum mismatch for $1: $actual != $3" >&2
    exit 1
  fi
}

mkdir -p "$SRC_DIR"
fetch "$TARBALL" "$FFMPEG_URL" "$FFMPEG_SHA256"
fetch "$DAV1D_TARBALL" "$DAV1D_URL" "$DAV1D_SHA256"

image_id="$(docker image inspect --format '{{.Id}}' "$IMAGE" 2>/dev/null || true)"
if [ -z "$image_id" ]; then
  docker pull "$IMAGE" >/dev/null
  image_id="$(docker image inspect --format '{{.Id}}' "$IMAGE")"
fi
stamp="ffmpeg=$FFMPEG_VERSION sha256=$FFMPEG_SHA256 dav1d=$DAV1D_SHA256 image=$image_id inner=$(sha256_of "$INNER")"
if [ -f "$OUT/stamp" ] && [ "$(cat "$OUT/stamp")" = "$stamp" ]; then
  echo "=== Reusing FFmpeg $FFMPEG_VERSION: $OUT ==="
  exit 0
fi

echo "=== Building FFmpeg $FFMPEG_VERSION for MLP1 ==="
rm -rf "$OUT"
mkdir -p "$OUT"
docker run --rm \
  -v "$TARBALL":/src/ffmpeg.tar.xz:ro \
  -v "$DAV1D_TARBALL":/src/dav1d.tar.xz:ro \
  -v "$INNER":/src/build-ffmpeg.sh:ro \
  -v "$OUT":/out \
  -e FFMPEG_VERSION="$FFMPEG_VERSION" \
  -e FFMPEG_URL="$FFMPEG_URL" \
  -e FFMPEG_SHA256="$FFMPEG_SHA256" \
  -e DAV1D_VERSION="$DAV1D_VERSION" \
  -e DAV1D_URL="$DAV1D_URL" \
  -e DAV1D_SHA256="$DAV1D_SHA256" \
  "$IMAGE" bash /src/build-ffmpeg.sh
printf '%s\n' "$stamp" >"$OUT/stamp"
echo "=== FFmpeg ready: $OUT/lib ==="
