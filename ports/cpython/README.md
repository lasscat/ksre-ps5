# CPython 3.12.8 for the PS5 (static)

Builds the interpreter Ren'Py 8.5.3 uses as a static library for `x86_64-sie-ps5`,
plus a pure-Python stdlib bundle. Nothing here has run on a console yet (milestone 03b).

```sh
source .local-config/env.sh            # PS5_PAYLOAD_SDK, LLVM_CONFIG
tools/fetch-pacbrew.sh swgl            # zlib, bzip2, xz come from this pinned bundle
ports/cpython/build.sh host            # build Python 3.12.8 -> build/host-python (~1 min)
ports/cpython/build.sh ps5             # static PS5 build -> build/cpython/ps5 (~1 min)
make -C experiments/03-python-hello ps5
```

Inputs: the python.org tarball pinned in `upstreams.lock` (`CPYTHON_URL`,
`CPYTHON_SHA256`), the SDK, pacbrew v0.40.3, and the host build Python of the same version
(`--with-build-python`). Host and target trees are separate: `build/host-python` is a
native build, everything under `build/cpython/ps5` is built only with `prospero-*`.

Outputs (`build/cpython/ps5/`): `lib/libpython3.12.a` (50 MB with debug info) and the
three bundled helper archives it needs (`libmpdec.a`, `libexpat.a`, `libHacl_Hash_SHA2.a`);
`install/` (headers, `make install` tree for prefix `/data/ksre-ps5/python`);
`python-home/lib/python312.zip` (6.5 MB); `embed.mk` (flags for payloads); `modules.txt`.
The build also links CPython's own `python` program as a PS5 ELF (`src/python.exe` on
macOS hosts), which is a second link check.

## How it is configured

| Choice | Why |
|---|---|
| `--host=x86_64-unknown-freebsd` | the target is FreeBSD-derived (clang defines `__FreeBSD__`, FreeBSD headers); configure's FreeBSD branches apply. `sys.platform` is `freebsd`, like SDL's `platform:FreeBSD` |
| `MODULE_BUILDTYPE=static` (config.site), `--disable-shared` | every extension module is linked into libpython and registered in the generated inittab (`Modules/config.c`); no `.so` files |
| `--without-libm` | the SDK has no libm; C99 math links from the console's `libSceLibcInternal` (01b probe: libm results correct). Keeps pacbrew's openlibm `libm.a` out |
| `--with-pkg-config=no`, explicit `ZLIB_*`/`BZIP2_*`/`LIBLZMA_*` | libraries come only from the pinned pacbrew bundle, never from host pkg-config (CI) |
| `--enable-ipv6`, `ac_cv_buggy_getaddrinfo=no` | getaddrinfo is the SDK's FreeBSD code; probe `net.getaddrinfo` passed |
| `--with-computed-gotos` | clang supports it; configure cannot test it when cross compiling |
| `--disable-test-modules`, `--without-ensurepip` | not needed on the console |
| `config.site` | answers configure cannot get while cross compiling, from the 01a link inventory and 01b probe: the 20 absent functions (`=no`), `uname` (header inline, `=yes`), no raw getrandom syscall, 16-bit unsigned `wchar_t`, pthreads without flags, x86-64 facts |
| `Setup.local` | `*disabled*` list (below) |

Patches (`patches/`, applied to the extracted tarball; written so they could go upstream):

| Patch | Change |
|---|---|
| `0001-configure-allow-freebsd-cross-host` | configure only allows cross builds for Linux/Cygwin/VxWorks/WASM hosts; adds `*-*-freebsd*` (`ac_sys_system=FreeBSD`, `_PYTHON_HOST_PLATFORM=freebsd-x86_64`) to `configure.ac` and `configure` (no autoreconf needed) |
| `0002-configure-respect-libm-for-bundled-expat-mpdec` | configure hard-codes `-lm` for the bundled expat and libmpdec, ignoring `--with-libm`; the stray `-lm` silently linked pacbrew's openlibm. Uses `$(LIBM)` instead |
| `0003-timemodule-test-value-of-have-decl-tzname` | timemodule tests `HAVE_DECL_TZNAME` with `#ifdef`, but configure defines it to 0; tests the value (03b) |
| `0004-thread-pthread-clear-ftz-daz-on-ps5` | PS5 threads start with MXCSR FTZ/DAZ set (console: 0x9fc0); clears them in `pythread_wrapper` so Python threads keep IEEE subnormals (03b) |
| `0005-timemodule-reject-trailing-e-o-modifier-on-ps5` | the console's `wcsftime()` never returns for a format ending in a bare `%E`/`%O`; `time.strftime` raises `ValueError` instead (03b) |

