#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run an ELF on the console as homebrew (foreground app) through websrv and
# save the streamed output to .local-work/logs/ (ignored by git). Use this,
# not tools/deploy.sh, for payloads that draw to the TV. Needs websrv (8080)
# and ftpsrv (2121) running on the console; see tools/fetch-websrv.sh.
#
# Usage: tools/deploy-hb.sh NAME PAYLOAD.elf [--timeout SECONDS] [--args "..."]
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

name=${1:?usage: tools/deploy-hb.sh NAME PAYLOAD.elf [--timeout SECONDS] [--args "..."]}
payload=${2:?usage: tools/deploy-hb.sh NAME PAYLOAD.elf [--timeout SECONDS] [--args "..."]}
shift 2
load_console_env
: "${PS5_HOST:?PS5_HOST is not set. Copy config/console.env.example to .local-config/console.env and edit it.}"

logdir="${KSRE_PS5_ROOT}/.local-work/logs"
mkdir -p "$logdir"
log="${logdir}/${name}-hb-$(date -u +%Y%m%dT%H%M%SZ).log"

echo "running ${name} as homebrew via websrv; log: ${log#"${KSRE_PS5_ROOT}"/}" >&2
python3 "${KSRE_PS5_ROOT}/tools/ps5_hbrun.py" "$PS5_HOST" "$payload" --name "$name" \
  --ftp-port "${PS5_FTP_PORT:-2121}" --log "$log" "$@"
