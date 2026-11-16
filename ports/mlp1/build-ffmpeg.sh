#!/bin/bash
# Configure and build Video From Hell's bundled FFmpeg. Runs INSIDE the
# mlp1-toolchain image; scripts/build-mlp1-ffmpeg.sh mounts the pinned tarball
# at /src/ffmpeg.tar.xz and the output directory at /out.
#
# LGPL-2.1+ only (no --enable-gpl / --enable-nonfree), shared libraries, and
# nothing autodetected from the sysroot. FFmpeg demuxes every container the
# library lists (vfh_media.c), decodes audio, decodes posters and embedded art,
# and decodes video in software when MPP does not take the stream (MPP handles
# H.264 and HEVC after the mp4toannexb filters). AV1 is not built: the stock
# 4.4 decoded it through libdav1d, which the toolchain sysroot does not carry.
set -euo pipefail

TC=/opt/mlp1-toolchain
export PATH="$TC/bin:$PATH"
SYSROOT="$TC/aarch64-buildroot-linux-gnu/sysroot"
CROSS=aarch64-buildroot-linux-gnu-
WORK=/tmp/ffmpeg-build
LIBS=(libavformat.so.58 libavcodec.so.58 libavutil.so.56 libswresample.so.3 libswscale.so.5)

demuxers=(mov matroska avi mpegts mpegps mpegvideo flv)
decoders=(
  h264 hevc mpeg4 h263 msmpeg4v1 msmpeg4v2 msmpeg4v3 mpeg1video mpeg2video
  vp8 vp9 flv vp6 vp6f vp6a theora vc1 wmv1 wmv2 wmv3 mjpeg png
  aac aac_latm ac3 eac3 dca truehd mlp mp1float mp2float mp3float opus vorbis
  flac alac wmav1 wmav2 wmapro amrnb amrwb adpcm_ima_wav adpcm_ms
  pcm_u8 pcm_s16be pcm_s16le pcm_s24be pcm_s24le pcm_s32le pcm_f32le
  pcm_alaw pcm_mulaw pcm_bluray pcm_dvd
)
parsers=(
  h264 hevc mpeg4video mpegvideo h263 vp8 vp9 vc1 mjpeg png
  aac aac_latm ac3 dca mlp mpegaudio opus vorbis flac
)
bsfs=(h264_mp4toannexb hevc_mp4toannexb vp9_superframe_split)

join() { local IFS=,; echo "$*"; }

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf /src/ffmpeg.tar.xz -C "$WORK" --strip-components=1
cd "$WORK"

configure_args=(
  --prefix=/usr
  --enable-cross-compile --arch=aarch64 --cpu=cortex-a55 --target-os=linux
  --cross-prefix="$CROSS" --sysroot="$SYSROOT" --pkg-config=pkg-config
  --enable-shared --disable-static --enable-pic
  --disable-autodetect --disable-everything
  --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-avfilter --disable-postproc
  --disable-network
  --enable-avformat --enable-avcodec --enable-swresample --enable-swscale --enable-zlib
  --enable-protocol=file
  --enable-demuxer="$(join "${demuxers[@]}")"
  --enable-decoder="$(join "${decoders[@]}")"
  --enable-parser="$(join "${parsers[@]}")"
  --enable-bsf="$(join "${bsfs[@]}")"
)
./configure "${configure_args[@]}" >/tmp/ff_configure.log 2>&1 \
  || { tail -40 /tmp/ff_configure.log; tail -30 ffbuild/config.log; exit 1; }
# A misspelt component is only a configure warning; make it fatal.
if grep -i "did not match anything" /tmp/ff_configure.log; then exit 1; fi
# Each library carries RUNPATH $ORIGIN so it finds its siblings in the pak's
# lib/ before the firmware's /usr/lib. Appended to config.mak rather than passed
# as --extra-ldsoflags, which configure re-evaluates; library.mak expands the
# link rule twice (define + eval), hence four dollars for one.
cat >>ffbuild/config.mak <<'MAK'
LDSOFLAGS += -Wl,-rpath,'$$$$ORIGIN' -Wl,--enable-new-dtags
MAK
make -j"$(nproc)" >/tmp/ff_make.log 2>&1 || { tail -60 /tmp/ff_make.log; exit 1; }
make install DESTDIR="$WORK/stage" >/tmp/ff_install.log 2>&1 \
  || { tail -20 /tmp/ff_install.log; exit 1; }

# FAT32 has no symlinks: ship each library once, under its SONAME.
mkdir -p /out/lib /out/licenses
for lib in "${LIBS[@]}"; do
  cp -L "$WORK/stage/usr/lib/$lib" "/out/lib/$lib"
  "${CROSS}strip" --strip-unneeded "/out/lib/$lib"
  runpath="$("${CROSS}readelf" -d "/out/lib/$lib" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
  [ "$runpath" = '$ORIGIN' ] || { echo "$lib RUNPATH is '$runpath', want \$ORIGIN" >&2; exit 1; }
done
cp COPYING.LGPLv2.1 /out/licenses/COPYING.LGPLv2.1
{
  echo "FFmpeg $FFMPEG_VERSION, built for Video From Hell (LGPL-2.1-or-later)."
  echo "Source: $FFMPEG_URL"
  echo "SHA-256: $FFMPEG_SHA256"
  echo "Build script: https://github.com/Utility-Muffin-Research-Kitchen/VideoFromHell/blob/main/ports/mlp1/build-ffmpeg.sh"
  echo "Configure:"
  sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' config.h
} >/out/licenses/FFMPEG-SOURCE.txt
ls -l /out/lib
