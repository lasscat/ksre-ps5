#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ren'Py 8.5.3 for the PS5 (milestone 04a): Ren'Py's native modules as a static library,
# our launcher, and the payload link. Same method as renpy-build (RENPY_STATIC=1 generate,
# compile, archive, inittab), written for this project; renpy-build itself is not used.
#
#   ports/renpy/build.sh            all steps below -> build/ports/renpy/ksre-ps5-renpy.elf
#   ports/renpy/build.sh generate   venv (Cython) + out-of-tree `RENPY_STATIC=1 setup.py generate`
#   ports/renpy/build.sh lib        librenpy.a + generated inittab (runs generate if needed)
#   ports/renpy/build.sh elf        launcher.c + link
#   ports/renpy/build.sh runtime    console file tree: Ren'Py pure Python + six/ecdsa + stub
#   ports/renpy/build.sh testgame   fixtures/testgame + generated assets -> build/ports/renpy/testgame
#   ports/renpy/build.sh host       the same modules + launcher built for this machine (twin)
#   ports/renpy/build.sh selftest-host   run the test game's unattended self-test on the twin
#   ports/renpy/build.sh fetch-sdk  official Ren'Py SDK zip (pinned) -> external/toolchains/renpy-sdk
#   ports/renpy/build.sh clean      remove build/ports/renpy
#
# Needs: PS5_PAYLOAD_SDK + host LLVM, the pinned Ren'Py checkout (tools/fetch-upstreams.sh
# renpy), pacbrew v0.40.3 (tools/fetch-pacbrew.sh swgl), ports/build-all-deps.sh (sysroot),
# ports/cpython/build.sh host + ps5, and a host python3 with venv + network for pip.
set -euo pipefail
# shellcheck source=../../tools/lib/common.sh
. "$(dirname "$0")/../../tools/lib/common.sh"

root=${KSRE_PS5_ROOT}
here="${root}/ports/renpy"
out="${root}/build/ports/renpy"
tree="${out}/tree"
gen="${tree}/tmp/gen3-static"
venv="${out}/venv"
commit=$(lock_get RENPY_COMMIT)
cython_ver=$(lock_get CYTHON_VERSION)
pacbrew_rel=$(lock_get PACBREW_SWGL_RELEASE)
pb="${PS5_PACBREW_PREFIX:-${root}/external/toolchains/pacbrew-${pacbrew_rel}/user/homebrew}"
sysroot="${root}/build/sysroot"
py_embed="${root}/build/cpython/ps5/embed.mk"
host_python="${root}/build/host-python/bin/python3.12"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
mkdir -p "$out"

# Modules in src/Setup that this port does not build (see ports/renpy/README.md):
#   renpy.gl2.assimp      needs the assimp C++ library; replaced by a Python stub
#   renpy.gl2.live2dmodel needs the proprietary Live2D Cubism SDK
skip_modules=" renpy.gl2.assimp renpy.gl2.live2dmodel "

# --- generate ---------------------------------------------------------------
make_venv() {
  if [ -x "${venv}/bin/cython" ] && "${venv}/bin/cython" --version 2>&1 | grep -q " ${cython_ver}\$"; then
    return 0
  fi
  local py=${RENPY_VENV_PYTHON:-}
  if [ -z "$py" ]; then
    for c in python3.12 python3; do
      if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import ssl, venv, ensurepip' 2>/dev/null; then py=$c; break; fi
    done
  fi
  [ -n "$py" ] || { echo "need a host python3 with ssl, venv and ensurepip (set RENPY_VENV_PYTHON)" >&2; exit 2; }
  rm -rf "$venv"
  "$py" -m venv "$venv"
  "${venv}/bin/pip" install -q --disable-pip-version-check "Cython==${cython_ver}" "setuptools==80.9.0"
  "${venv}/bin/cython" --version
}

