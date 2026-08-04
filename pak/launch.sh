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

# Current Leaf payloads export the active writable card as SDCARD_PATH and
# USERDATA_PATH.  Older payloads can still advertise a read-only system card
# first, even though the ordered *_PATHS variables contain a writable card.
# Prefer the explicitly selected path whenever it is usable, then recover from
# the advertised list without making a private storage layout of our own.
vfh_path_is_writable_dir() {
  [ -n "$1" ] && [ -d "$1" ] && [ -w "$1" ]
}

vfh_pick_userdata_path() {
  VFH_SELECTED_USERDATA=""
  if vfh_path_is_writable_dir "${USERDATA_PATH:-}"; then
    VFH_SELECTED_USERDATA="$USERDATA_PATH"
    return
  fi
  VFH_OLD_IFS=$IFS
  IFS=:
  for VFH_CANDIDATE in ${USERDATA_PATHS:-}; do
    if vfh_path_is_writable_dir "$VFH_CANDIDATE"; then
      VFH_SELECTED_USERDATA="$VFH_CANDIDATE"
      break
    fi
  done
  IFS=$VFH_OLD_IFS
}

vfh_pick_recordings_card() {
  VFH_SELECTED_CARD=""
  if vfh_path_is_writable_dir "${SDCARD_PATH:-}"; then
    VFH_SELECTED_CARD="$SDCARD_PATH"
    return
  fi
  VFH_OLD_IFS=$IFS
  IFS=:
  for VFH_CANDIDATE in ${SDCARD_PATHS:-}; do
    if vfh_path_is_writable_dir "$VFH_CANDIDATE"; then
      VFH_SELECTED_CARD="$VFH_CANDIDATE"
      break
    fi
  done
  IFS=$VFH_OLD_IFS
}

vfh_pick_userdata_path
if [ -n "$VFH_SELECTED_USERDATA" ] && [ "$VFH_SELECTED_USERDATA" != "${USERDATA_PATH:-}" ]; then
  export USERDATA_PATH="$VFH_SELECTED_USERDATA"
fi

vfh_prepare_writable_dir() {
  [ -n "$1" ] || return 1
  if ! mkdir -p "$1" 2>/dev/null; then
    return 1
  fi
  VFH_LOG_PROBE="$1/.vfh-launch-$$.tmp"
  if ! (umask 077 && : >"$VFH_LOG_PROBE") 2>/dev/null; then
    return 1
  fi
  rm -f "$VFH_LOG_PROBE"
  return 0
}

# Catastrophe also uses LOGS_PATH for its own diagnostics.  Keep it aligned
# with the recovered writable userdata root instead of only redirecting this
# shell's stderr there.
VFH_RUNTIME_LOGS=""
if vfh_prepare_writable_dir "${LOGS_PATH:-}"; then
  VFH_RUNTIME_LOGS="$LOGS_PATH"
elif [ -n "${USERDATA_PATH:-}" ] && vfh_prepare_writable_dir "$USERDATA_PATH/logs"; then
  VFH_RUNTIME_LOGS="$USERDATA_PATH/logs"
elif vfh_prepare_writable_dir "${TMPDIR:-/tmp}/videofromhell"; then
  VFH_RUNTIME_LOGS="${TMPDIR:-/tmp}/videofromhell"
fi
if [ -n "$VFH_RUNTIME_LOGS" ]; then
  export LOGS_PATH="$VFH_RUNTIME_LOGS"
fi

# New Leaf runtimes supply this exact producer-owned path.  The fallback only
# applies to old/direct launches, where the writable primary card is the best
# available compatibility signal.
if [ -z "${RECORDINGS_PATH:-}" ]; then
  vfh_pick_recordings_card
  if [ -n "$VFH_SELECTED_CARD" ]; then
    export RECORDINGS_PATH="$VFH_SELECTED_CARD/Recordings"
  fi
fi

BIN="$PAK_DIR/bin/videofromhell"
export VIDEOFROMHELL_PAK_DIR="$PAK_DIR"

vfh_try_log_dir() {
  [ -n "$1" ] || return 1
  if ! vfh_prepare_writable_dir "$1"; then
    return 1
  fi
  VFH_LOG_FILE="$1/videofromhell.log"
  if [ -e "$VFH_LOG_FILE" ] && [ ! -w "$VFH_LOG_FILE" ]; then
    VFH_LOG_FILE=""
    return 1
  fi
  return 0
}

VFH_LOG_FILE=""
if ! vfh_try_log_dir "${LOGS_PATH:-}"; then
  if ! vfh_try_log_dir "${USERDATA_PATH:-}/logs"; then
    vfh_try_log_dir "${TMPDIR:-/tmp}/videofromhell" || true
  fi
fi

cd "$PAK_DIR"
if [ -n "$VFH_LOG_FILE" ]; then
  exec "$BIN" 2>"$VFH_LOG_FILE"
fi
exec "$BIN"
