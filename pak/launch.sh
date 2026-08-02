#!/bin/sh
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Source the Leaf platform env (paths, runtime directories) when available.
PLATFORM="${PLATFORM:-mlp1}"
PLATFORM_ENV_SH=""
for root in "${SDCARD_PATH:-/mnt/sdcard}" /mnt/sdcard /media/sdcard1; do
  env_sh="$root/.system/leaf/platforms/$PLATFORM/launcher/env.sh"
  if [ -f "$env_sh" ]; then
    PLATFORM_ENV_SH="$env_sh"
    . "$env_sh"
    break
  fi
done

# A Pak may live on the writable card while Leaf's platform files live on the
# other card.  Keep the caller's storage roots, but fall back to the actual
# launcher asset directory if its sourced paths do not exist there.
if [ -n "$PLATFORM_ENV_SH" ]; then
  LAUNCHER_DIR="${PLATFORM_ENV_SH%/env.sh}"
  if [ ! -d "${CAT_FONTS_DIR:-}" ]; then
    export CAT_FONTS_DIR="$LAUNCHER_DIR/res"
  fi
  if [ ! -d "${CAT_THEMES_DIR:-}" ]; then
    export CAT_THEMES_DIR="$LAUNCHER_DIR/res/themes"
  fi
  if [ ! -d "${CAT_STATUS_ASSETS_DIR:-}" ]; then
    export CAT_STATUS_ASSETS_DIR="$LAUNCHER_DIR/res/assets"
  fi
fi

BIN="$PAK_DIR/bin/videofromhell"
export VIDEOFROMHELL_PAK_DIR="$PAK_DIR"
LOG_DIR="${LOGS_PATH:-$PAK_DIR}"
mkdir -p "$LOG_DIR" 2>/dev/null || true

cd "$PAK_DIR"
exec "$BIN" 2>"$LOG_DIR/videofromhell.log"
