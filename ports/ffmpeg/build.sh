#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# LGPL-only static FFmpeg for x86_64-sie-ps5 -> build/sysroot (lib/, include/).
#
#   ports/ffmpeg/build.sh        configure, build, install, verify the licence flags
#   ports/ffmpeg/build.sh clean  remove build/ffmpeg
#
# Needs PS5_PAYLOAD_SDK (tools/fetch-sdk.sh), nasm (x86 asm) and the pacbrew bundle
# (zlib only; tools/fetch-pacbrew.sh swgl). Only FFmpeg's own native decoders are used:
# no external codec libraries, no GPL/version3/nonfree options, no network.
set -euo pipefail
# shellcheck source=../../tools/lib/common.sh
. "$(dirname "$0")/../../tools/lib/common.sh"

ver=$(lock_get FFMPEG_VERSION)
tarball="${KSRE_PS5_ROOT}/external/toolchains/downloads/ffmpeg-${ver}.tar.xz"
out="${KSRE_PS5_ROOT}/build/ffmpeg"
sysroot="${KSRE_PS5_ROOT}/build/sysroot"
pacbrew_rel=$(lock_get PACBREW_SWGL_RELEASE)
pacbrew="${PS5_PACBREW_PREFIX:-${KSRE_PS5_ROOT}/external/toolchains/pacbrew-${pacbrew_rel}/user/homebrew}"

if [ "${1:-}" = clean ]; then rm -rf "$out"; exit 0; fi

if [ -z "${PS5_PAYLOAD_SDK:-}" ] || [ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-clang" ]; then
  echo "PS5_PAYLOAD_SDK is not set or not an SDK; run tools/fetch-sdk.sh and export it" >&2
  exit 2
fi
command -v nasm >/dev/null || { echo "nasm missing: brew install nasm" >&2; exit 2; }
[ -f "${pacbrew}/lib/libz.a" ] || { echo "missing ${pacbrew}/lib/libz.a; run tools/fetch-pacbrew.sh swgl" >&2; exit 2; }

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
sdk=${PS5_PAYLOAD_SDK}

download_verified "$(lock_get FFMPEG_URL)" "$tarball" "$(lock_get FFMPEG_SHA256)"
rm -rf "${out}/src"
mkdir -p "${out}/src" "$sysroot"
tar -xJf "$tarball" -C "${out}/src" --strip-components=1

# Muxers/encoders/filters are off: Ren'Py only demuxes and decodes through its own AVIO.
demuxers="au avi flac m4v matroska mov mp3 mpegps mpegts mpegvideo ogg wav"
decoders="flac mp2 mp3 mp3on4 mpeg1video mpeg2video mpeg4 msmpeg4v1 msmpeg4v2 msmpeg4v3 \
pcm_dvd pcm_s16be pcm_s16le pcm_s8 pcm_u16be pcm_u16le pcm_u8 theora vorbis opus vp3 vp8 vp9"
parsers="flac mpegaudio mpegvideo mpeg4video opus vorbis vp3 vp8 vp9"
opts=()
for x in $demuxers; do opts+=("--enable-demuxer=$x"); done
for x in $decoders; do opts+=("--enable-decoder=$x"); done
for x in $parsers; do opts+=("--enable-parser=$x"); done

echo "configuring FFmpeg ${ver} (log: build/ffmpeg/configure.log)"
(
  cd "${out}/src"
  env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CPPFLAGS -u LIBS \
    ./configure \
      --prefix="$sysroot" \
      --enable-cross-compile --arch=x86_64 --target-os=freebsd \
      --cc="${sdk}/bin/prospero-clang" --cxx="${sdk}/bin/prospero-clang++" \
      --ld="${sdk}/bin/prospero-clang" --ar="${sdk}/bin/prospero-ar" \
      --ranlib="${sdk}/bin/prospero-ranlib" --strip="${sdk}/bin/prospero-strip" \
      --nm="${sdk}/bin/prospero-nm" --x86asmexe=nasm \
      --extra-cflags="-I${pacbrew}/include -Wno-unknown-warning-option" \
      --extra-ldflags="-L${pacbrew}/lib" \
      --pkg-config=false \
      --enable-static --disable-shared --enable-pic --enable-runtime-cpudetect \
      --disable-autodetect --disable-programs --disable-doc --disable-network \
      --disable-debug --disable-avdevice --disable-avfilter --disable-postproc \
      --disable-everything --enable-zlib \
      --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale \
      "${opts[@]}" \
    > "${out}/configure.log" 2>&1 || { tail -40 "${out}/configure.log"; exit 1; }
  echo "building (log: build/ffmpeg/make.log)"
  make -j"$jobs" > "${out}/make.log" 2>&1 || { tail -60 "${out}/make.log"; exit 1; }
  make install > "${out}/install.log" 2>&1 || { tail -40 "${out}/install.log"; exit 1; }
)

# Licence gate: the configuration must be plain LGPL 2.1+.
cfg="${out}/src/config.h"
for flag in CONFIG_GPL CONFIG_VERSION3 CONFIG_NONFREE; do
  if ! grep -q "^#define ${flag} 0" "$cfg"; then
    echo "licence check FAILED: ${flag} is not 0 in config.h" >&2
    exit 1
  fi
done
grep '^#define FFMPEG_LICENSE' "$cfg"
{
  echo "FFmpeg ${ver}"
  grep '^#define FFMPEG_LICENSE' "$cfg"
  grep '^#define FFMPEG_CONFIGURATION' "$cfg"
} > "${sysroot}/ffmpeg-config.txt"
echo "done: ${sysroot}/lib/libavcodec.a libavformat.a libavutil.a libswscale.a libswresample.a"
