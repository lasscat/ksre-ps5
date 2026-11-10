# Experiment 03 — a payload that embeds CPython 3.12.8

**Question:** does the static CPython build (`ports/cpython`) link into a PS5 payload, and
(03b) does it start on the console and import the stdlib parts Ren'Py and KSRE need?

`main.c` clears MXCSR FTZ/DAZ, starts the interpreter on a thread with a 16 MiB stack
(payload threads default to 64 KiB, the main thread has 2 MiB), and initialises Python with
an isolated `PyConfig`: UTF-8 mode, `home` = `/data/ksre-ps5/python`, module search path
`lib/python312.zip`, `lib/python3.12`, `lib/python3.12/lib-dynload` under the home,
unbuffered stdio, faulthandler on. It runs the embedded `hello.py` (or `--script FILE`),
handles `SystemExit` itself, finalizes, prints `KSRE_PS5_03_END rc=N` and calls `_exit()`.
Options: `--home DIR`, `--script FILE`; other arguments go to `sys.argv`.

`hello.py` prints `sys.version`/platform/path, then one `03 PASS|FAIL` line each for:
`encodings` (UTF-8 incl. non-BMP, CJK codec), `zlib`, `json` (+ `_json`), `_struct`,
`math` (+ `cmath`, subnormals survive = FTZ/DAZ cleared), `select` (kqueue on the PS5),
`_socket` (socketpair), `threading` (4 concurrent threads; reports whether they see
subnormals), `asyncio` (`asyncio.run` + `gather`, selector name), `os.urandom`; then
`03 SUMMARY pass=N fail=M`. On the console it also writes `/data/ksre-ps5/03/last-run.log`.

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

## Results

"—" = not attempted. Console results need a captured log in `.local-work/logs/`.

| Date | Variant | host | compiles | links | loader | console | notes |
|---|---|---|---|---|---|---|---|
| 2026-11-10 | macOS arm64 against build/host-python 3.12.8 | yes: `03 SUMMARY pass=10 fail=0`, `END rc=0`; `SystemExit(3)` → rc 3; uncaught error → traceback, rc 1 | — | — | — | — | logic check only |
| 2026-11-10 | macOS arm64, `--home` with only the PS5 `python312.zip` (+ host `lib-dynload`) | yes: 10/10; stdlib modules load from the zip's `.pyc` through `zipimporter` | — | — | — | — | checks the bundle format, not the PS5 |
| 2026-11-10 | CI Ubuntu 24.04 (run 42782874345): PS5 link + host twin (GCC 13, `-rdynamic`) | yes: 10/10, `EpollSelector` | yes | yes | — | — | first CI run failed only because the host twin lacked `-rdynamic` (fixed in dc7a80b) |
| 2026-11-10 | PS5 cross-build, SDK v0.45, LLVM 18.1.8, pacbrew v0.40.3, `-rdynamic` | — | yes | yes: ELF64 x86-64 PIE, 30 MB with debug info, NEEDED `libkernel_web`, `libSceLibcInternal`, `libSceNet`; 347 undefined dynamic symbols, all resolved against the SDK stubs | — | — | |
