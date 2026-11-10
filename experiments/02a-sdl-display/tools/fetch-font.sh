#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the pinned DejaVu Sans font (sha256-verified) for experiment 02a and
# write it to the path given as $1. The font is never committed; it is
# embedded into the experiment binary at build time.
#
# Usage: fetch-font.sh OUT.ttf
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=../../../tools/lib/common.sh
. "${here}/../../../tools/lib/common.sh"

out=${1:?usage: fetch-font.sh OUT.ttf}
url=$(lock_get DEJAVU_URL)
tsum=$(lock_get DEJAVU_TARBALL_SHA256)
member=$(lock_get DEJAVU_SANS_MEMBER)
fsum=$(lock_get DEJAVU_SANS_SHA256)
ver=$(lock_get DEJAVU_VERSION)

if [ -f "$out" ] && [ "$(sha256_of "$out")" = "$fsum" ]; then
  exit 0
fi

dl="${KSRE_PS5_ROOT}/external/toolchains/downloads/dejavu-fonts-ttf-${ver}.tar.bz2"
download_verified "$url" "$dl" "$tsum" >&2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tar -xjf "$dl" -C "$tmp" "$member"
got=$(sha256_of "${tmp}/${member}")
if [ "$got" != "$fsum" ]; then
  echo "font SHA-256 mismatch: expected $fsum, got $got" >&2
  exit 1
fi
mkdir -p "$(dirname "$out")"
cp "${tmp}/${member}" "$out"
