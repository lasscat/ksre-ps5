# Ren'Py 8.5.3 for the PS5 (static modules + launcher)

Builds Ren'Py's native modules into a static library, adds this project's launcher, and links
one payload, `build/ports/renpy/ksre-ps5-renpy.elf`. The method is renpy-build's (generate with
`RENPY_STATIC=1`, compile, archive, register the modules in an inittab), reimplemented here;
no renpy-build file is used.

```sh
source .local-config/env.sh
tools/fetch-upstreams.sh renpy pacbrew   # pinned Ren'Py source (+ pacbrew recipes for VERSIONS.txt)
ports/cpython/build.sh host && ports/cpython/build.sh ps5
ports/build-all-deps.sh                   # build/sysroot (03c)
ports/renpy/build.sh                      # generate, lib, elf, runtime, testgame
ports/renpy/build.sh host                 # optional: the same build for this machine (twin)
ports/renpy/build.sh selftest-host        # run the test game's self-test on the twin
```

## Steps

| Step | What it does |
|---|---|
| `generate` | venv with Cython 3.1.4 (`upstreams.lock`) + setuptools 80.9.0; a clean `git archive` of the pinned Ren'Py commit in `build/ports/renpy/tree`; `RENPY_STATIC=1 setup.py generate` there (62 modules, ~3 s). `external/renpy` is never written |
| `lib` | compiles the generated C and Ren'Py's `src/*.c` listed in `src/Setup` with `prospero-clang` (72 sources, no source changes), writes `inittab.c` (`init_librenpy()`), archives `librenpy.a` (34 MB with line tables) |
| `elf` | compiles `launcher.c`, links with `prospero-clang++ -rdynamic` against `librenpy.a`, the static CPython (03a) and `build/sysroot` (03c). Linked twice: the second pass embeds the list of the ELF's 450 imported symbols for the launcher's start-up check |
| `runtime` | console tree `build/ports/renpy/runtime/renpy/` (14 MB): `renpy.py`, Ren'Py's Python package and `common/`, `renpy/vc_version.py`, the assimp stub, `lib/python/` with six 1.16.0 and ecdsa 0.19.0 (sha256-pinned wheels), checked-hash `.pyc` files from the host build Python 3.12.8 |
| `testgame` | `fixtures/testgame` + generated images/sounds into `build/ports/renpy/testgame` |

Modules: 60 of the 62 in `src/Setup`. Not built: `renpy.gl2.live2dmodel` (proprietary Cubism SDK)
and `renpy.gl2.assimp` (needs assimp, C++). Ren'Py imports `renpy.gl2.assimp` unconditionally, so
`stubs/assimp.py` provides its interface (hooks do nothing; `GLTFModel` raises). Desktop-only
`renpy.tfd` (file dialogs; import is optional) and renpy-build's brotli module are not built.
`_ctypes`, `_ssl`, `_hashlib` stay out of CPython: Ren'Py imports ctypes only on Windows/macOS/
Android paths or inside `try`, and `requests`/TLS only for the updater, Steam and sync.

Facts found while building (source / host):

* A git checkout has no `renpy/vc_version.py`; Ren'Py then reports the `main` branch version
  (8.6.0). `runtime` writes it from the pinned tag: `8.5.3.26051504`, branch `fix`,
  `official = False` (shown as `8.5.3.26051504+unofficial`).
* Apart from `vc_version.py`, generated `.pxi` files and the Windows `angle/` directory, the
  pure-Python tree is identical to the official 8.5.3 SDK's (`diff -r`).
* The link must search `build/sysroot/lib` before the pacbrew directory: pacbrew has its own
  FFmpeg (GPL configuration, with TLS) that would otherwise be picked.
* No `-lm`: math resolves from the console's `libSceLibcInternal`, like CPython's.

## Launcher (`launcher.c`)

Our code (GPL-3.0-or-later), from the 03 launcher. Registers the inittab, starts an isolated
interpreter (UTF-8 mode, `write_bytecode` off) and runs `renpy.py` as `__main__` with the game
directory as its argument. Console layout under `--root` (default `/data/ksre-ps5`):
`python/lib/python312.zip`, `renpy/` (runtime tree), `testgame/`, and `saves/`, `logs/`, `tmp/`
(created). Defaults it sets unless already set (`--env KEY=VALUE` overrides):
`RENPY_PATH_TO_SAVES=<root>/saves`, `RENPY_LOG_BASE=<root>/logs` (Ren'Py's `log.txt`,
`traceback.txt`), `RENPY_RENDERER=gl2`, `RENPY_PLATFORM=ps5-x86_64`, `HOME`, `TMPDIR`,
`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`, `RENPY_GL_VSYNC=0` (06b: the PS5 SDL/OSMesa flip
already waits for the 60 Hz vblank; Ren'Py's swap interval 1 on top of it dropped VP9 1080p30
videos to 19–25 fps, 0 keeps them at 29–30 fps).

