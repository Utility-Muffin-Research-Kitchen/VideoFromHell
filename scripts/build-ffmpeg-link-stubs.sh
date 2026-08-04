#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VFH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="$(cd "$VFH_DIR/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"

docker run --rm -v "$WORKSPACE":/workspace -w /workspace/VideoFromHell "$IMAGE" /bin/sh -eu -c '
  toolchain=/opt/mlp1-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
  stub=third_party/ffmpeg/stub
  mpp_stub=third_party/mpp/stub
  build_stub() {
    "$toolchain" -std=c11 -Wall -Wextra -Werror -shared -fPIC \
      -Ithird_party/ffmpeg/include -Wl,-soname,"$2" -o "$1" "$3"
  }
  build_stub "$stub/libavformat.so.58" libavformat.so.58 "$stub/vfh_avformat_stub.c"
  build_stub "$stub/libavcodec.so.58" libavcodec.so.58 "$stub/vfh_avcodec_stub.c"
  build_stub "$stub/libavutil.so.56" libavutil.so.56 "$stub/vfh_avutil_stub.c"
  build_stub "$stub/libswresample.so.3" libswresample.so.3 "$stub/vfh_swresample_stub.c"
  build_stub "$stub/libswscale.so.5" libswscale.so.5 "$stub/vfh_swscale_stub.c"
  build_stub "$mpp_stub/librockchip_mpp.so.1" librockchip_mpp.so.1 "$mpp_stub/vfh_mpp_stub.c"
'
