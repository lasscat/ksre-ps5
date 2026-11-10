#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Download a pinned pacbrew-repo binary bundle (SDL2, SDL2_image, SDL2_ttf,
# freetype, libpng, Mesa, ...) and unpack its library prefix into
# external/toolchains/pacbrew-<release>/ (ignored by git).
#
#   tools/fetch-pacbrew.sh hwgl    # PACBREW_HWGL_* pin: v0.41.1, Mesa 26.2 zink/RADV on AGC
#   tools/fetch-pacbrew.sh swgl    # PACBREW_SWGL_* pin: v0.40.3, Mesa 22.1.7 OSMesa (software GL)
#
# How the bundle relates to the SDK
# ---------------------------------
# The release tarball is a complete pacman-style root: it contains a copy of
# ps5-payload-sdk (bin/, toolchain/, samples/, target/include, ...) plus the
# packaged libraries under opt/ps5-payload-sdk/target/user/homebrew/. On a
# normal install that last directory *is* the SDK's "homebrew root"
# (prospero.sh: PS5_HBROOT=/user/homebrew, DESTDIR=$PS5_PAYLOAD_SDK/target).
#
# This script deliberately extracts ONLY that library prefix, into
#
#     external/toolchains/pacbrew-<release>/user/homebrew/{include,lib,bin,...}
#
# so the pinned SDK checkout (external/toolchains/ps5-payload-sdk-<release>)
# is never modified and the bundle's bundled copy of the SDK is ignored. Build
# files overlay it explicitly with -I<prefix>/include, -I<prefix>/include/SDL2
# and -L<prefix>/lib (see experiments/02a-sdl-display/Makefile). The SDK
# wrapper `prospero-clang` still does the compiling and linking, so SDK
# headers/crt/stubs come from the pinned SDK and only third-party libraries
# come from the bundle. Put the bundle's -I paths BEFORE the SDK's so the
# bundle's SDL headers win.
#
# Everything the bundle packages is also what is installed under
# /user/homebrew on the console by pacbrew; this project links the static
# libraries (.a) into the payload. The shared Mesa libraries in the bundle
# (libEGL.so, libGL.so, libgallium-*.so, libvulkan.so) are dlopen()ed by SDL
# at run time only for GL/Vulkan windows (milestone 02b) and are not needed by
# the 02a framebuffer path.
#
# The download is cached in external/toolchains/downloads/ and verified
# against the SHA-256 in upstreams.lock before anything is unpacked.
#
# Prints the PS5_PACBREW_PREFIX export line (like tools/fetch-sdk.sh).
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

usage() {
  echo "usage: tools/fetch-pacbrew.sh hwgl|swgl [--force]" >&2
  exit 2
}

[ $# -ge 1 ] || usage
variant=$1
force=0
if [ $# -ge 2 ]; then
  [ "$2" = "--force" ] || usage
  force=1
fi

case "$variant" in
  hwgl) key=PACBREW_HWGL ;;
  swgl) key=PACBREW_SWGL ;;
  *) usage ;;
esac

rel=$(lock_get "${key}_RELEASE")
url=$(lock_get "${key}_URL")
sum=$(lock_get "${key}_SHA256")
base="${KSRE_PS5_ROOT}/external/toolchains"
tarball="${base}/downloads/pacbrew-${rel}.tar.gz"
dest="${base}/pacbrew-${rel}"
member="opt/ps5-payload-sdk/target/user/homebrew"
prefix="${dest}/user/homebrew"
marker="${dest}/.fetched-sha256"

download_verified "$url" "$tarball" "$sum"

if [ "$force" = 0 ] && [ -f "$marker" ] && [ "$(cat "$marker")" = "$sum" ] &&
   [ -f "${prefix}/lib/libSDL2.a" ]; then
  echo "pacbrew ${variant} ${rel} already unpacked"
else
  # Refuse archives that could write outside the destination.
  if tar -tzf "$tarball" | grep -E -q '^/|(^|/)\.\.(/|$)'; then
    echo "refusing ${tarball}: it contains absolute or parent-relative paths" >&2
    exit 1
  fi
  tmp="${dest}.tmp"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  # Naming a directory member extracts everything below it (GNU tar and bsdtar).
  tar -xzf "$tarball" -C "$tmp" "$member"
  [ -d "${tmp}/${member}/lib" ] || { echo "unexpected bundle layout in ${tarball}" >&2; exit 1; }
  rm -rf "$dest"
  mkdir -p "${dest}/user"
  mv "${tmp}/${member}" "${prefix}"
  rm -rf "$tmp"
  echo "$sum" > "$marker"
fi

for f in include/SDL2/SDL.h include/SDL2/SDL_image.h include/SDL2/SDL_ttf.h \
         lib/libSDL2.a lib/libSDL2_image.a lib/libSDL2_ttf.a lib/libfreetype.a lib/libpng16.a; do
  [ -e "${prefix}/${f}" ] || { echo "bundle ${rel} lacks ${f}" >&2; exit 1; }
done

sdl_ver=$(sed -n 's/^#define SDL_MAJOR_VERSION[[:space:]]*//p' "${prefix}/include/SDL2/SDL_version.h" | head -1)
sdl_ver="${sdl_ver}.$(sed -n 's/^#define SDL_MINOR_VERSION[[:space:]]*//p' "${prefix}/include/SDL2/SDL_version.h" | head -1)"
sdl_ver="${sdl_ver}.$(sed -n 's/^#define SDL_PATCHLEVEL[[:space:]]*//p' "${prefix}/include/SDL2/SDL_version.h" | head -1)"
echo "pacbrew ${variant} ${rel} ready (SDL ${sdl_ver}) at ${prefix}"
echo
echo "export PS5_PACBREW_PREFIX=\"${prefix}\""