* `RENPY cpus` line (06b): `sysconf`, `sysctl hw.ncpu` and FFmpeg's `av_cpu_count()` (what
  `ffmedia.c`'s `threads=auto` uses); 16 / 16 / 16 on the console.

* stdout/stderr also go to `<root>/logs/launcher-last.log` (previous run: `launcher-prev.log`).
* Start-up check: every imported symbol is looked up with `dlsym`; missing ones are printed as
  `RENPY import-missing NAME` (03b showed the SDK stubs list functions the console lacks).
  29 imports are new compared with payloads that already ran on the console (single-precision
  math, `setjmp`/`longjmp`, `posix_memalign`, `strerror_r`, ...).
* Python runs on a 16 MiB thread (PS5 main thread: 2 MiB); `--main-thread` runs it on the main
  thread (default on macOS, where Cocoa creates windows only there). Whether the PS5 SDL video
  driver accepts a non-main thread is unknown until 04b.
* `--max-seconds N` stops the payload after N s; `Py_FinalizeEx` gets 10 s. The last line is
  always `KSRE_PS5_RENPY_END rc=N`; exit through `_exit()`.
* No arguments (loaders pass none): arguments are read from `<root>/renpy-args.txt`.
* No child processes: before `renpy.py` runs, `subprocess` and every `os` spawn/exec function
  raise `OSError(ENOSYS)` (see 04b below). Also on the host twin.
* `LD_LIBRARY_PATH` defaults to the start directory (where the homebrew launcher put
  `eboot.elf` and `libOSMesa.so.8`), because the launcher `chdir()`s to the Ren'Py base.
* Memory (05): CPython's object arenas come from `malloc` (`PyObject_SetArenaAllocator`), not
  `mmap`. The app gets only 448 MiB of flexible memory (mmap, thread stacks, JIT code); malloc
  uses direct memory. With mmap()ed arenas KSRE's scripts used up the flexible pool and no
  thread could start. `KSRE_PS5_MEMLOG=N` prints `RENPY mem` (maxrss, flexible available /
  configured, direct size) every N s; it never allocates.
* Exit: the PS5 refuses the `exit` system call after a Ren'Py run (SIGSYS). `finish()` ignores
  SIGSYS, prints the end marker, tries `_exit` and then ends with `SIGKILL`
  (`RENPY WARN _exit refused; ending with SIGKILL`).

`probe/renpy.py` is a start-up tracer for crashes before Ren'Py's first log line: with
`--renpy-base <dir of probe>` the launcher runs it instead of `renpy.py`; it runs the real
`renpy.py` and prints every first import, Ren'Py call and C call as a `PROBE` line, pausing
4 ms after each so the line leaves the console before the next step.

## Console run (04b)

```sh
B=build/ports/renpy
tools/deploy-hb.sh ksre-renpy $B/ksre-ps5-renpy.elf --upload-only \
  --extra external/toolchains/pacbrew-v0.40.3/user/homebrew/lib/libOSMesa.so.8 \
  --tree build/cpython/ps5/python-home/lib:/data/ksre-ps5/python/lib \
  --tree $B/runtime/renpy:/data/ksre-ps5/renpy --tree $B/testgame:/data/ksre-ps5/testgame
# wait ~35 s (see ftpsrv below), then:
tools/deploy-hb.sh ksre-renpy $B/ksre-ps5-renpy.elf --no-upload \
  --env "KSRE_TESTGAME_SELFTEST=1 RENPY_LOG_TO_STDOUT=1" --args="--max-seconds 150" \
  --timeout 170 --idle-timeout 45
```

`RENPY_LOG_TO_STDOUT=1` streams Ren'Py's `log.txt` lines through the loader socket. Leave out
`KSRE_TESTGAME_SELFTEST=1` for the interactive game (`--max-seconds 600`, `--idle-timeout 620`).

