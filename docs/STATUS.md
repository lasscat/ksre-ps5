# Status

Last updated: 2026-11-13 (milestone 06b). Statuses are deliberately separate: compiling is
not loading, loading is not running, running a test is not playing the game.

| Status | Meaning |
|---|---|
| `host` | Built and run on a development machine (not PS5 evidence) |
| `compiles` | Compiles for `x86_64-sie-ps5` (object files only) |
| `links` | Links into a PS5 payload ELF |
| `loader` | ps5-payload-elfldr accepted and started the payload |
| `console` | Ran to completion on a real PS5 with the expected output |
| `game` | Works across the full KSRE game |
| — | not attempted |

## Components

| Component | host | compiles | links | loader | console | game |
|---|---|---|---|---|---|---|
| Runtime probe (`experiments/01-runtime-probe`) | yes (macOS arm64; CI Linux + macOS) | yes (Apple clang, SDK v0.45 headers) | yes (local + CI, LLVM 18) | yes (elfldr 9021) | yes (fw 7.61, 29 pass / 1 fail) | n/a |
| Link inventory for CPython configure | n/a | n/a | yes (local + CI): 152/173 link; header-aware: 1 header-inline (`uname`), 20 absent | n/a | n/a | n/a |
| SDL2 display + image + text (`experiments/02a-sdl-display`) | yes (macOS arm64, dummy video driver) | yes | yes (pacbrew v0.41.1 and v0.40.3) | yes (elfldr and websrv homebrew launcher) | partial: ran to its end marker with captured logs; picture appears only when launched as homebrew; maintainer confirmed the picture and aspect ratio were correct; the console showed an error after the run ended (text not recorded) | n/a |
| OpenGL / Ren'Py-style shaders (`experiments/02b-gl-shaders`) | yes (macOS GL 2.1: 7/7 read-back probes pass; GLES dialect not runnable on that host) | yes | yes (HW and SW pacbrew bundles) | yes (homebrew launcher; Mesa libraries found in the payload directory) | **SW bundle (OSMesa/llvmpipe): works**: GL 4.5 compat context, 7/7 read-back probes pass, 0 GL errors, 1080p scene 59.9 fps, full-screen blur 12–19 fps (300 frames per test; needs `-rdynamic`, exit via `_exit`). **HW bundle: fails**, no GL context (`sceAgcInit(8)` 0x8a6c002f on firmware 7.61, also with `-rdynamic`) | n/a |
| Controller / audio / storage (`experiments/02c-input-audio`) | yes (macOS arm64, dummy drivers + virtual pad) | yes | yes | yes (homebrew launcher) | **works**: pad recognised natively as a GameController, 23/23 required controls + touchpad produce events, 48 kHz tone callback on time and audible, `/data/ksre-ps5/` write/fsync/rename/read round-trip and persistence across runs pass | n/a |
| CPython 3.12.8 static (`ports/cpython`) | yes (host build Python 3.12.8; macOS arm64, CI Linux) | yes (2 configure patches + 3 run-time patches from 03b) | yes: `libpython3.12.a`, 76 built-in extension modules; CPython's `python` program links (local + CI) | yes | **yes: 272/272 stdlib imports; curated CPython tests 2494 run, 41 fail/err, all explained (console FS/libc semantics, no `_testcapi`)** | n/a |
| Payload embedding Python (`experiments/03-python-hello`) | yes (same launcher + script on macOS arm64 against the host build: 10/10 checks) | yes | yes (local + CI) | yes | **yes: 10/10, interpreter ready 0.019 s, ru_maxrss 43 MiB** | n/a |
| Ren'Py C dependencies (`ports/ffmpeg`, `ports/build-all-deps.sh`) | n/a | yes (LGPL FFmpeg 7.1.5 with x86 asm, no patches; other libraries from pacbrew v0.40.3) | yes: `build/sysroot` static libraries; a link check of FFmpeg, FreeType, HarfBuzz, FriBidi, libpng, libjpeg, libwebp, SDL2_image, zlib/bz2/xz; Ren'Py `ffmedia.c`/`ftsupport.c` compile against them | n/a | not run (decode benchmark deferred to 06b) | n/a |
| Ren'Py 8.5.3 native modules + launcher (`ports/renpy`) | yes: host twin (same generated C, inittab, launcher and runtime tree, built for macOS) runs the test game | yes: 60 modules, 72 sources, no source changes | yes: `ksre-ps5-renpy.elf` (local + CI step in ps5-cross-build) | yes (websrv homebrew launcher) | **yes**: Ren'Py 8.5.3 starts and runs a game (450/450 imports, gl2 on OSMesa/llvmpipe); needs the launcher's no-spawn guard (a fork/exec powered the console off), `LD_LIBRARY_PATH` for `libOSMesa.so.8`, and ends with SIGKILL because `exit` is refused | n/a |
| Ren'Py original test game (`fixtures/testgame`) | yes: unattended self-test 11/11 on the host twin (gl2, pixel read-back, ATL, audio, persistent, game menu, save, load, rollback, choice); official SDK reference run: pending | n/a | n/a | yes | **yes**: self-test 10/10 on the console (before the `gamemenu` check was added); interactive checklist with the pad confirmed by the maintainer (dialogue, choices, ATL, chime, save, load, rollback, persistent start count) | n/a |
| KSRE v2.0.5 (`ports/ksre`) | yes: `.rpyc` compiled on the host twin, game starts full screen | n/a | n/a | yes | **partial (05)**: title screen, new game, first scenes and the opening video with sound (maintainer + log); colours match the host; saves work; very slow transitions/text animation on software GL (06b); **06a: every menu/settings screen and the in-game menu navigated with the pad only (maintainer + log)**; **06b: full-screen videos 28–30 fps, credits video 20 fps, dissolves 16–19 fps, wipes 13–15 fps, text ~20 fps; music/sound/ambient together; maintainer: no stutter, sound in sync (benchmark logs)** | — |
| Launcher / packaging | — | — | — | — | — | — |

