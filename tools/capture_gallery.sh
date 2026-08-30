#!/bin/bash
# Renders every screen the device can show, straight out of the firmware's own
# view code, into assets/screens/*.png.
#
# These are not mockups and not photographs: the desktop build runs the same
# source at the same 360x360, and time is simulated and fixed-step, so a
# regenerated gallery is byte-identical unless the rendering actually changed.
# That makes a diff in these files a real signal rather than camera noise.
#
# Usage:  tools/capture_gallery.sh        (from the repo root)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/assets/screens"
BIN="$ROOT/.pio/build/native/program"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

HOMEBREW_PREFIX="${HOMEBREW_PREFIX:-/opt/homebrew}" pio run -e native >/dev/null
mkdir -p "$OUT"

# shot <name> <simulated-ms> [VAR=VALUE ...]
shot() {
  local name="$1" ms="$2"; shift 2
  env "$@" KNOB_HEADLESS=1 KNOB_EXIT_MS="$ms" KNOB_DUMP="$TMP/$name.bmp" "$BIN"
  python3 "$ROOT/tools/bmp_to_png.py" "$TMP/$name.bmp" "$OUT/$name.png"
  echo "  $name.png"
}

echo "rendering gallery -> assets/screens/"

# The effects, each given enough simulated time to fill its particle field.
shot cover-light 4000 KNOB_THEME=0
shot heartbeat   4000 KNOB_THEME=1
shot rain        4000 KNOB_THEME=2
shot tetris      6000 KNOB_THEME=3
shot outrun      4000 KNOB_THEME=4
shot matrix      4000 KNOB_THEME=5
shot record      4000 KNOB_THEME=6

# The furniture.
shot playlists   2000 KNOB_SCREEN=list KNOB_POS=2.4
# Theme 0 behind the picker on purpose: Outrun and Matrix own the backdrop and
# ignore the ambient dim, which is right on the device and unreadable in a still.
shot themes      2000 KNOB_SCREEN=themes KNOB_POS=4.0 KNOB_THEME=0
shot confirm     2000 KNOB_SCREEN=confirm KNOB_POS=0.72
shot daisy       3000 KNOB_SCREEN=daisy
shot safe-mode   1000 KNOB_SCREEN=safe

# The meeting controller, in the states worth seeing side by side.
shot teams-live    2500 KNOB_SCREEN=teams KNOB_TEAMS=0,1,754
shot teams-muted   2500 KNOB_SCREEN=teams KNOB_TEAMS=1,0,754
shot teams-pending 1500 KNOB_SCREEN=teams KNOB_TEAMS=1,0,312 KNOB_TEAMS_PENDING=mic
shot teams-unknown 1500 KNOB_SCREEN=teams KNOB_TEAMS=-1,-1,-1

echo "done."