make_tree() {
  local src="${root}/external/renpy"
  if [ -f "${tree}/.commit" ] && [ "$(cat "${tree}/.commit")" = "$commit" ]; then
    return 0
  fi
  git -C "$src" cat-file -e "${commit}^{commit}" 2>/dev/null ||
    { echo "pinned Ren'Py commit missing; run tools/fetch-upstreams.sh renpy" >&2; exit 2; }
  rm -rf "$tree"
  mkdir -p "$tree"
  # A clean copy of the pinned commit: generation writes into the tree (tmp/, styles).
  git -C "$src" archive "$commit" | tar -x -C "$tree"
  echo "$commit" > "${tree}/.commit"
}

do_generate() {
  make_venv
  make_tree
  if [ -f "${gen}/.stamp" ] && [ "$(cat "${gen}/.stamp")" = "${commit} cython-${cython_ver}" ]; then
    echo "generated C up to date (${gen#"${root}"/})"
    return 0
  fi
  echo "generating Cython C (log: build/ports/renpy/generate.log)"
  # CFLAGS/LDFLAGS set so setup.py does not call pkg-config; generate does not compile.
  (cd "$tree" && env PATH="${venv}/bin:${PATH}" RENPY_STATIC=1 RENPY_REGENERATE_CYTHON=1 \
      RENPY_CYTHON="${venv}/bin/cython" CFLAGS= LDFLAGS= "${venv}/bin/python" setup.py generate) \
    > "${out}/generate.log" 2>&1 || { tail -40 "${out}/generate.log"; exit 1; }
  echo "${commit} cython-${cython_ver}" > "${gen}/.stamp"
  echo "generated $(find "$gen" -name '*.c' -o -name '*.cc' | wc -l | tr -d ' ') C/C++ files"
}

# --- lib --------------------------------------------------------------------
# write_makefile TARGET: objects for every module in src/Setup except $skip_modules.
write_makefile() {
  local target=$1 odir="${out}/obj-$1" mk modules=() srcs=() line name rest f
  mkdir -p "$odir"
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%%#*}
    # shellcheck disable=SC2086
    set -- $line
    [ $# -gt 0 ] || continue
    name=$1; shift
    case "$skip_modules" in *" ${name} "*) continue ;; esac
    modules+=("$name")
    for rest in "$@"; do
      f=${rest/..\/tmp\/gen\//..\/tmp\/gen3-static\/}
      srcs+=("${tree}/src/${f}")
    done
  done < "${tree}/src/Setup"

  {
    echo '#include "Python.h"'
    echo "/* Generated by ports/renpy/build.sh from Ren'Py src/Setup; do not edit. */"
    for m in "${modules[@]}"; do echo "PyMODINIT_FUNC PyInit_${m//./_}(void);"; done
    echo "static struct _inittab ksre_renpy_inittab[] = {"
    for m in "${modules[@]}"; do echo "    {\"${m}\", PyInit_${m//./_}},"; done
    echo "    {NULL, NULL},"
    echo "};"
    echo "int ksre_renpy_module_count = ${#modules[@]};"
    echo "/* Register the modules as built-ins; call before Py_Initialize*. */"
    echo "int init_librenpy(void) { return PyImport_ExtendInittab(ksre_renpy_inittab); }"
  } > "${odir}/inittab.c"
  printf '%s\n' "${modules[@]}" > "${odir}/modules.txt"

  mk="${odir}/Makefile"
  {
    echo "# Generated by ports/renpy/build.sh; do not edit."
    printf 'OBJS :='
    for f in "${srcs[@]}"; do b=$(basename "$f"); printf ' %s' "${b%.*}.o"; done
    echo " inittab.o"
    echo "librenpy.a: \$(OBJS)"
    echo "	rm -f \$@ && \$(AR) rcs \$@ \$(OBJS)"
    for f in "${srcs[@]}"; do
      b=$(basename "$f")
      case "$b" in
        *.cc) echo "${b%.*}.o: ${f}"; echo "	\$(CXX) \$(CXXFLAGS) -c \$< -o \$@" ;;
        *)    echo "${b%.*}.o: ${f}"; echo "	\$(CC) \$(CFLAGS) -c \$< -o \$@" ;;
      esac
    done
    echo "inittab.o: inittab.c"
    echo "	\$(CC) \$(CFLAGS) -c \$< -o \$@"
  } > "$mk"
  echo "${target}: ${#modules[@]} modules, ${#srcs[@]} sources"
}