"CI" means the GitHub Actions job `ps5-cross-build` performs the step; check
the latest run for its result. The runtime probe has now been loaded by
ps5-payload-elfldr and run to completion on a real PS5 (firmware 7.61): it
ran twice to its end marker with 29 pass / 1 fail (the fail is the FPU
flush-to-zero deviation described in the experiment README), confirming
threads, emulated TLS, JIT (W^X), ~2 GiB memory headroom, writable and
persistent `/data`, and the dynamic-loading and event paths CPython and
Ren'Py need. See
[experiments/01-runtime-probe](../experiments/01-runtime-probe/README.md).

## Known blockers

See the blocker table in [ARCHITECTURE.md](ARCHITECTURE.md#open-blockers-and-the-smallest-experiment-for-each).

## Milestone 02a (SDL2 display) in short

The pinned PS5 SDL2 port (2.30.12, pacbrew bundle v0.41.1) plus SDL2_image and
SDL2_ttf cross-build and link into a payload that renders an original test
card and UTF-8 text through SDL's software renderer at 1920×1080@60. Sent
straight to elfldr (port 9021) the payload ran and presented 600 frames but
nothing appeared on the TV; started through the websrv homebrew launcher
(foreground application) the picture appeared. Open items: the error the
console showed after the payload ended, and the aspect-ratio check. Details,
logs summary and caveats:
[experiments/02a-sdl-display](../experiments/02a-sdl-display/README.md) and
[CONSOLE_TESTING.md](CONSOLE_TESTING.md#payloads-that-draw-to-the-tv-milestone-2a-onwards).

## Milestone 02b (OpenGL for Ren'Py) in short

A GL experiment that requests Ren'Py's GL attributes, resolves Ren'Py's 58
required functions, assembles Ren'Py's own shader parts (texture, alpha,
matrixcolor, blur) in both GLSL dialects, draws with Ren'Py's call patterns and
checks the pixels it reads back, builds for the host and for both pacbrew
bundles. On a host GPU and on the console's **software** path (Mesa OSMesa /
llvmpipe, v0.40.3) every read-back check passes with no GL errors; at 1080p a
typical dialogue scene renders in 9.9 ms (60 fps) while full-screen effects
cost 25–50 ms each (matrixcolor 30 fps, blur 12–19 fps). The **hardware**
bundle (Mesa 26.2, zink on RADV-on-AGC) cannot create a Vulkan device on
firmware 7.61, so it provides no GL context. Decision: software path, `gl2`,
1080p, payloads linked with `-rdynamic`, exit via `_exit()`. Whether KSRE is
playable at those speeds is not yet known. Details, the draft upstream
issues and the root-cause write-up: [GRAPHICS.md](GRAPHICS.md) and
[experiments/02b-gl-shaders](../experiments/02b-gl-shaders/README.md).

## Milestone 03a (CPython cross-build) in short

CPython 3.12.8 cross-compiles for the PS5 as a static library with every stdlib extension
module built in (76; `_ctypes`, `_ssl`/`_hashlib`, `grp`, multiprocessing and the
terminal/database/GUI modules are left out, with reasons in
[ports/cpython](../ports/cpython/README.md)). configure runs with
`--host=x86_64-unknown-freebsd` (so `sys.platform` is `freebsd`) after two small configure
patches, takes math from the console's libc, and gets its cross-compiling answers from the
01a/01b evidence. The pure-Python stdlib is packed into a 6.5 MB zip. A test payload that
embeds Python links. Details:
[experiments/03-python-hello](../experiments/03-python-hello/README.md).

## Milestone 03b (CPython on the console) in short

CPython 3.12.8 runs on the PS5 (fw 7.61) from an elfldr payload: interpreter ready in 0.02 s,
all 272 stdlib modules import (except the ones not built), and a curated part of CPython's
test suite (2494 tests: struct, json, zlib, time, float, math, threading, os, asyncio) passes
except for 41 explained cases (console filesystem/libc semantics, no `_testcapi`). Five console
problems were fixed in `ports/cpython` (`struct tm` zone, thread FTZ/DAZ, `clock_nanosleep`,
`wcsftime` hang, `fdopendir`); see [ports/cpython](../ports/cpython/README.md).

## Milestone 03c (Ren'Py C dependencies) in short

FFmpeg 7.1.5 is built LGPL-only (native decoders, x86 asm, no external codec libraries) into
`build/sysroot`; the text and image libraries are staged from the pinned pacbrew bundle, and a
link-only program proves they all resolve. AVIF, assimp and Live2D are left out. Details,
versions and licences: [ports/ffmpeg](../ports/ffmpeg/README.md).

## Milestone 04a (Ren'Py native modules, launcher, test game) in short

Ren'Py 8.5.3's native modules are generated with Cython 3.1.4 (`RENPY_STATIC=1`), compiled
unchanged for the PS5 and registered through our own inittab; with this project's launcher,
the static CPython and the 03c libraries they link into one payload. `renpy.gl2.assimp`
(assimp) and `renpy.gl2.live2dmodel` (Live2D) are not built; a Python stub replaces the
assimp module. The same build compiled for the Mac (host twin) runs an original test game's
self-test with every check passing. Details:
[ports/renpy](../ports/renpy/README.md), [fixtures/testgame](../fixtures/testgame/README.md).

## Milestone 04b (Ren'Py test game on the console) in short

Ren'Py 8.5.3 runs the original test game on the PS5 (firmware 7.61): self-test 10/10 and the
interactive checklist with the controller confirmed by the maintainer. Three console facts
needed launcher changes: a child process (`platform.platform()` → `subprocess`) powered the
whole console off, so all process spawning now raises `OSError`; the launcher's `chdir()` hid
`libOSMesa.so.8` from the SDK linker, so `LD_LIBRARY_PATH` points at the payload directory;
the `exit` system call is refused after a run, so the payload ends with `SIGKILL` after its
end marker. ftpsrv does not fsync: files uploaded just before a console crash were damaged.
The test game needed `layout.defaults()` for its menus and a visible focus style.

## Milestone 05 (KSRE title screen and opening) in short

KSRE v2.0.5, unmodified plus this project's overlay (`ports/ksre`), runs on the console:
4LS splash video, title screen, new game, the first scenes and the opening video with sound
(maintainer confirmation + captured logs); main-menu colours match the host twin. The first
run failed because the app has only 448 MiB of flexible memory and CPython's mmap()ed object
arenas used it up; the launcher now backs them with malloc. Everything is very slow on the
software GL path (transitions, text, animations); performance is milestone 06b. Details:
[ports/ksre](../ports/ksre/README.md).

## Milestone 02c (controller, audio, storage) in short

On the console, SDL's PS5 driver exposes the pad as a GameController without any extra
mapping string. Every face button, D-pad direction, shoulder, stick click, Options, touchpad
click, both sticks and both triggers produced controller events; the PS and Create buttons
are not delivered. A tone generated in code played through SDL audio (48 kHz, callback on
time, heard by the maintainer) and a file round-trip on `/data/ksre-ps5/` passed and
persisted across runs. Details: [experiments/02c-input-audio](../experiments/02c-input-audio/README.md).

## Milestone 06b (video, audio, performance) in short

An unattended benchmark overlay (debug only) measured KSRE on the console. All KSRE videos are
VP9 1080p30 + Opus. Full-screen videos now hold 28–30 fps: the launcher sets Ren'Py's swap
interval to 0, because the PS5 flip already waits for the vblank. With interval 1 they dropped
to 19–25 fps. KSRE's camera function asked for a redraw every frame even with parallax off;
the overlay stops that, which doubles transition speed (dissolve 9 → 16–19 fps) and lets still
screens stop redrawing. Music, sound and ambient play together at the right rate. The rest of
the slowness is software GL at 1080p: per-frame cost is in GL, not Python, and
`LP_NUM_THREADS` and mipmaps do not help. Details:
[ports/ksre](../ports/ksre/README.md#performance-06b).
