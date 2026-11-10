# Status

Last updated: 2026-11-10 (milestone 02a). Statuses are deliberately separate: compiling is
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
| Runtime probe (`experiments/01-runtime-probe`) | yes (macOS arm64; CI Linux + macOS) | yes (Apple clang, SDK v0.45 headers) | yes (CI, LLVM 18) | yes (elfldr 9021) | yes (fw 7.61, 29 pass / 1 fail) | n/a |
| Link inventory for CPython configure | n/a | n/a | yes (CI): 152/173 link | n/a | n/a | n/a |
| SDL2 display + image + text (`experiments/02a-sdl-display`) | yes (macOS arm64, dummy video driver) | yes | yes (pacbrew v0.41.1 and v0.40.3) | yes (elfldr and websrv homebrew launcher) | partial: ran to its end marker with captured logs; picture appears only when launched as homebrew; maintainer reported it "seemed to work" but aspect ratio is unconfirmed and the console showed an error afterwards | n/a |
| SDL2 GL / input mapping / audio experiments | — | — | — | — | — | — |
| CPython 3.12.8 | — | — | — | — | — | — |
| Ren'Py 8.5.3 native modules | — | — | — | — | — | — |
| Ren'Py original test game | — | — | — | — | — | — |
| KSRE v2.0.5 | — | — | — | — | — | — |
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
