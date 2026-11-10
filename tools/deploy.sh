#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Send a payload to the console's ELF loader and save the streamed output to
# .local-work/logs/ (ignored by git). Console settings come from
# .local-config/console.env (copy config/console.env.example).
#
# Usage: tools/deploy.sh PAYLOAD.elf [--timeout SECONDS] [--idle-timeout S] [--until REGEX]
# Stops on EOF, on a KSRE_PS5_*_END line, or at the timeouts (see tools/ps5_send.py).
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

payload=${1:?usage: tools/deploy.sh PAYLOAD.elf [--timeout SECONDS]}
shift
load_console_env
: "${PS5_HOST:?PS5_HOST is not set. Copy config/console.env.example to .local-config/console.env and edit it.}"
port=${PS5_PORT:-9021}

logdir="${KSRE_PS5_ROOT}/.local-work/logs"
mkdir -p "$logdir"
log="${logdir}/$(basename "${payload%.*}")-$(date -u +%Y%m%dT%H%M%SZ).log"

echo "deploying $(basename "$payload") to the console (port ${port}); log: ${log#"${KSRE_PS5_ROOT}"/}" >&2
python3 "${KSRE_PS5_ROOT}/tools/ps5_send.py" "$PS5_HOST" "$port" "$payload" --log "$log" "$@"
