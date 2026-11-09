# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
# Shared helpers for ksre-ps5 shell tools. Source, do not execute.

KSRE_PS5_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export KSRE_PS5_ROOT

# Default location of the pinned SDK unpacked by tools/fetch-sdk.sh.
ksre_default_sdk_dir() {
  echo "${KSRE_PS5_ROOT}/external/toolchains/ps5-payload-sdk-$(lock_get PS5_SDK_RELEASE)/ps5-payload-sdk"
}

# lock_get KEY -> value from upstreams.lock (parsed, never executed).
lock_get() {
  local key=$1 line
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      "${key}="*) echo "${line#*=}"; return 0 ;;
    esac
  done < "${KSRE_PS5_ROOT}/upstreams.lock"
  echo "upstreams.lock has no key ${key}" >&2
  return 1
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

# download URL DEST SHA256 : download to DEST unless it already matches.
download_verified() {
  # Optional 4th argument: a GitHub REST API asset URL tried if the primary
  # URL fails. GITHUB_TOKEN, if set, authenticates that API request only.
  local url=$1 dest=$2 want=$3 fallback=${4:-} got
  if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$want" ]; then
    echo "already present and verified: $dest"
    return 0
  fi
  mkdir -p "$(dirname "$dest")"
  echo "downloading $url"
  if ! curl -fL --retry 3 --retry-delay 5 --retry-connrefused -o "${dest}.part" "$url"; then
    rm -f "${dest}.part"
    [ -n "$fallback" ] || return 1
    echo "primary URL failed; trying the API asset URL $fallback"
    local hdr=(-H "Accept: application/octet-stream")
    if [ -n "${GITHUB_TOKEN:-}" ]; then
      hdr+=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
    fi
    if ! curl -fL --retry 6 --retry-delay 10 --retry-connrefused "${hdr[@]}" -o "${dest}.part" "$fallback"; then
      rm -f "${dest}.part"
      return 1
    fi
  fi
  got=$(sha256_of "${dest}.part")
  if [ "$got" != "$want" ]; then
    rm -f "${dest}.part"
    echo "SHA-256 mismatch for $url" >&2
    echo "  expected $want" >&2
    echo "  got      $got" >&2
    return 1
  fi
  mv "${dest}.part" "$dest"
  echo "verified sha256 $got"
}

# Load private console settings (never tracked) if present.
load_console_env() {
  local f="${KSRE_PS5_ROOT}/.local-config/console.env"
  if [ -f "$f" ]; then
    # Accept only simple KEY=VALUE lines for known keys.
    local line key val
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in ''|\#*) continue ;; esac
      key=${line%%=*}
      val=${line#*=}
      val=${val%\"}; val=${val#\"}
      case "$key" in
        PS5_HOST|PS5_PORT|PS5_FTP_PORT|PS5_KLOG_PORT) export "$key=$val" ;;
      esac
    done < "$f"
  fi
}
