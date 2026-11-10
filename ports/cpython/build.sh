#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CPython 3.12.8 for ksre-ps5: a host "build Python" and a static PS5 build.
#
#   ports/cpython/build.sh host    build Python for this machine -> build/host-python
#   ports/cpython/build.sh ps5     static CPython for x86_64-sie-ps5 -> build/cpython/ps5/
#   ports/cpython/build.sh all     both
#   ports/cpython/build.sh clean   remove build/cpython and build/host-python
#
# ps5 needs PS5_PAYLOAD_SDK (tools/fetch-sdk.sh), a host LLVM for the SDK wrappers and
# the pacbrew bundle the project uses (tools/fetch-pacbrew.sh swgl) for zlib, bzip2, xz.
#
# Outputs of `ps5` (all under build/cpython/ps5/):
#   install/                 `make install DESTDIR=...` tree (prefix /data/ksre-ps5/python)
#   lib/libpython3.12.a      static interpreter + every enabled extension module, plus the
#                            bundled helper archives it needs (libmpdec, expat, HACL SHA-2)
#   python-home/lib/python312.zip   pure-Python stdlib (+ .pyc), home = /data/ksre-ps5/python
#   embed.mk                 include/link flags for payloads that embed Python
#   modules.txt              built-in extension modules (from the generated config.c)
set -euo pipefail
# shellcheck source=../../tools/lib/common.sh
. "$(dirname "$0")/../../tools/lib/common.sh"

here="${KSRE_PS5_ROOT}/ports/cpython"
ver=$(lock_get CPYTHON_VERSION)
pyver=${ver%.*}
tarball="${KSRE_PS5_ROOT}/external/toolchains/downloads/Python-${ver}.tar.xz"
host_prefix="${KSRE_PS5_ROOT}/build/host-python"
host_python="${host_prefix}/bin/python${pyver}"
out="${KSRE_PS5_ROOT}/build/cpython"
ps5_out="${out}/ps5"
target_prefix=/data/ksre-ps5/python
pacbrew_rel=$(lock_get PACBREW_SWGL_RELEASE)
pacbrew="${PS5_PACBREW_PREFIX:-${KSRE_PS5_ROOT}/external/toolchains/pacbrew-${pacbrew_rel}/user/homebrew}"

if command -v getconf >/dev/null 2>&1; then
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
else
  jobs=4
fi

fetch() {
  download_verified "$(lock_get CPYTHON_URL)" "$tarball" "$(lock_get CPYTHON_SHA256)"
}

unpack() { # dest
  rm -rf "$1"
  mkdir -p "$1"
  tar -xJf "$tarball" -C "$1" --strip-components=1
}

host_is_built() {
  [ -x "$host_python" ] &&
    "$host_python" -c "import sys; sys.exit(sys.version.split()[0] != '${ver}')" 2>/dev/null
}

# ---------------------------------------------------------------------------
build_host() {
  if host_is_built; then
    echo "host Python ${ver} already built: ${host_python}"
    return 0
  fi
  fetch
  local src="${out}/host-src"
  unpack "$src"
  echo "configuring host Python ${ver} (prefix ${host_prefix})"
  # A plain native build; nothing from the PS5 environment may leak in.
  (
    cd "$src"
    env -u CC -u CXX -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS -u CONFIG_SITE \
      -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
      ./configure --prefix="$host_prefix" --without-ensurepip --disable-test-modules \
      > "${out}/host-configure.log" 2>&1 || { tail -40 "${out}/host-configure.log"; exit 1; }
    echo "building host Python (log: build/cpython/host-make.log)"
    { make -j"$jobs" && make install; } > "${out}/host-make.log" 2>&1 ||
      { tail -60 "${out}/host-make.log"; exit 1; }
  )
  host_is_built || { echo "host Python did not report ${ver}" >&2; exit 1; }
  rm -rf "$src"
  echo "host Python: $("$host_python" -c 'import sys; print(sys.version)')"
}

