#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Report which host prerequisites are present, per milestone, and print the
# exact commands to install what is missing. Changes nothing on the system.
#
# Usage: tools/check-env.sh [--milestone 1|2|3|all]
#   1   : build and deploy the runtime probe (default)
#   2   : SDL/GL experiments with the pacbrew dependency bundles
#   3   : CPython / Ren'Py cross-builds (adds autotools, Python 3.12, Cython)
# Exit status: 0 when everything required for the chosen milestone is present.
set -uo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

milestone=1
while [ $# -gt 0 ]; do
  case "$1" in
    --milestone) milestone=${2:-1}; shift 2 ;;
    -h|--help) sed -n '3,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ "$milestone" = all ] && milestone=9

os=$(uname -s); arch=$(uname -m)
missing_required=()
missing_brew=(); missing_apt=()
notes=()

warn() { printf '  [warn]    %-26s %s\n' "$1" "${2:-}"; }
ok()   { printf '  [ok]      %-26s %s\n' "$1" "${2:-}"; }
miss() { printf '  [MISSING] %-26s %s\n' "$1" "${2:-}"; }
opt()  { printf '  [later]   %-26s %s\n' "$1" "${2:-}"; }

need() { # need MIN_MILESTONE NAME BREW_PKG APT_PKG [found-detail]
  local min=$1 name=$2 brew=$3 apt=$4 detail=${5:-}
  if [ -n "$detail" ]; then ok "$name" "$detail"; return; fi
  if [ "$milestone" -ge "$min" ]; then
    miss "$name" "(needed from milestone $min)"
    missing_required+=("$name")
    [ -n "$brew" ] && missing_brew+=("$brew")
    [ -n "$apt" ] && missing_apt+=("$apt")
  else
    opt "$name" "(needed from milestone $min)"
  fi
}

ver_of() { "$@" 2>/dev/null | head -1; }

echo "ksre-ps5 environment check (milestone ${milestone/9/all})"
echo
echo "Host"
if [ "$os" = Darwin ]; then
  ok "OS" "macOS $(sw_vers -productVersion 2>/dev/null) ($arch)"
  [ "$arch" = arm64 ] && notes+=("Apple Silicon host: host tools are arm64; every PS5 artifact is x86_64-sie-ps5 and is built only through the SDK wrappers.")
elif [ "$os" = Linux ]; then
  ok "OS" "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Linux}") ($arch)"
else
  miss "OS" "$os is untested; use macOS or Debian/Ubuntu"
fi
free_gb=$(df -Pk "$KSRE_PS5_ROOT" | awk 'NR==2 {printf "%d", $4/1048576}')
if [ "${free_gb:-0}" -ge 30 ]; then ok "free disk" "${free_gb} GiB"; else warn "free disk" "${free_gb} GiB (30+ GiB recommended once dependencies and game data are local)"; fi

echo
echo "Core tools"
for t in git curl make unzip python3; do
  if command -v "$t" >/dev/null 2>&1; then ok "$t" "$(command -v "$t")"; else need 1 "$t" "$t" "$t"; fi
done
if command -v python3 >/dev/null 2>&1; then
  pyv=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
  if python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)'; then ok "python3 >= 3.8" "$pyv"; else need 1 "python3 >= 3.8" "python@3.12" "python3"; fi
fi
if command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1; then ok "sha256" "available"; else need 1 "sha256sum/shasum" "coreutils" "coreutils"; fi

echo
echo "PS5 cross toolchain (host LLVM used by the SDK wrappers)"
llvm_config=""
if [ -n "${LLVM_CONFIG:-}" ] && [ -x "${LLVM_CONFIG}" ]; then
  llvm_config=$LLVM_CONFIG
else
  for c in llvm-config-23 llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15 llvm-config; do
    if command -v "$c" >/dev/null 2>&1; then llvm_config=$(command -v "$c"); break; fi
  done
fi
brew_llvm18=""
if [ "$os" = Darwin ] && command -v brew >/dev/null 2>&1; then
  for p in /opt/homebrew/opt/llvm@18 /usr/local/opt/llvm@18 /opt/homebrew/opt/llvm /usr/local/opt/llvm; do
    if [ -x "$p/bin/llvm-config" ]; then brew_llvm18="$p/bin/llvm-config"; break; fi
  done
fi
if [ -n "$llvm_config" ]; then
  ok "llvm-config" "$llvm_config ($(ver_of "$llvm_config" --version))"
  lld="$("$llvm_config" --bindir)/ld.lld"
  if [ -x "$lld" ]; then ok "ld.lld" "$lld"
  elif [ "$os" = Darwin ] && command -v brew >/dev/null 2>&1 && [ -x "$(brew --prefix lld 2>/dev/null)/bin/ld.lld" ]; then ok "ld.lld" "$(brew --prefix lld)/bin/ld.lld"
  else need 1 "ld.lld" "lld" "lld-18"; fi
