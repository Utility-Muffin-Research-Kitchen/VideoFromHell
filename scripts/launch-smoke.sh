#!/bin/sh
# Exercise the pak launcher when an older Leaf environment advertises a
# read-only/invalid primary path before a writable secondary userdata card.
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vfh-launch-smoke.XXXXXX")"

cleanup() {
  rm -rf "$TEST_DIR"
}
trap cleanup EXIT HUP INT TERM

PAK_DIR="$TEST_DIR/VideoFromHell.pak"
WRITABLE_USERDATA="$TEST_DIR/userdata"
WRITABLE_CARD="$TEST_DIR/card"
INVALID_USERDATA="$TEST_DIR/invalid-userdata"
INVALID_CARD="$TEST_DIR/invalid-card"
INVALID_LOGS="$TEST_DIR/invalid-logs"

mkdir -p "$PAK_DIR/bin" "$WRITABLE_USERDATA" "$WRITABLE_CARD"
: >"$INVALID_USERDATA"
: >"$INVALID_CARD"
: >"$INVALID_LOGS"
cp "$ROOT_DIR/pak/launch.sh" "$PAK_DIR/launch.sh"
chmod +x "$PAK_DIR/launch.sh"
ln -s "$(command -v env)" "$PAK_DIR/bin/videofromhell"

OUTPUT="$(
  PLATFORM=vfh-launch-smoke \
  LOGS_PATH="$INVALID_LOGS" \
  USERDATA_PATH="$INVALID_USERDATA" \
  USERDATA_PATHS="$INVALID_USERDATA:$WRITABLE_USERDATA" \
  SDCARD_PATH="$INVALID_CARD" \
  SDCARD_PATHS="$INVALID_CARD:$WRITABLE_CARD" \
  "$PAK_DIR/launch.sh"
)"

printf '%s\n' "$OUTPUT" | grep -Fx "USERDATA_PATH=$WRITABLE_USERDATA" >/dev/null
printf '%s\n' "$OUTPUT" | grep -Fx "LOGS_PATH=$WRITABLE_USERDATA/logs" >/dev/null
printf '%s\n' "$OUTPUT" | grep -Fx "RECORDINGS_PATH=$WRITABLE_CARD/Recordings" >/dev/null
test -f "$WRITABLE_USERDATA/logs/videofromhell.log"

echo "launch-smoke: PASS"
