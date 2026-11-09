#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check out pinned upstream sources into external/ (ignored by git) for
# inspection and, in later milestones, for building. Revisions come from
# upstreams.lock. Existing checkouts are moved to the pinned commit only if
# they have no local modifications.
#
# Usage: tools/fetch-upstreams.sh [renpy] [renpy-build] [ps5-sdl] [pacbrew]
#                                 [elfldr] [websrv] [ksre-code]
#   (no arguments = everything except ksre-code)
#
# ksre-code is a blobless, sparse checkout of KSRE's text files only (scripts,
# configuration) for reading. It deliberately excludes images, audio, video
# and fonts. Game data for playing is a separate step: docs/GAME_DATA.md.
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

ext="${KSRE_PS5_ROOT}/external"
mkdir -p "$ext"

checkout_pinned() { # name url commit [extra clone args...]
  local name=$1 url=$2 commit=$3
  shift 3
  local dir="${ext}/${name}"
  if [ ! -d "${dir}/.git" ]; then
    git clone --quiet --filter=blob:none --no-checkout "$@" "$url" "$dir"
  fi
  if [ -n "$(git -C "$dir" status --porcelain 2>/dev/null)" ]; then
    echo "skip ${name}: local modifications present in ${dir}" >&2
    return 0
  fi
  if ! git -C "$dir" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    git -C "$dir" fetch --quiet origin "$commit" || git -C "$dir" fetch --quiet --tags origin
  fi
  git -C "$dir" -c advice.detachedHead=false checkout --quiet "$commit"
  echo "${name}: $(git -C "$dir" rev-parse HEAD)"
}

want=("$@")
[ ${#want[@]} -gt 0 ] || want=(renpy renpy-build ps5-sdl pacbrew elfldr websrv)

for w in "${want[@]}"; do
  case "$w" in
    renpy)       checkout_pinned renpy "$(lock_get RENPY_URL)" "$(lock_get RENPY_COMMIT)" ;;
    renpy-build) checkout_pinned renpy-build "$(lock_get RENPY_BUILD_URL)" "$(lock_get RENPY_BUILD_COMMIT)" ;;
    ps5-sdl)     checkout_pinned ps5-SDL "$(lock_get PS5_SDL_URL)" "$(lock_get PS5_SDL_COMMIT)" ;;
    pacbrew)     checkout_pinned pacbrew-repo "$(lock_get PACBREW_REPO)" "$(lock_get PACBREW_COMMIT)" ;;
    elfldr)      checkout_pinned elfldr "$(lock_get ELFLDR_REPO)" "$(lock_get ELFLDR_COMMIT)" ;;
    websrv)      checkout_pinned websrv "$(lock_get WEBSRV_REPO)" "$(lock_get WEBSRV_COMMIT)" ;;
    ksre-code)
      dir="${ext}/ksre"
      if [ ! -d "${dir}/.git" ]; then
        git clone --quiet --filter=blob:none --no-checkout "$(lock_get KSRE_URL)" "$dir"
      fi
      git -C "$dir" sparse-checkout init --no-cone
      printf '%s\n' '/*' '!*.png' '!*.jpg' '!*.webp' '!*.mkv' '!*.webm' '!*.ogg' \
        '!*.opus' '!*.ttf' '!*.otf' '!*.icns' '!*.ico' > "${dir}/.git/info/sparse-checkout"
      git -C "$dir" -c advice.detachedHead=false checkout --quiet "$(lock_get KSRE_COMMIT)"
      echo "ksre (code only): $(git -C "$dir" rev-parse HEAD)"
      ;;
    *) echo "unknown upstream: $w" >&2; exit 2 ;;
  esac
done