# ---------------------------------------------------------------------------
check_ps5_inputs() {
  if [ -z "${PS5_PAYLOAD_SDK:-}" ] || [ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-clang" ]; then
    echo "PS5_PAYLOAD_SDK is not set or not an SDK; run tools/fetch-sdk.sh and export it" >&2
    exit 2
  fi
  local lib
  for lib in libz.a libbz2.a liblzma.a; do
    if [ ! -f "${pacbrew}/lib/${lib}" ]; then
      echo "missing ${pacbrew}/lib/${lib}; run tools/fetch-pacbrew.sh swgl" >&2
      exit 2
    fi
  done
  if ! host_is_built; then
    echo "host Python ${ver} missing; run: ports/cpython/build.sh host" >&2
    exit 2
  fi
}

build_ps5() {
  check_ps5_inputs
  fetch
  mkdir -p "$ps5_out"
  local src="${ps5_out}/src" sdk="${PS5_PAYLOAD_SDK}" p
  unpack "$src"
  for p in "${here}"/patches/*.patch; do
    echo "patch: $(basename "$p")"
    patch -d "$src" -p1 --quiet --forward < "$p"
  done
  cp "${here}/Setup.local" "${src}/Modules/Setup.local"

  echo "configuring CPython ${ver} for x86_64-sie-ps5 (log: build/cpython/ps5/configure.log)"
  (
    cd "$src"
    # Third-party libraries come from the pinned pacbrew bundle only, never from
    # pkg-config (which would find host libraries in CI).
    env -u CPPFLAGS -u LIBS -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
      CONFIG_SITE="${here}/config.site" \
      CC="${sdk}/bin/prospero-clang" CXX="${sdk}/bin/prospero-clang++" \
      AR="${sdk}/bin/prospero-ar" RANLIB="${sdk}/bin/prospero-ranlib" \
      STRIP="${sdk}/bin/prospero-strip" \
      CFLAGS="-Wno-unknown-warning-option" LDFLAGS="-rdynamic" \
      ZLIB_CFLAGS="-I${pacbrew}/include" ZLIB_LIBS="-L${pacbrew}/lib -lz" \
      BZIP2_CFLAGS="-I${pacbrew}/include" BZIP2_LIBS="-L${pacbrew}/lib -lbz2" \
      LIBLZMA_CFLAGS="-I${pacbrew}/include" LIBLZMA_LIBS="-L${pacbrew}/lib -llzma" \
      ./configure \
        --host=x86_64-unknown-freebsd \
        --build="$(./config.guess)" \
        --prefix="$target_prefix" \
        --disable-shared \
        --enable-ipv6 \
        --without-libm \
        --with-computed-gotos \
        --without-ensurepip \
        --disable-test-modules \
        --with-pkg-config=no \
        --with-build-python="$host_python" \
      > "${ps5_out}/configure.log" 2>&1 || { tail -40 "${ps5_out}/configure.log"; exit 1; }
    echo "building (log: build/cpython/ps5/make.log)"
    make -j"$jobs" > "${ps5_out}/make.log" 2>&1 || { tail -60 "${ps5_out}/make.log"; exit 1; }
    rm -rf "${ps5_out}/install"
    make install DESTDIR="${ps5_out}/install" > "${ps5_out}/install.log" 2>&1 ||
      { tail -40 "${ps5_out}/install.log"; exit 1; }
  )
  rm -rf "${ps5_out:?}/lib"
  mkdir -p "${ps5_out}/lib"
  cp "${src}/libpython${pyver}.a" "${ps5_out}/lib/"
  write_module_list "$src"
  write_embed_mk "$src"
  "$host_python" -I "${here}/make_stdlib_zip.py" \
    "${ps5_out}/install${target_prefix}/lib/python${pyver}" \
    "${ps5_out}/python-home/lib/python${pyver//./}.zip"
  echo "done: ${ps5_out}/lib/libpython${pyver}.a ($(wc -c < "${ps5_out}/lib/libpython${pyver}.a" | tr -d ' ') bytes)"
}

# Built-in modules = the inittab in the generated Modules/config.c.
write_module_list() {
  local src=$1
  sed -n 's/^extern PyObject\* PyInit_\(.*\)(void);$/\1/p' "${src}/Modules/config.c" | sort \
    > "${ps5_out}/modules.txt"
  echo "built-in extension modules: $(wc -l < "${ps5_out}/modules.txt" | tr -d ' ') (build/cpython/ps5/modules.txt)"
}

# Flags for embedding: headers from the install tree; libraries = what the Makefile links
# the `python` program with (expanded by make). The helper archives it names relative to
# the source tree (bundled libmpdec, expat, HACL SHA-2) are copied next to libpython.
write_embed_mk() {
  local src=$1 tok libs="" seen=" "
  # shellcheck disable=SC2016 # $(...) here are make variables, not shell
  printf 'print-embed-libs:\n\t@echo $(LIBS) $(MODLIBS) $(SYSLIBS)\n' > "${ps5_out}/print-libs.mk"
  for tok in $(cd "$src" && make -s -f Makefile -f "${ps5_out}/print-libs.mk" print-embed-libs); do
    case "$tok" in
      /*) ;;
      *.a) cp "${src}/${tok}" "${ps5_out}/lib/"; tok="${ps5_out}/lib/$(basename "$tok")" ;;
    esac
    case "$seen" in *" ${tok} "*) continue ;; esac
    seen="${seen}${tok} "
    libs="${libs:+${libs} }${tok}"
  done
  {
    echo "# Generated by ports/cpython/build.sh; do not edit."
    echo "PY_PS5_PREFIX := ${ps5_out}"
    echo "PY_PS5_INCLUDES := -I${ps5_out}/install${target_prefix}/include/python${pyver}"
    echo "PY_PS5_LIBS := ${ps5_out}/lib/libpython${pyver}.a ${libs}"
    echo "PY_PS5_STDLIB_ZIP := ${ps5_out}/python-home/lib/python${pyver//./}.zip"
  } > "${ps5_out}/embed.mk"
}

case "${1:-}" in
  host) build_host ;;
  ps5) build_ps5 ;;
  all) build_host; build_ps5 ;;
  clean) rm -rf "$out" "$host_prefix" ;;
  *) sed -n '4,9p' "$0"; exit 2 ;;
esac