Findings (console unless marked):

* **A `fork`/`exec` from the homebrew app takes the whole PS5 down** (black screen, console
  powered off; twice). Ren'Py 8.5.3 calls `platform.platform()` while importing
  `renpy.exports` (and in `log.py`); on FreeBSD CPython 3.12 runs `file -b <executable>` and
  `uname -p` through `subprocess` there. The probe's last events were the `os.pipe`/`os.open`
  that start `subprocess.Popen`; with the launcher's no-spawn guard the same start-up runs
  through (cause: console + inference; the `fork_exec` call itself was never seen).
* **ftpsrv does not fsync**: after those power-offs `renpy.py` and `script.rpyc` were 0 bytes
  and `python312.zip` had lost its last 4 bytes, so size-based skipping cannot be trusted after
  a crash. Re-upload with `--reupload` and give the console ~35 s (FreeBSD syncer delay,
  inference) before a run that may crash it; `--upload-only` / `--no-upload` split the steps.
* The SDK run-time linker searches the current directory last and splits `LD_LIBRARY_PATH`
  wrongly (it tests `path[off]` for `:`), so only a single directory works (source:
  `crt/rtld.c`). Without it: `Could not get pygame screen: error('libOSMesa.so.8:
  error_code=0xffffffff')` and Ren'Py falls back to `sw`, which also fails.
* All 450 imports resolve (`missing=0`, R17 closed); Python on the 16 MiB thread opens the SDL
  window and GL context (R16 closed: `--main-thread` not needed).
* Context: llvmpipe (LLVM 14.0.6, 256 bits), GL 4.5 compatibility, Mesa 22.1.7; Ren'Py reports
  max texture 4096. One `GL_INVALID_OPERATION in glUnmapBuffer(buffer is not mapped)` per start
  (harmless so far). Steam init fails without `_ctypes` (expected, ignored by Ren'Py).
* The window opens at 1739×978 inside the 1920×1080 display ("Windowed mode") unless the
  game or the player chooses full screen.

## Results

| Date | What | host | compiles | links | loader | console |
|---|---|---|---|---|---|---|
| 2026-11-10 | `librenpy.a` + payload, macOS arm64, LLVM 18.1.8, SDK v0.45 | — | yes: 72 sources, 9 warnings (all in upstream code) | yes: ELF64 x86-64 PIE, 74 MB unstripped, NEEDED = 03 payload's + SDL's Sce* modules | — | — |
| 2026-11-10 | host twin (same generated C, inittab, launcher, runtime tree; brew FFmpeg 9.0/SDL2-compat, FriBidi 1.0.15 from source) running `fixtures/testgame` self-test | yes: 10/10 (gl2, pixel read-back, ATL, audio, persistent 1→2 across runs, save, load, rollback, choice); interpreter ready 0.007 s, test 2.8 s | n/a | n/a | n/a | n/a |
| 2026-11-11 | console fw 7.61, websrv homebrew launcher, runtime tree + stdlib zip under `/data/ksre-ps5`, `libOSMesa.so.8` next to `eboot.elf` (before the fixes) | — | — | — | yes | **FAIL**: twice the whole console went black and powered off right after `renpy.py` started (fork/exec, see findings) |
| 2026-11-11 | console, launcher with no-spawn guard + `LD_LIBRARY_PATH` + exit fallback; test game self-test | yes: 11/11 (with `gamemenu`) | n/a | n/a | yes | **yes: 10/10** (gl2 on llvmpipe, savedir, persistent runs 1→2→3 across runs, ATL, pixel read-back, WAV position, choices, save, load, rollback), test 6.1 s, Python ready 0.08 s; ends `rc=0` then `_exit refused; SIGKILL` |
| 2026-11-11 | console, KSRE v2.0.5 with arenas via malloc (see ports/ksre) | yes: test game self-test passes | n/a | n/a | yes | **yes**: title screen, new game, first scenes, opening video (ports/ksre/README.md) |
| 2026-11-11 | console, interactive test game with the controller (maintainer) | n/a | n/a | n/a | yes | **yes**: maintainer confirmed the checklist ("everything works": dialogue, both choices (spin/slide), ATL, chime, save, load, rollback); persistence across restarts from the console's persistent file (`ksre_starts` = 7 after 7 starts over several launches and power cycles); game menu needed `layout.defaults()` and a focus style (test-game fixes); Quit ends with `rc=0` |
