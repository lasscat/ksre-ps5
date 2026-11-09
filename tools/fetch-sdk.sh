#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Download and unpack the pinned ps5-payload-sdk binary release into
# external/toolchains/ (ignored by git). No root access or /opt install is
# needed: the SDK's wrapper scripts locate themselves.
#
# Usage: tools/fetch-sdk.sh            # prints the PS5_PAYLOAD_SDK export line
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

rel=$(lock_get PS5_SDK_RELEASE)
url=$(lock_get PS5_SDK_ZIP_URL)
sum=$(lock_get PS5_SDK_ZIP_SHA256)
base="${KSRE_PS5_ROOT}/external/toolchains"
zip="${base}/downloads/ps5-payload-sdk-${rel}.zip"
dest="${base}/ps5-payload-sdk-${rel}"

download_verified "$url" "$zip" "$sum"

if [ ! -f "${dest}/ps5-payload-sdk/target/lib/libc.a" ]; then
  rm -rf "${dest}.tmp"
  mkdir -p "${dest}.tmp"
  unzip -q "$zip" -d "${dest}.tmp"
  rm -rf "$dest"
  mv "${dest}.tmp" "$dest"
fi

sdk="${dest}/ps5-payload-sdk"
[ -x "${sdk}/bin/prospero-clang" ] || { echo "unexpected SDK layout in $dest" >&2; exit 1; }
echo "SDK ${rel} ready at ${sdk}"
echo
echo "export PS5_PAYLOAD_SDK=\"${sdk}\""
