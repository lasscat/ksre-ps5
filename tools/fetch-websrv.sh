#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Download the pinned ps5-payload-websrv release ELF (SHA-256 verified) to
# external/toolchains/downloads/. websrv provides the homebrew launcher
# (/hbldr) that runs an ELF as a foreground application, which payloads that
# draw to the TV need (see experiments/02a-sdl-display/README.md).
#
# Sending it to the console is a separate, deliberate step:
#   tools/deploy.sh "$(tools/fetch-websrv.sh)"
# websrv then listens on TCP 8080 WITHOUT authentication until the console is
# restarted: use it only on a trusted LAN.
#
# Prints the path of the verified ELF on stdout.
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

url=$(lock_get WEBSRV_ELF_URL)
sum=$(lock_get WEBSRV_ELF_SHA256)
rel=$(lock_get WEBSRV_RELEASE)
dest="${KSRE_PS5_ROOT}/external/toolchains/downloads/websrv-${rel}.elf"
download_verified "$url" "$dest" "$sum" >&2
echo "$dest"
