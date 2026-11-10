# Experiment 02a — SDL2 display, image and text, clean exit

**Question:** can a payload built against the pinned PS5 SDL2 port put an
original image and rendered text on the TV, animate, and exit cleanly?

The payload (`main.c`) opens a 1920×1080 SDL window, draws an **original
generated test card** (a deterministic PNG made by `tools/gen_image.py`:
gradient, 16:9 grid, circle, border and corner markers, colour bars), renders
UTF-8 text with SDL2_ttf (DejaVu Sans, fetched at build time with a pinned
sha256 and embedded; never committed), animates for ~10 s (progress bar,
orbiting markers, bouncing square, a live fps line), and exits. It logs
`SDL_GetError` at every failure point, the display modes, the renderer list
and info, joystick devices, per-second frame statistics, and which kind of
process it runs as. A watchdog thread ends the process if it overruns its
duration by 20 s (the PS5 present path blocks on a kernel flip event).

The renderer is SDL's **software** renderer on purpose: on the PS5 port it
draws into the VideoOut framebuffer path and never touches Mesa. Hardware GL
is milestone 02b. Early exit: `SDL_QUIT`, Escape, or any joystick button
(raw index logged; controller mapping is 02c).

## Build

```sh
tools/fetch-pacbrew.sh hwgl          # pinned bundle v0.41.1 -> external/toolchains/ (sha256-verified)
make -C experiments/02a-sdl-display ps5
make -C experiments/02a-sdl-display ps5 PACBREW=swgl   # v0.40.3 bundle (run tools/fetch-pacbrew.sh swgl first)
make -C experiments/02a-sdl-display run-host           # headless logic test, system SDL2
```

`tools/fetch-pacbrew.sh` extracts only the bundle's library prefix
(`user/homebrew`) next to the SDK, so the pinned SDK is never modified; see
the script header for how it overlays the SDK. The host build uses the
system SDL2 (Homebrew/apt) and is never linked into a PS5 build.

## Run on the console

Graphics payloads must be started as homebrew, **not** through the bare
elfldr port; see `docs/CONSOLE_TESTING.md` ("Payloads that draw to the TV").

```sh
tools/deploy-hb.sh ksre-02a build/experiments/02a-sdl-display/ps5/ksre-ps5-02a-sdl-display.elf
```

## Results

Statuses are kept separate. "—" means not attempted.

| Date | Variant | host | compiles | links | loader | console | notes |
|---|---|---|---|---|---|---|---|
| 2026-11-10 | host: macOS arm64, SDL 2.32 (sdl2-compat), dummy driver | yes (606 frames, `fail=0`) | — | — | — | — | logic test only; screenshot of the frame reviewed |
| 2026-11-10 | PS5 cross-build, bundle v0.41.1, SDK v0.45, LLVM 18 | — | yes | yes (ELF64 x86-64 PIE; NEEDED are system `.sprx` only, no Mesa) | — | — | linked SDL reports `2.30.12-gee4c47dc0`, the pinned `PS5_SDL_COMMIT` |
| 2026-11-10 | PS5 cross-build, bundle v0.40.3 | — | yes | yes | — | — | SDL `2.30.12-ga47182a20` |
| 2026-11-10 | console, bare elfldr (9021), bundle v0.41.1 | — | — | — | yes | **no picture**: log shows `fail=0`, 600 frames, 59.9 fps, 1920×1080@60 ABGR8888, software renderer; maintainer saw only the home screen | log captured; keyboard and pad open failed (`0x809b0081`); payload was a background process |
| 2026-11-10 | console, bare elfldr (9021), bundle v0.40.3 | — | — | — | yes (second payload accepted straight after the first exited) | same as above | log captured |
| 2026-11-10 | console, **websrv homebrew launcher**, bundle v0.41.1, two runs | — | — | — | yes | log: `fail=0`, 600 frames, 59.9 fps, worst 18 ms; big app id non-zero; pad opens (17 buttons, 6 axes, 1 hat; GUID `0300d042…`); maintainer: "it seemed to work" | **picture details not yet confirmed** (aspect ratio, edge cropping), and the maintainer reported an **error shown by the console after the run**; its text is not yet recorded |

Open at the end of 02a: the exact error after exit and whether it is caused by
how the payload ends (the process simply returns from `main`); the aspect
ratio check; early exit by joystick button/Escape was not exercised.

### Findings (console, from the captured logs)

* The PS5 SDL port reports a single display, 1920×1080@60 Hz ABGR8888, and
  exactly one display mode. `SDL_GetDisplayDPI` is unsupported.
* With the software renderer, 1080p full-screen blit + text + fills hold
  59.9 fps (vsync-paced), worst frame 18 ms. Headroom is not measured; that
  needs an unpaced benchmark (02b).
* SDL2_ttf 2.22.0 in the bundle is built **without HarfBuzz** (the recipe
  disables it: "harfbuzz is currently crashing when used with SDL2_ttf").
  FreeType rendering of UTF-8 text works. The default C locale is irrelevant
  because strings are handed over as UTF-8.
* Bare elfldr payloads cannot show a picture; launch through websrv's `/hbldr`.
