#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERIAL="${ADB_SERIAL:-$(adb devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')}"
FFMPEG_BIN="${VFH_FFMPEG_BIN:-/media/sdcard1/.system/leaf/platforms/mlp1/bin/ffmpeg}"

if [[ -z "$SERIAL" ]]; then
    echo "media-fixture-smoke: no online adb device" >&2
    exit 1
fi

ADB=(adb -s "$SERIAL")
FIXTURE_DIR="/tmp/vfh-media-fixture-$$"
REMOTE_SMOKE="$FIXTURE_DIR/vfh-media-smoke"

cleanup() {
    "${ADB[@]}" shell "rm -rf '$FIXTURE_DIR'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"${ADB[@]}" shell "test -x '$FFMPEG_BIN'"
"${ADB[@]}" shell "rm -rf '$FIXTURE_DIR' && mkdir -p '$FIXTURE_DIR'"
"${ADB[@]}" push "$ROOT_DIR/build/mlp1/vfh-media-smoke" "$REMOTE_SMOKE" >/dev/null

"${ADB[@]}" shell "set -eu
chmod 755 '$REMOTE_SMOKE'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -f lavfi -i color=black:s=320x180:d=2 \\
  -f lavfi -i testsrc2=s=320x180:d=8 \\
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0' -c:v mpeg4 \\
  '$FIXTURE_DIR/black-then-test.mp4'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -f lavfi -i color=black:s=320x180:d=10 -c:v mpeg4 '$FIXTURE_DIR/all-black.mp4'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -f lavfi -i testsrc2=s=320x180:d=0.4 -c:v mpeg4 '$FIXTURE_DIR/short.mp4'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -f lavfi -i testsrc2=s=320x180:d=4 -c:v mpeg4 '$FIXTURE_DIR/gameplay-lowres.mp4'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -f lavfi -i color=red:s=64x64:d=0.1 -frames:v 1 '$FIXTURE_DIR/cover.png'
'$FFMPEG_BIN' -hide_banner -loglevel error -y \\
  -i '$FIXTURE_DIR/short.mp4' -i '$FIXTURE_DIR/cover.png' \\
  -map 0:v -map 1:v -c:v:0 copy -c:v:1 mjpeg -disposition:v:1 attached_pic \\
  '$FIXTURE_DIR/embedded-art.mp4'
printf 'not media\\n' > '$FIXTURE_DIR/corrupt.mp4'
"

expect_poster() {
    local name="$1"
    "${ADB[@]}" shell "'$REMOTE_SMOKE' '$FIXTURE_DIR/$name'"
}

expect_finite_failure() {
    local name="$1"
    if "${ADB[@]}" shell "'$REMOTE_SMOKE' '$FIXTURE_DIR/$name'" >/dev/null 2>&1; then
        echo "media-fixture-smoke: expected $name to fail" >&2
        exit 1
    fi
}

# 10% is black and 25% has a test pattern: this proves the dark-frame retry.
expect_poster black-then-test.mp4
expect_poster short.mp4
expect_poster gameplay-lowres.mp4
expect_poster embedded-art.mp4
expect_finite_failure all-black.mp4
expect_finite_failure corrupt.mp4

echo "media-fixture-smoke: PASS ($SERIAL)"
