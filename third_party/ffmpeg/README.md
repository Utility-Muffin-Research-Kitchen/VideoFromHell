# Vendored FFmpeg interface (not FFmpeg itself)

Video From Hell uses FFmpeg 4.4 for demuxing, audio decode, software-video
fallback, and one-off thumbnail conversion. The pak ships that runtime itself in
`lib/`, built by `scripts/build-mlp1-ffmpeg.sh`, because LoongOS 1.4 replaced the
stock FFmpeg 4.4 with 6.1. This directory contains only the public headers and
tiny aarch64 SONAME link stubs needed to compile against it:

- `include/` is the public-header closure used by the app: `libavformat`,
  `libavcodec`, `libavutil`, `libswresample`, and `libswscale`. The headers are
  from FFmpeg 4.4 and remain LGPL-2.1-or-later.
- `stub/` contains no FFmpeg implementation. The tiny `.so.N` files exist only
  to satisfy the cross-linker; the installed application binds to the pak's
  `lib/libav*.so.N` through its `$ORIGIN/../lib` RUNPATH.

Nothing in this directory is packaged into `VideoFromHell.pak`.

When a source file starts using a new FFmpeg symbol, regenerate the relevant
stub with that symbol exported and retain its exact runtime SONAME. The
regeneration must use the MLP1 cross toolchain. If the symbol lives in a library
or component the bundled build disables, enable it in `ports/mlp1/build-ffmpeg.sh`.
