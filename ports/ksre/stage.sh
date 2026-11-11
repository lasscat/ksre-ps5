#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stage KSRE v2.0.5 for the console (milestone 05): copy the maintainer's own game/
# directory, add the platform overlay, and compile .rpyc + the bytecode cache on the
# host twin (same Ren'Py 8.5.3 and CPython 3.12.8 as the console payload).
#
#   ports/ksre/stage.sh [KSRE_DIR]   default game-data/ksre-v2.0.5 -> build/ports/ksre/stage
#
# Upload the result with tools/deploy-hb.sh --tree build/ports/ksre/stage:/data/ksre-ps5/ksre
set -euo pipefail
# shellcheck source=../../tools/lib/common.sh
. "$(dirname "$0")/../../tools/lib/common.sh"
root=$KSRE_PS5_ROOT
src=${1:-$root/game-data/ksre-v2.0.5}
out=$root/build/ports/ksre
stage=$out/stage
twin=$root/build/ports/renpy/ksre-ps5-renpy-host

[ -f "$src/game/definitions.rpy" ] || { echo "no KSRE game/ in $src (docs/GAME_DATA.md)" >&2; exit 1; }
[ -x "$twin" ] || { echo "missing $twin: run ports/renpy/build.sh host" >&2; exit 1; }

rm -rf "$stage"
mkdir -p "$stage" "$out/host-root"
# APFS clone when available (no extra space), plain copy otherwise.
cp -cR "$src/game" "$stage/game" 2>/dev/null || cp -R "$src/game" "$stage/game"
# Ren'Py writes its caches here; anything left over from the source tree is dropped.
rm -rf "$stage/game/cache" "$stage/game/saves"
find "$stage/game" -name '*.rpyc' -delete
cp "$root/ports/ksre/zz_ksre_ps5.rpy" "$stage/game/"
# What Ren'Py's own distribution build adds (launcher/game/distribute.rpy): marks a built
# game, so developer mode, script backups and auto-reload stay off.
printf '(8, 5, 3)' > "$stage/game/script_version.txt"

log=$out/compile-host.log
"$twin" --root "$out/host-root" --python-home "$root/build/host-python" \
  --renpy-base "$root/build/ports/renpy/runtime/renpy" --game "$stage" \
  --max-seconds 600 compile > "$log" 2>&1 || true
grep -a '^KSRE_PS5_RENPY_END' "$log" || true
if ! grep -q '^KSRE_PS5_RENPY_END rc=0$' "$log"; then
  echo "compile failed; see ${log#"$root"/}" >&2
  exit 1
fi
n_rpy=$(find "$stage/game" -name '*.rpy' | wc -l | tr -d ' ')
n_rpyc=$(find "$stage/game" -name '*.rpyc' | wc -l | tr -d ' ')
echo "staged $stage: $n_rpy .rpy, $n_rpyc .rpyc, $(du -sh "$stage" | cut -f1)"