Run-time corrections in `config.site` (03b, console evidence): `struct tm` `tm_zone`/`tm_gmtoff`
off (the console's `localtime_r` leaves them unset, so `import time` crashed); `tzname` off;
`clock_nanosleep` off (links against an SDK stub, absent on the console: `time.sleep` raised
ENOSYS; `nanosleep` works); `fdopendir` off (fails with EOPNOTSUPP, which broke `os.scandir(fd)`,
`os.fwalk` and `shutil.rmtree`).

No C source of CPython needed a change for compiling and linking (03a); patches 0003–0005 are
run-time fixes from 03b. The build compiles with
`-Werror=implicit-function-declaration` (CPython's default) and links with `-rdynamic`
(R13); the only warnings are the SDK `math.h` `_Generic` notes also seen in 01a and one
always-false `wchar_t` range comparison (16-bit `wchar_t`).

## Extension modules

76 built in (`build/cpython/ps5/modules.txt`): `_abc _ast _asyncio _bisect _blake2 _bz2
_codecs _codecs_cn _codecs_hk _codecs_iso2022 _codecs_jp _codecs_kr _codecs_tw _collections
_contextvars _csv _datetime _decimal _elementtree _functools _heapq _imp _io _json _locale
_lsprof _lzma _md5 _multibytecodec _opcode _operator _pickle _posixsubprocess _queue _random
_sha1 _sha2 _sha3 _signal _socket _sre _stat _statistics _string _struct _symtable _thread
_tokenize _tracemalloc _typing _weakref _xxinterpchannels _xxsubinterpreters _zoneinfo array
atexit audioop binascii cmath errno faulthandler fcntl gc itertools math mmap posix pwd
pyexpat resource select syslog termios time unicodedata zlib`. expat and libmpdec are
CPython's bundled copies; zlib 1.3.2, bzip2 1.0.8 and xz 5.4.6 come from pacbrew.

Not built:

| Module | Reason |
|---|---|
| `_ctypes` | needs libffi; not in the SDK or the pacbrew bundle. Ren'Py uses ctypes only on Windows/macOS/Android code paths (to confirm in 04a) |
| `_ssl`, `_hashlib` | OpenSSL 3.5.2 exists in pacbrew, but nothing up to 03b needs TLS (asyncio imports `ssl` optionally) and `hashlib` uses the built-in `_md5/_sha1/_sha2/_sha3/_blake2`. Decide in 04a |
| `grp` | the SDK libc has no `setgrent/getgrent/endgrent` (link error); no group database on the console. `shutil`, `tarfile`, `pathlib` treat `grp` as optional |
| `_multiprocessing`, `_posixshmem` | the payload is one process; no fork/exec of Python children |
| `_sqlite3`, `_dbm`, `_gdbm`, `_curses`, `_curses_panel`, `readline`, `_tkinter`, `_uuid`, `nis`, `_crypt`, `ossaudiodev` | no library in the SDK/bundle, no use on the console (configure reports them missing; also listed in `Setup.local`) |
| `spwd`, `_scproxy` | not applicable on FreeBSD (configure) |
| test modules (`_testcapi`, `xxsubtype`, ...) | `--disable-test-modules` |

Compared with renpy-build's 3.12.8 module list (read, not copied): same core set; this
build adds `_decimal` (bundled libmpdec), `audioop`, `syslog` and the subinterpreter
modules, and leaves out `_ctypes`, `_ssl`, `_hashlib` and `grp` for the reasons above.

## Stdlib bundle

`make_stdlib_zip.py` packs the installed `lib/python3.12` into `python312.zip`: every `.py`
plus an unchecked-hash `.pyc` next to it (zipimport has no `__pycache__` support and the
console cannot write into the zip), entries sorted with fixed timestamps (same input, same
zip). Left out: `test`, `idlelib`, `tkinter`, `turtle*`, `ensurepip`, `lib2to3`, `venv`,
`pydoc_data`, `site-packages`, `config-3.12-*`. It includes the target's
`_sysconfigdata__freebsd_.py`. On the console it goes to
`/data/ksre-ps5/python/lib/python312.zip` (PyConfig `home` = `/data/ksre-ps5/python`).

## Results

| Date | What | host | compiles | links | loader | console |
|---|---|---|---|---|---|---|
| 2026-11-10 | host build Python 3.12.8 (macOS arm64; CI Ubuntu) | yes | n/a | n/a | n/a | n/a |
| 2026-11-10 | static CPython, macOS arm64 host, LLVM 18.1.8, SDK v0.45, pacbrew v0.40.3 | — | yes (all objects, no errors) | yes: `libpython3.12.a`; CPython's `python` program links as ELF64 x86-64 PIE, NEEDED `libkernel_web`, `libSceLibcInternal`, `libSceNet` only | — | — |
| 2026-11-10 | same in CI (`ps5-cross-build`, Ubuntu 24.04, LLVM 18, run 42782874345) | yes (host build Python, GCC 13) | yes | yes: same 76 modules, payload NEEDED identical; host Python + PS5 build ~3.5 min | — | — |

## Open points for 03b

* MXCSR FTZ/DAZ is per thread. The launcher clears it on the threads it creates; whether
  threads started by Python inherit a clean MXCSR is measured by the hello script (R2b).
* `fork`/`vfork` link statically from the SDK libc; their run-time behaviour is unknown and
  nothing should call them (`subprocess` is importable, not usable).
* `os.urandom` and hash randomisation read `/dev/urandom` (probe: works).
* `faulthandler` uses `sigaltstack` (links; untested on the console).
