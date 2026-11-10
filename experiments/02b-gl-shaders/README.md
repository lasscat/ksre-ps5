# Experiment 02b — OpenGL context, Ren'Py-style shaders, 1080p benchmark

**Question:** can a payload built against the pinned PS5 SDL2 port get an
OpenGL (or OpenGL ES) context that runs the shaders and draw calls Ren'Py's
`gl2` renderer needs, fast enough for 1080p visual-novel frames?

The payload (`main.c`) does, for each requested API:

* **`gl`** = Ren'Py's `gl2` renderer: desktop OpenGL 2.0 **compatibility**
  profile, GLSL 1.20 shaders (`#version 120`).
* **`gles`** = Ren'Py's `gles2` renderer: OpenGL ES **3.0** context, GLSL ES
  1.00 shaders (`#version 100`, with Ren'Py's precision header).

and runs these stages, logging every step (`02B PASS/INFO/FAIL ...`):

1. Requests the GL attributes of `renpy/gl2/gl2draw.pyx`
   `select_gl_attributes` (RGBA 8/8/8/8, 24-bit depth, version/profile above,
   `SDL_OPENGL_ES_DRIVER` hint) and creates a fullscreen-desktop window (falls
   back to windowed, as Ren'Py does). If Ren'Py's own request fails it tries
   three other requests per API and says so, to separate "this driver cannot
   do GL at all" from "this driver cannot do Ren'Py's version/profile".
2. Logs `GL_VENDOR/RENDERER/VERSION/SHADING_LANGUAGE_VERSION`, limits, and the
   extension list (read with `glGetString(GL_EXTENSIONS)` like Ren'Py), and
   resolves all **58 functions of Ren'Py's `required_functions`**
   (`renpy/gl2/gl2functions.py`, checked against the file by script) through
   `SDL_GL_GetProcAddress`.
3. Assembles Ren'Py's built-in shader parts (`renpy.geometry`, `renpy.texture`,
   `renpy.alpha`, `renpy.matrixcolor`, `renpy.blur`; texts copied from
   `renpy/common/_shaders.rpym` at the pinned tag) the way
   `gl2shadercache.py:source()` does (variable declarations sorted by name,
   parts sorted by priority, per-stage "used variable" filtering, the GLES
   precision header), then compiles/links four programs and logs the compile
   and link logs.
4. Draws the way Ren'Py draws: client-side vertex arrays, client-side
   `GL_UNSIGNED_INT` indices, premultiplied blending (`GL_ONE,
   GL_ONE_MINUS_SRC_ALPHA`), a renderbuffer FBO sized by Ren'Py's rule
   (4096×4096 RGBA8 + depth), render-to-texture by `glCopyTexImage2D`, full
   mip chain by `glGenerateMipmap`, and Ren'Py's mip-**bias** blur shader.
5. **Read-back self-test** (a frame that is black or wrong with "no GL errors"
   must not pass): flat colours composited and read back with `glReadPixels`
   and compared with values computed on the CPU, for the texture shader + blend,
   the FBO copy + `matrixcolor`, and the mipmapped blur; plus an orientation
   marker (green square must be at the top-left).
6. **Benchmark** (default 300 frames per test, with vsync on and, if the driver
   allows it, unpaced): scene straight to the window, same limited to a 1080p
   viewport, an 8-layer fill-rate stress, scene→FBO→copy→`matrixcolor`,
   blur on a cached mipmapped texture, and the worst case (re-render +
   re-allocate + mipmap + blur every frame) plus a per-stage breakdown. Times
   are wall-clock around `glFinish()` so GPU work is included, plus the
   swap time and the frame period.
7. Holds an animated scene on screen for a few seconds so the maintainer can
   check the picture (circle round, four coloured corner squares).

A watchdog thread (256 KiB stack) ends the process if the main thread makes no
progress for 45 s; a crash handler records the stage, the registers, the code
bytes and the top of the stack before `_exit`. The log is written to stdout and
to `/data/ksre-ps5/02b/last-run.log`. On the PS5 the whole test runs on a 64 MiB
thread, and the process ends with `_exit()` after the end marker.

Any pad button, Escape or window close ends the run early (by design; the log
says `stopping early`). **Do not touch the controller during a measured run.**

Options: `--api gl|gles|gl,gles` (default both, in one process), `--frames N`,
`--vsync 0|1|both`, `--windowed`, `--hold-ms N`, `--selftest-only`,
`--dump-shaders`, `--screenshot PATHPREFIX` (host aid), `--no-preflight`.
Diagnostics: `--probe-calls` (one call per entry-point class with stage
markers), `--dump-procs` (module/offset of every resolved GL function),
`--no-mipmap`, `--stack-mb N` (0 = stay on the main thread), `--normal-exit`
(return from `main` instead of `_exit()`), `--stall-ms N`.

## Build and run

```sh
source .local-config/env.sh
tools/fetch-pacbrew.sh hwgl            # Mesa 26.2 / zink bundle (v0.41.1)
tools/fetch-pacbrew.sh swgl            # OSMesa bundle (v0.40.3)
make -C experiments/02b-gl-shaders host && make -C experiments/02b-gl-shaders run-host   # desktop GL on this machine
make -C experiments/02b-gl-shaders ps5                  # HW bundle ELF  -> build/.../ps5/
make -C experiments/02b-gl-shaders ps5 PACBREW=swgl     # SW bundle ELF  -> build/.../ps5-swgl/
make -C experiments/02b-gl-shaders deploy-hw ENVV="EGL_LOG_LEVEL=debug MESA_DEBUG=1"
make -C experiments/02b-gl-shaders deploy-sw ARGS="--api gl --vsync 1"
```

The PS5 binaries are linked with **`-rdynamic`**; without it the Mesa libraries'
weak `kernel_mprotect` import resolves to NULL and the first executable-memory
allocation crashes (see [docs/GRAPHICS.md](../../docs/GRAPHICS.md)). The runner
(`tools/deploy-hb.sh` → `tools/ps5_hbrun.py`) stops by itself at the payload's
`KSRE_PS5_02B_END` line, when the connection closes, after `--idle-timeout`
seconds of silence, or at `--timeout`.

**Where Mesa's `.so` files go.** SDL `dlopen()`s the bare names `libEGL.so.1`
(then `libGL.so.1` or `libGLESv2.so.2`) or, in the software bundle,
`libOSMesa.so.8`. The SDK's run-time linker (`crt/rtld.c`, `__rtld_find_file`)
looks in the system library directories, then `LD_LIBRARY_PATH`, then
`/user/homebrew/lib` (absent on this console), and finally the **current
directory**, which the homebrew launcher sets to the directory the ELF was
uploaded to. `tools/deploy-hb.sh --extra` therefore uploads the libraries next
to `eboot.elf` under `/data/homebrew/ksre-02b-hw/` (or `-sw/`); nothing outside
`/data/homebrew/<name>/` and `/data/ksre-ps5/` is written. The launcher's
`env=` parameter (`--env "A=1 B=2"`) sets the payload's environment.

## Results

Statuses are kept separate ("—" = not attempted). Console rows cite logs in
`.local-work/logs/` (not committed).

| Date | Variant | host | compiles | links | loader | console | notes |
|---|---|---|---|---|---|---|---|
| 2026-11-10 | host: macOS arm64, SDL 2.32 (sdl2-compat), Apple GL 2.1 Metal (GLSL 1.20), `--api gl` | yes: all 7 read-back probes PASS, 62/62 GL functions resolve, 4/4 programs link, 0 GL errors, orientation correct, screenshot reviewed | — | — | — | — | benchmark numbers are from an Apple GPU and mean nothing for the PS5. With vsync on, the swap blocked forever on this (non-visible) window; the watchdog ended it, so the host run uses `--vsync 0`. The GLES dialect cannot run on this host. |
| 2026-11-10 | PS5 cross-build, bundle v0.41.1 (HW), SDK v0.45, LLVM 18 | — | yes | yes (ELF64 x86-64; NEEDED = system `.sprx` only; no Mesa linked) | — | — | |
| 2026-11-10 | PS5 cross-build, bundle v0.40.3 (SW) | — | yes | yes (same NEEDED) | — | — | |
| 2026-11-10 | **console, HW bundle v0.41.1 via websrv homebrew launcher, no `-rdynamic`** (`ksre-02b-hw-hb-20261110T053235Z.log`) | — | — | — | yes (ran; libraries found in the working directory; `dlopen("libEGL.so.1")` succeeded) | **FAIL: no GL context for any API or request variant.** RADV could not create a device: `radv/ps5: sceAgcInit(8) failed: 0x8a6c002f`, `the platform did not initialise; no device`, `ZINK: failed to choose pdev`, `egl: failed to create dri2 screen`, SDL `Could not initialize EGL`. Just before it, Sony's `libSceAgc` printed `[Agc] FS Table offset has shifted. This is not survivable.` TV: black screen (maintainer). No shader or draw code ran. | after the run the payload crashed with SIGSEGV during process exit (stage `shutdown`, after `KSRE_PS5_02B_END`); afterwards websrv, ftpsrv and elfldr stopped answering and the console stopped answering ping for a while. Causality not established. |
| 2026-11-10 | console, SW bundle v0.40.3, **before the `-rdynamic` fix** (13 runs, `ksre-02b-sw-hb-20261110T061101Z.log` … `T070437Z.log`) | — | — | — | yes (ran; `libOSMesa.so.8` found; context created) | **FAIL: SIGSEGV in the first draw-like call** (`glGenerateMipmap`, `rip=0`), also with `GALLIUM_DRIVER=softpipe DRAW_USE_LLVM=0`. Cause found afterwards: unresolved weak import `kernel_mprotect` (see findings). Earlier crash handler printed wrong registers; fixed. | |
| 2026-11-10 | first working run, `-rdynamic`, 60 frames (`ksre-02b-sw-hb-20261110T070956Z.log`, plus the payload's own log fetched over FTP: `ksre-02b-sw-console-last-run-20261110T0711Z.log`) | — | — | — | yes | **PASS: `END fail=0`**, same probes, 60-frame numbers within 1–2 % of the 300-frame run. The runner log is cut after 4 benchmarks (the runner bug below), the FTP copy is complete | |
| 2026-11-10 | **console, SW bundle v0.40.3, `-rdynamic`, 300 frames, vsync 1** (`ksre-02b-sw-hb-20261110T072331Z.log`) | — | — | — | yes | **PASS: `END fail=0`.** llvmpipe (LLVM 14.0.6) GL 4.5 compat; 7/7 read-back probes pass; 0 GL errors; `scene_window` 9.9 ms render / 59.9 fps, `fill8` 36.5 ms / 20 fps, `fbo_matrixcolor` 26.7 ms / 30 fps, `blur_cached` 49.0 ms / 18.8 fps, `blur_rerender` 69.4 ms / 12 fps. Picture on TV not reviewed by the maintainer (the maintainer was asked not to touch the pad; no report) | clean exit by `_exit()`; returning from `main` crashed (below) |
| 2026-11-10 | console, SW bundle, `-rdynamic`, returning from `main` (`--normal-exit` equivalent; `ksre-02b-sw-hb-20261110T072022Z.log` (a pad button also ended it early at `blur_rerender` frame 188) and the 10-frame `T072251Z.log`; the 10-frame `--fast-exit` run `T072300Z.log` is clean) | — | — | — | yes | benchmark and `END fail=0` as above, **then SIGSEGV at stage `shutdown`** (jump into unloaded GL library range); `_exit()` avoids it | |
| 2026-11-10 | console, HW bundle v0.41.1 retest with `-rdynamic` (`ksre-02b-hw-hb-20261110T072541Z.log`) | — | — | — | yes | **FAIL, identical:** `sceAgcInit(8) failed: 0x8a6c002f`, no device, no context for any request. Ended in 4 s; loader services stayed up (`_exit()`) | |

### Findings

* **`-rdynamic` is mandatory for payloads that load Mesa** (console + source).
  `libOSMesa.so.8` contains the SDK's `mmap`, which makes executable memory by
  calling the weak import `kernel_mprotect(-1, addr, len, prot)`; the SDK's
  run-time linker resolves it only against the executable's exported symbols.
  Without `-rdynamic` the call goes to address 0. Found from the crash stack:
  the return address mapped (via `llvm-nm`/`llvm-objdump` on the unstripped
  library and the `0x29f4000` alias delta) to `mmap`+0xb1 ← `rtasm_exec_malloc`
  ← `x86_init_func_common` ← `build_vertex_emit`, i.e. Mesa's vertex-fetch
  code generator, which the draw module uses with or without LLVM.
* **llvmpipe works on the console** (JIT included) once that import is
  resolved; numbers in the results table and in `docs/GRAPHICS.md`.
* **Exit:** return from `main` after unloading the GL library crashes; use
  `_exit()` (default). The PS5 signal frame has extra words before `mc_rip`
  (cs `0x43` at word 27); the crash handler decodes the raw words.
* **Runner bug fixed:** `ps5_hbrun.py` used `http.client` with a socket
  timeout; after the first quiet stretch Python's socket file raises `cannot
  read from timed out object` on every read, so long silences looked like a
  dead payload and the tool waited for its whole `--timeout`. It now polls a
  raw socket with `select`, decodes chunked transfer itself, and stops at the
  payload's end marker.

* **Library lookup works** (console): libraries placed in the payload's working
  directory are found by the SDK linker; no `/user/homebrew` is needed.
* **The websrv `env=` parameter reaches the payload** (console):
  `getenv("MESA_DEBUG")` returned `1`. Whether Mesa's own libc sees it was not
  established (Mesa links the console's `libSceLibcInternal`).
* **The hardware path fails before any GL negotiation.** All six context
  requests (Ren'Py's two plus fallbacks) fail the same way at EGL display
  initialisation, so this run says nothing about which GL/GLES versions zink
  would offer. The message that precedes it comes from the system library
  `libSceAgc.sprx` at load time, not from Mesa (the string is not in any Mesa
  patch or in libvulkan). A firmware-specific mismatch between the Mesa patches
  (`PS5_AGC_INIT_VERSION 8`) and firmware 7.61's AGC is a hypothesis
  (inference); nothing in the recipe or release notes states a supported
  firmware.
* The SIGSEGV during process exit was seen once, after Mesa/AGC had been
  loaded (and after `SDL_Quit` returned and the end marker was written). 02a,
  which never loads those libraries, also ended with an error shown by the
  console (text never recorded), so exit behaviour of homebrew big apps is
  still an open item (plan risk R10); this run does not separate the causes.