need_ps5() {
  if [ -z "${PS5_PAYLOAD_SDK:-}" ] || [ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-clang" ]; then
    echo "PS5_PAYLOAD_SDK is not set (source .local-config/env.sh or tools/fetch-sdk.sh)" >&2; exit 2
  fi
  [ -f "$py_embed" ] || { echo "no PS5 CPython: ports/cpython/build.sh host && ports/cpython/build.sh ps5" >&2; exit 2; }
  [ -f "${sysroot}/lib/libavformat.a" ] || { echo "no sysroot: run ports/build-all-deps.sh" >&2; exit 2; }
  [ -f "${pb}/lib/libSDL2.a" ] || { echo "no pacbrew ${pacbrew_rel}: run tools/fetch-pacbrew.sh swgl" >&2; exit 2; }
}

# make_var NAME: a variable from build/cpython/ps5/embed.mk
make_var() { sed -n "s/^$1 := //p" "$py_embed"; }

ps5_cflags() {
  # Same include set for Ren'Py's C, the generated C and the launcher.
  echo "-O2 -gline-tables-only -DRENPY_BUILD -Wno-unused-function -Wno-unknown-warning-option" \
       "-Wno-deprecated-declarations -fno-strict-aliasing" \
       "-I${tree}/src -I${gen} $(make_var PY_PS5_INCLUDES)" \
       "-I${sysroot}/include -I${sysroot}/include/freetype2 -I${sysroot}/include/harfbuzz" \
       "-I${sysroot}/include/fribidi -I${pb}/include -I${pb}/include/SDL2"
}

do_lib() {
  need_ps5
  do_generate
  write_makefile ps5
  local sdk=${PS5_PAYLOAD_SDK} cflags
  cflags=$(ps5_cflags)
  echo "compiling librenpy.a (log: build/ports/renpy/obj-ps5/make.log)"
  make -C "${out}/obj-ps5" -j"$jobs" \
    CC="${sdk}/bin/prospero-clang" CXX="${sdk}/bin/prospero-clang++" AR="${sdk}/bin/prospero-ar" \
    CFLAGS="-std=gnu11 ${cflags}" CXXFLAGS="${cflags}" > "${out}/obj-ps5/make.log" 2>&1 ||
    { grep -E 'error' "${out}/obj-ps5/make.log" | head -40; exit 1; }
  echo "librenpy.a: $(du -h "${out}/obj-ps5/librenpy.a" | cut -f1), warnings: $(grep -c 'warning:' "${out}/obj-ps5/make.log" || true)"
}

# --- elf --------------------------------------------------------------------
# write_imports FILE [ELF]: C table of the ELF's undefined (imported) symbols for the
# launcher's start-up check; empty without an ELF (first link pass, host twin).
write_imports() {
  {
    echo "/* Generated by ports/renpy/build.sh; do not edit. */"
    echo "const char *const ksre_imports[] = {"
    if [ -n "${2:-}" ]; then
      "${PS5_PAYLOAD_SDK}/bin/prospero-nm" -u "$2" | awk '$1 == "U" { printf "    \"%s\",\n", $2 }' | sort -u
    fi
    echo "    0};"
    echo "const int ksre_import_count = sizeof ksre_imports / sizeof ksre_imports[0] - 1;"
  } > "$1"
}

do_elf() {
  need_ps5
  [ -f "${out}/obj-ps5/librenpy.a" ] || do_lib
  local sdk=${PS5_PAYLOAD_SDK} elf="${out}/ksre-ps5-renpy.elf" cflags pass
  cflags=$(ps5_cflags)
  # shellcheck disable=SC2046,SC2086
  "${sdk}/bin/prospero-clang" -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -Werror=implicit-function-declaration ${cflags} -c "${here}/launcher.c" -o "${out}/launcher.o"
  write_imports "${out}/imports.c"
  for pass in 1 2; do
    "${sdk}/bin/prospero-clang" -c "${out}/imports.c" -o "${out}/imports.o"
    # clang++ (HarfBuzz is C++), -rdynamic (R13: Mesa is dlopen()ed). -L sysroot first:
    # pacbrew also has an FFmpeg (GPL, with TLS) that must never be linked, and
    # PY_PS5_LIBS brings its own -L for the pacbrew lib directory. No -lm: math comes
    # from the console's libSceLibcInternal like CPython's (03a), never pacbrew openlibm.
    # shellcheck disable=SC2046,SC2086
    "${sdk}/bin/prospero-clang++" -rdynamic -L"${sysroot}/lib" -o "$elf" "${out}/launcher.o" \
      "${out}/imports.o" "${out}/obj-ps5/librenpy.a" $(make_var PY_PS5_LIBS) -L"${pb}/lib" \
      -lSDL2_image -lwebpdemux -lwebp -lsharpyuv -lpng16 -ljpeg \
      -lavformat -lavcodec -lswscale -lswresample -lavutil \
      -lharfbuzz -lfreetype -lfribidi -lbz2 -llzma -lz \
      -lSDL2 -liconv -lsamplerate -lSceSystemService -lSceUserService -lScePad -lSceAudioOut \
      -lSceVideoOut -lSceKeyboard -lSceImeDialog -lpthread \
      > "${out}/link.log" 2>&1 || { tail -60 "${out}/link.log"; exit 1; }
    [ "$pass" = 2 ] || write_imports "${out}/imports.c" "$elf"
  done
  file "$elf"
  echo "imports $(grep -c '^    "' "${out}/imports.c")  size $(du -h "$elf" | cut -f1)  sha256 $(sha256_of "$elf")"
}

# --- runtime ----------------------------------------------------------------
# Console tree (renpy_base): renpy.py, renpy/ (Python + Ren'Py's .rpy common files),
# lib/python/ (six, ecdsa) and the assimp stub, with checked-hash .pyc files compiled
# by the host build Python 3.12.8 (same bytecode magic as the console interpreter).
fetch_wheel() { # NAME -> path of the verified wheel
  local up url sha dest
  up=$(echo "$1" | tr '[:lower:]' '[:upper:]')
  url=$(lock_get "${up}_WHEEL_URL"); sha=$(lock_get "${up}_WHEEL_SHA256")
  dest="${root}/external/toolchains/downloads/$(basename "$url")"
  download_verified "$url" "$dest" "$sha" >&2
  echo "$dest"
}

do_runtime() {
  make_tree
  [ -x "$host_python" ] || { echo "no host build Python: ports/cpython/build.sh host" >&2; exit 2; }
  local rt="${out}/runtime/renpy"
  rm -rf "${out}/runtime"
  mkdir -p "${rt}/lib/python"
  cp "${tree}/renpy.py" "${rt}/"
  # Ren'Py's package without Cython sources/stubs (those modules are built in).
  (cd "$tree" && find renpy -type f ! -name '*.pyx' ! -name '*.pxd' ! -name '*.pxi' \
      ! -name '*.pyi' ! -name '*.c' ! -name '*.h' ! -name '*.rpyc' ! -name '*.rpymc' \
      ! -path '*/__pycache__/*' -print | sort) > "${out}/runtime-files.txt"
  (cd "$tree" && tar -cf - -T "${out}/runtime-files.txt") | tar -xf - -C "$rt"
  cp "${here}/stubs/assimp.py" "${rt}/renpy/gl2/assimp.py"
  # The SDK has renpy/vc_version.py (generated at release time from git). Without it Ren'Py
  # takes the `main` branch numbers (8.6.0). Same values as the official 8.5.3 SDK except
  # `official`, which this build is not.
  local tag name
  tag=$(lock_get RENPY_TAG)
  name=$(sed -n "s/^Version(\"fix\", 3, \"${tag%.*}\", \"\(.*\)\")\$/\1/p" "${tree}/renpy/versions.py")
  [ -n "$name" ] || { echo "no fix-branch ${tag%.*} entry in renpy/versions.py" >&2; exit 1; }
  printf "branch = 'fix'\nnightly = False\nofficial = False\nversion = '%s'\nversion_name = '%s'\n" \
    "$tag" "$name" > "${rt}/renpy/vc_version.py"
  local w
  for w in six ecdsa; do
    (cd "${rt}/lib/python" && "$host_python" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall()' "$(fetch_wheel "$w")")
  done
  rm -rf "${rt}"/lib/python/*.dist-info
  "$host_python" -m compileall -q -j0 --invalidation-mode checked-hash "${rt}/renpy" "${rt}/lib/python" >/dev/null
  echo "runtime: ${rt#"${root}"/} ($(du -sh "$rt" | cut -f1), $(find "$rt" -name '*.py' | wc -l | tr -d ' ') .py)"
}

# --- test game ----------------------------------------------------------------
# fixtures/testgame (original) + its generated images/sounds -> build/ports/renpy/testgame
do_testgame() {
  local tg="${out}/testgame"
  rm -rf "$tg"
  mkdir -p "$tg"
  cp -R "${root}/fixtures/testgame/game" "${tg}/"
  python3 "${root}/fixtures/testgame/make_assets.py" "${tg}/game"
}

# --- host twin --------------------------------------------------------------
# The same generated C, Ren'Py C sources, inittab and launcher, built for this machine
# against build/host-python and pkg-config libraries (FFmpeg, SDL2, SDL2_image, FreeType,
# HarfBuzz). FriBidi is built from the pinned source because Homebrew does not install
# it with those. A logic check of the static-module build and the launcher, not PS5 evidence.
host_fribidi() {
  local deps="${out}/host-deps" ver tarball
  [ -f "${deps}/lib/pkgconfig/fribidi.pc" ] && return 0
  ver=$(lock_get FRIBIDI_VERSION)
  tarball="${root}/external/toolchains/downloads/fribidi-${ver}.tar.xz"
  download_verified "$(lock_get FRIBIDI_URL)" "$tarball" "$(lock_get FRIBIDI_SHA256)"
  rm -rf "${out}/host-fribidi"
  mkdir -p "${out}/host-fribidi"
  tar -xJf "$tarball" -C "${out}/host-fribidi" --strip-components=1
  (cd "${out}/host-fribidi" &&
    ./configure --prefix="$deps" --enable-static --disable-shared --disable-docs &&
    make -j"$jobs" && make install) > "${out}/host-fribidi.log" 2>&1 ||
    { tail -30 "${out}/host-fribidi.log"; exit 1; }
}

do_host() {
  do_generate
  [ -x "$host_python" ] || { echo "no host build Python: ports/cpython/build.sh host" >&2; exit 2; }
  host_fribidi
  export PKG_CONFIG_PATH="${out}/host-deps/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  local pkgs="libavformat libavcodec libavutil libswresample libswscale harfbuzz freetype2 fribidi sdl2 SDL2_image libpng libjpeg zlib"
  # shellcheck disable=SC2086
  pkg-config --exists $pkgs || { echo "host twin needs pkg-config packages: $pkgs" >&2; exit 2; }
  local pycfg="${root}/build/host-python/bin/python3.12-config" cflags hcc=${HOST_CC:-cc}
  # shellcheck disable=SC2086
  cflags="-O1 -g -DRENPY_BUILD -Wno-unused-function -Wno-deprecated-declarations -fno-strict-aliasing \
-I${tree}/src -I${gen} -I${out}/host-deps/include $("$pycfg" --includes) $(pkg-config --cflags $pkgs)"
  write_makefile host
  echo "compiling host librenpy.a (log: build/ports/renpy/obj-host/make.log)"
  make -C "${out}/obj-host" -j"$jobs" CC="$hcc" CXX="${HOST_CXX:-c++}" AR=ar \
    CFLAGS="-std=gnu11 ${cflags}" CXXFLAGS="${cflags}" > "${out}/obj-host/make.log" 2>&1 ||
    { grep -E 'error' "${out}/obj-host/make.log" | head -40; exit 1; }
  write_imports "${out}/obj-host/imports.c"
  # shellcheck disable=SC2046,SC2086
  "$hcc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter ${cflags} -DKSRE_ROOT="\"${out}/host-root\"" \
    -o "${out}/ksre-ps5-renpy-host" "${here}/launcher.c" "${out}/obj-host/imports.c" \
    "${out}/obj-host/librenpy.a" $("$pycfg" --ldflags --embed) $(pkg-config --libs $pkgs) \
    -lpthread -rdynamic > "${out}/link-host.log" 2>&1 || { tail -40 "${out}/link-host.log"; exit 1; }
  echo "host twin: ${out#"${root}"/}/ksre-ps5-renpy-host (default --root build/ports/renpy/host-root)"
}

# Official Ren'Py 8.5.3 SDK for the reference run on the development machine (never shipped).
do_fetch_sdk() {
  local zip dest="${root}/external/toolchains/renpy-sdk"
  zip="${root}/external/toolchains/downloads/$(basename "$(lock_get RENPY_SDK_URL)")"
  download_verified "$(lock_get RENPY_SDK_URL)" "$zip" "$(lock_get RENPY_SDK_SHA256)"
  if [ ! -d "${dest}/$(basename "$zip" .zip)" ]; then
    mkdir -p "$dest"
    (cd "$dest" && unzip -q "$zip")
  fi
  echo "SDK: ${dest}/$(basename "$zip" .zip) (reference command: fixtures/testgame/README.md)"
}

# Unattended self-test of the test game on the host twin (opens a window for a few seconds).
do_selftest_host() {
  [ -x "${out}/ksre-ps5-renpy-host" ] || do_host
  [ -f "${out}/runtime/renpy/renpy.py" ] || do_runtime
  [ -d "${out}/testgame/game" ] || do_testgame
  local log="${out}/selftest-host.log"
  "${out}/ksre-ps5-renpy-host" --root "${out}/host-root" --python-home "${root}/build/host-python" \
    --renpy-base "${out}/runtime/renpy" --game "${out}/testgame" \
    --env KSRE_TESTGAME_SELFTEST=1 --max-seconds 90 > "$log" 2>&1 || true
  grep -a '^TESTGAME \|^RENPY step\|^KSRE_PS5_RENPY_END' "$log"
  grep -q '^KSRE_PS5_RENPY_END rc=0$' "$log"
}

case "${1:-all}" in
  generate) do_generate ;;
  lib) do_lib ;;
  elf) do_elf ;;
  runtime) do_runtime ;;
  testgame) do_testgame ;;
  host) do_host ;;
  selftest-host) do_selftest_host ;;
  fetch-sdk) do_fetch_sdk ;;
  all) do_lib; do_elf; do_runtime; do_testgame ;;
  clean) rm -rf "$out" ;;
  *) echo "usage: $0 [all|generate|lib|elf|runtime|testgame|host|selftest-host|fetch-sdk|clean]" >&2; exit 2 ;;
esac