elif [ -n "$brew_llvm18" ]; then
  miss "llvm-config" "installed at $brew_llvm18 but not visible to the SDK"
  missing_required+=("LLVM_CONFIG export")
  notes+=("Homebrew LLVM is keg-only. Run: export LLVM_CONFIG=\"$brew_llvm18\"")
else
  need 1 "host LLVM (clang+lld)" "llvm@18" "clang-18 lld-18 llvm-18"
  [ "$os" = Darwin ] && notes+=("After 'brew install llvm@18' run: export LLVM_CONFIG=\"\$(brew --prefix llvm@18)/bin/llvm-config\" (llvm@18 matches the SDK's own CI and includes ld.lld).")
fi
if [ "$os" = Darwin ] && clang -target x86_64-sie-ps5 -dM -E - </dev/null 2>/dev/null | grep -q __PROSPERO__; then
  ok "compile-only cross check" "system clang knows x86_64-sie-ps5 (make ps5-compile-check)"
fi

sdk=${PS5_PAYLOAD_SDK:-}
[ -z "$sdk" ] && [ -d "$(ksre_default_sdk_dir)" ] && sdk=$(ksre_default_sdk_dir)
if [ -n "$sdk" ] && [ -x "$sdk/bin/prospero-clang" ] && [ -f "$sdk/target/lib/libc.a" ]; then
  ok "ps5-payload-sdk" "$sdk"
  [ -z "${PS5_PAYLOAD_SDK:-}" ] && notes+=("SDK found but PS5_PAYLOAD_SDK is not exported. Run: export PS5_PAYLOAD_SDK=\"$sdk\"")
  if "$sdk/bin/prospero-llvm-config" --version >/dev/null 2>&1; then
    ok "SDK finds host LLVM" "$("$sdk/bin/prospero-llvm-config" --version)"
  else
    miss "SDK finds host LLVM" "prospero-llvm-config failed (install LLVM / export LLVM_CONFIG)"
    missing_required+=("SDK host LLVM")
  fi
else
  miss "ps5-payload-sdk" "not found; run tools/fetch-sdk.sh"
  missing_required+=("ps5-payload-sdk")
fi

echo
echo "Console transfer"
ok "payload sender" "tools/ps5_send.py (Python stdlib; socat optional)"
if [ -f "${KSRE_PS5_ROOT}/.local-config/console.env" ]; then
  load_console_env
  if [ -n "${PS5_HOST:-}" ]; then ok "console config" ".local-config/console.env (PS5_HOST set, port ${PS5_PORT:-9021})"; else miss "console config" "PS5_HOST missing in .local-config/console.env"; fi
else
  opt "console config" "copy config/console.env.example to .local-config/console.env (needed to deploy)"
fi

echo
echo "Later milestones"
for spec in "2 cmake cmake cmake" "2 ninja ninja ninja-build" "2 meson meson meson" "2 pkg-config pkgconf pkg-config" \
            "3 nasm nasm nasm" "3 autoconf autoconf autoconf" "3 automake automake automake" \
            "3 libtool libtool libtool" "3 patch gpatch patch" "3 xz xz xz-utils"; do
  # shellcheck disable=SC2086 # intentional word splitting of the spec
  set -- $spec
  if command -v "$2" >/dev/null 2>&1 || { [ "$2" = libtool ] && command -v glibtool >/dev/null 2>&1; }; then ok "$2"; else need "$1" "$2" "$3" "$4"; fi
done
if command -v python3.12 >/dev/null 2>&1; then ok "python3.12" "$(command -v python3.12) (build Python for CPython $(lock_get CPYTHON_VERSION))"
else need 3 "python3.12 (build Python)" "python@3.12" "python3.12 python3.12-venv"; fi

echo
for n in ${notes[@]+"${notes[@]}"}; do echo "note: $n"; done
if [ ${#missing_required[@]} -eq 0 ]; then
  echo "RESULT: everything required for milestone ${milestone/9/all} is present."
  exit 0
fi
echo "RESULT: missing for milestone ${milestone/9/all}: ${missing_required[*]}"
if [ "$os" = Darwin ] && [ ${#missing_brew[@]} -gt 0 ]; then
  echo "install (macOS): brew install $(printf '%s\n' "${missing_brew[@]}" | sort -u | tr '\n' ' ')"
elif [ "$os" = Linux ] && [ ${#missing_apt[@]} -gt 0 ]; then
  echo "install (Debian/Ubuntu): sudo apt-get install $(printf '%s\n' "${missing_apt[@]}" | sort -u | tr '\n' ' ')"
fi
case " ${missing_required[*]} " in *" ps5-payload-sdk "*) echo "then: tools/fetch-sdk.sh" ;; esac
exit 1
