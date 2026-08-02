# Vendored FFmpeg interface (not FFmpeg itself)

Video From Hell will use the vendor's FFmpeg 4.4 runtime for demuxing, audio
decode, software-video fallback, and one-off thumbnail conversion. This directory
contains only the public headers and tiny aarch64 SONAME link stubs needed to
compile against that runtime:

- `include/` is the public-header closure used by the app: `libavformat`,
  `libavcodec`, `libavutil`, `libswresample`, and `libswscale`. The headers are
  from FFmpeg 4.4 and remain LGPL-2.1-or-later.
- `stub/` contains no FFmpeg implementation. The tiny `.so.N` files exist only
  to satisfy the cross-linker; the installed application binds to the device's
  matching `/usr/lib/libav*.so.N` at runtime.

Nothing in this directory is packaged into `VideoFromHell.pak`. The device probe
recorded FFmpeg 4.4 (`libavformat.so.58`, `libavcodec.so.58`,
`libavutil.so.56`, `libswresample.so.3`, and `libswscale.so.5`) as part of the
vendor Buildroot image. It is not a bundled FFmpeg distribution or a Kodi
dependency.

When a source file starts using a new FFmpeg symbol, regenerate the relevant
stub with that symbol exported and retain its exact runtime SONAME. The
regeneration must use the MLP1 cross toolchain; do not ship real FFmpeg binaries
or libraries in the pak.
