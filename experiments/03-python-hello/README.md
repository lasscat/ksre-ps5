# Experiment 03 — a payload that embeds CPython 3.12.8

**Question:** does the static CPython build (`ports/cpython`) link into a PS5 payload, and
(03b) does it start on the console and import the stdlib parts Ren'Py and KSRE need?

`main.c` clears MXCSR FTZ/DAZ, starts the interpreter on a thread with a 16 MiB stack
(payload threads default to 64 KiB, the main thread has 2 MiB), and initialises Python with
an isolated `PyConfig`: UTF-8 mode, `home` = `/data/ksre-ps5/python`, module search path
`lib/python312.zip`, `lib/python3.12`, `lib/python3.12/lib-dynload` under the home,
unbuffered stdio, faulthandler on. It runs the embedded `hello.py` (or `--script FILE`),
handles `SystemExit` itself, finalizes, prints `KSRE_PS5_03_END rc=N` and calls `_exit()`.
Options: `--home DIR`, `--script FILE`, `--verbose`; other arguments go to `sys.argv`. elfldr
passes no arguments, so without any the payload reads them from `/data/ksre-ps5/03/args.txt`.
A fault prints signal, `rip` and the load address, then `KSRE_PS5_03_END rc=134`.

`hello.py` prints `sys.version`/platform/path, then one `03 PASS|FAIL` line each for:
`encodings` (UTF-8 incl. non-BMP, CJK codec), `zlib`, `json` (+ `_json`), `_struct`,
`math` (+ `cmath`, subnormals survive = FTZ/DAZ cleared), `select` (kqueue on the PS5),
`_socket` (socketpair), `threading` (4 concurrent threads; reports whether they see
subnormals), `asyncio` (`asyncio.run` + `gather`, selector name), `os.urandom`; then
`03 SUMMARY pass=N fail=M`. On the console it also writes `/data/ksre-ps5/03/last-run.log`.
It also prints the start time (payload `main()` → first Python line) and `ru_maxrss`.

`tests.py` (03b) imports every stdlib module (272 on the console) and runs a curated part of
CPython's test suite (struct, json, zlib, time, float, math, threading, os, asyncio futures/
tasks/locks/queues/runners/timeouts/taskgroups/base_events) from `lib/python312-tests.zip`
(built by `ports/cpython/build.sh ps5`, unpacked at run time). Process spawning is disabled
(`fork`/`exec` must not be called). Output: `03T MODULE …` lines, tracebacks of failures,
`03T SUMMARY …`.

## Build and run

```sh
source .local-config/env.sh
ports/cpython/build.sh host && ports/cpython/build.sh ps5
make -C experiments/03-python-hello ps5        # build/experiments/03-python-hello/ps5/ksre-ps5-03-python-hello.elf
make -C experiments/03-python-hello run-host   # same launcher + script against build/host-python
```

Console (03b): upload `build/cpython/ps5/python-home/lib/python312.zip` to
`/data/ksre-ps5/python/lib/python312.zip` (FTP 2121), then send the ELF to elfldr (9021;
it draws nothing, so the homebrew launcher is not needed). The script needs a few seconds.
For the tests also upload `python312-tests.zip` next to it and `tests.py` to `/data/ksre-ps5/03/`,
write `--script /data/ksre-ps5/03/tests.py [MODULE ...]` to `/data/ksre-ps5/03/args.txt`, and run
`tools/deploy.sh ELF --timeout 900 --idle-timeout 150` (stops at `KSRE_PS5_03_END`; ~3.5 min).

## Results

"—" = not attempted. Console results need a captured log in `.local-work/logs/`.

| Date | Variant | host | compiles | links | loader | console | notes |
|---|---|---|---|---|---|---|---|
| 2026-11-10 | macOS arm64 against build/host-python 3.12.8 | yes: `03 SUMMARY pass=10 fail=0`, `END rc=0`; `SystemExit(3)` → rc 3; uncaught error → traceback, rc 1 | — | — | — | — | logic check only |
| 2026-11-10 | macOS arm64, `--home` with only the PS5 `python312.zip` (+ host `lib-dynload`) | yes: 10/10; stdlib modules load from the zip's `.pyc` through `zipimporter` | — | — | — | — | checks the bundle format, not the PS5 |
| 2026-11-10 | CI Ubuntu 24.04 (run 42782874345): PS5 link + host twin (GCC 13, `-rdynamic`) | yes: 10/10, `EpollSelector` | yes | yes | — | — | first CI run failed only because the host twin lacked `-rdynamic` (fixed in dc7a80b) |
| 2026-11-10 | PS5 cross-build, SDK v0.45, LLVM 18.1.8, pacbrew v0.40.3, `-rdynamic` | — | yes | yes: ELF64 x86-64 PIE, 30 MB with debug info, NEEDED `libkernel_web`, `libSceLibcInternal`, `libSceNet`; 347 undefined dynamic symbols, all resolved against the SDK stubs | — | — | |
| 2026-11-10 | **PS5 console** (fw 7.61, elfldr 9021), `hello.py` | — | yes | yes | yes | **yes: 10/10, `END rc=0`**; `Py_InitializeFromConfig` 0.019 s, first Python line 0.022 s after `main()`; `ru_maxrss` 42.7 MiB; threads see subnormals (patch 0004) | log `ksre-ps5-03-python-hello-20261110T174013Z.log` (local) |
| 2026-11-10 | **PS5 console**, `tests.py` (curated CPython tests) | macOS: 2444 run, only `_testcapi` errors | yes | yes | yes | **yes: 272/272 stdlib imports (missing as expected: crypt ctypes curses sqlite3 ssl); 16 modules, 2494 run, 20 fail, 21 err, 245 skip, 137 s, `ru_maxrss` 111 MiB.** Clean: struct, json, zlib, float, math, asyncio futures/tasks/locks/queues/runners/timeouts/taskgroups. Remaining (no Ren'Py impact): test_threading 3 (`_testcapi` not built); test_time 3 (libc `%Y` for years >9999/<0, `pthread_getcpuclockid` = `CLOCK_THREAD_CPUTIME_ID`); test_base_events 7 (`getaddrinfo` service names/IPv6 scope); test_os 28 (`os.utime` ignored on `/data`, no hard links/FIFOs/tty, uid/gid setters succeed, `readlink`/`pathconf`/`fchdir`/`closerange` error codes differ) | log `…T174014Z.log` (local) |

Console fixes found in 03b (`ports/cpython`): `import time` crashed (SDK `struct tm` has
`tm_zone`, console `localtime_r` leaves it unset → `config.site` + patch 0003); Python threads
flushed subnormals (patch 0004); `time.sleep` raised ENOSYS (`clock_nanosleep` is an SDK stub
→ `config.site`); `wcsftime` hangs on a trailing `%E`/`%O` (patch 0005); `fdopendir` fails with
EOPNOTSUPP, breaking `shutil.rmtree` (→ `config.site`). Importing the builtin `_asyncio` before
`asyncio` breaks `asyncio.current_task()` (upstream 3.12 behaviour, also on the host build);
`tests.py` imports `asyncio` first.
