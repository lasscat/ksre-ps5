# KSRE v2.0.5 on the PS5 (platform overlay + staging)

KSRE's own files stay unmodified. This directory adds:

| File | Purpose |
|---|---|
| `zz_ksre_ps5.rpy` | Platform overlay (GPL-3.0-or-later), copied into `game/`: replaces KSRE's Discord Rich Presence thread (`rpc`) before `init 999` starts it, makes self-voicing TTS spawn nothing (`config.tts_function`), starts in full screen (`config.default_fullscreen`), draws no KSRE mouse cursor (`config.mouse_displayable = None`; there is no mouse); 06b: while KSRE's parallax option is off, `tf_parallax` (KSRE's camera function) stops asking for a redraw every frame |
| `stage.sh` | Copies the maintainer's `game/` (docs/GAME_DATA.md) to `build/ports/ksre/stage/game`, adds the overlay and `script_version.txt`, compiles `.rpyc` + `cache/` on the host twin |
| `debug/zz_ksre_ps5_bench.rpy` | Debug only, never staged: with `KSRE_PS5_BENCH=1`, replaces the splash screen with an unattended media/performance benchmark and quits (one `KSRE_PS5_BENCH seg=…` line per segment); `KSRE_PS5_BENCH_SHORT=1`, `KSRE_PS5_BENCH_SCENES_ONLY=1` |
| `debug/zz_ksre_ps5_shots.rpy` | Debug only, never staged: with `KSRE_PS5_SHOTS=DIR`, periodic screenshots + one of the main menu (colour comparison with the host twin) |

Decisions (05):

* **`.rpyc` are compiled on the host twin** (`ksre-ps5-renpy-host … compile`, same Ren'Py
  8.5.3 + CPython 3.12.8 as the payload; 310 files, ~15 s). The console then only loads them.
* **`script_version.txt` = `(8, 5, 3)`** is what Ren'Py's own distribution build adds
  (`launcher/game/distribute.rpy`). Without it a source checkout runs in developer mode and
  backs up all scripts into the save directory on every start.
* Console layout: game at `/data/ksre-ps5/ksre/game`, saves in
  `/data/ksre-ps5/saves/sh.fhs.ksre/`.
* KSRE and its bundled `pypresence` contain no `subprocess`/`os.system`/`webbrowser` call
  (source grep). Ren'Py's own error handler tries `xdg-open` (`renpy/editor.py`); the launcher's
  no-spawn guard refuses it (console, 05).

```sh
ports/renpy/build.sh elf && ports/renpy/build.sh host
ports/ksre/stage.sh                        # -> build/ports/ksre/stage (1.4 GB)
B=build/ports/renpy
tools/deploy-hb.sh ksre-renpy $B/ksre-ps5-renpy.elf --upload-only \
  --extra external/toolchains/pacbrew-v0.40.3/user/homebrew/lib/libOSMesa.so.8 \
  --tree build/cpython/ps5/python-home/lib:/data/ksre-ps5/python/lib \
  --tree $B/runtime/renpy:/data/ksre-ps5/renpy --tree build/ports/ksre/stage:/data/ksre-ps5/ksre
# ~35 s later (1.55 GB uploads in ~40 s):
tools/deploy-hb.sh ksre-renpy $B/ksre-ps5-renpy.elf --no-upload --env "RENPY_LOG_TO_STDOUT=1" \
  --args="--env KSRE_PS5_MEMLOG=10 --game /data/ksre-ps5/ksre --max-seconds 600" \
  --timeout 620 --idle-timeout 620
```

## Results

| Date (UTC) | What | host | console |
|---|---|---|---|
| 2026-11-11 | stage + compile on the host twin; 45 s run | yes: 310/310 `.rpyc`, script load 1.2 s, full screen, no traceback | n/a |
| 2026-11-11 | console run 1 | n/a | **FAIL**: scripts loaded (11.9 s), then `RuntimeError: can't start new thread` (savelocation scan thread); clean exit |
| 2026-11-11 | console, `KSRE_PS5_MEMLOG=1` | n/a | cause: the app has **448 MiB flexible memory**; CPython's mmap()ed object arenas used it all (0 MiB at 6 s) while malloc (direct memory) kept going to 800 MB RSS |
| 2026-11-11 | console run 3, launcher arenas via malloc | n/a | **yes**: script load 6.9 s, gl2/llvmpipe full screen 1920×1080, 4LS splash video, main menu, new game, first scenes, opening video **with sound** (maintainer), saves work; flexible memory steady at 117–124 MiB free, RSS 1.6 GB; very slow transitions/text/animations (software GL, 06b); ended by `--max-seconds` |
| 2026-11-11 | console screenshots vs host twin (main menu) | reference | colours match: mean RGB of the paper area 253.5/232.1/186.8 vs 253.5/232.0/186.8; intro video frames look right |
| 2026-11-11 | 06a: pad-only navigation, 122 s run (`KSRE_PS5_MEMLOG=30`) | n/a | **yes** (maintainer, controller only): main menu, Options (sliders, toggles), Accessibility, Saves/Load (scrolling), Extras (Jukebox, Gallery, Library, Cinema), Mods, new game, in-game menu (Save, History, Skip, Auto, Return), a choice, back to main menu: all reachable and usable; no traceback in the log; flexible memory steady at 123 MiB free |
| 2026-11-13 | 06b: unattended media benchmark, 5 console runs (170–226 s, self-stopping) | short bench passes on the twin | **yes**: full-screen VP9 1080p30 videos 28.4–30.0 fps with swap interval 0 (19–30 fps with 1); credits `Movie` 20 fps; camera fix doubles transition fps (dissolve 9 → 16–19 fps); music + sound + ambient play together, positions track wall time; maintainer watched the TV: videos fine, sound in sync, all three sounds heard; no traceback; see Performance |

Navigation (06a): Ren'Py's default `config.pad_bindings` are enough (D-pad/left stick focus and sliders, ✕ select, ○ and Options back/menu). KSRE has no text input, so no on-screen keyboard is needed. The Options screen still shows the "Discord activity" and "Hardware cursor" toggles; they have no effect on the PS5.

## Performance (06b)

All 23 KSRE videos are VP9 1920×1080 30 fps + Opus 48 kHz (`.mkv`): the 4LS splash and act
title cards via `renpy.movie_cutscene`, the credits as `Movie` displayables. Measured with
`debug/zz_ksre_ps5_bench.rpy` (console fw 7.61, gl2 on llvmpipe, 1080p, 16 CPUs; frames =
drawn frames, video = frames delivered by `ffmedia.c`).

| Segment | before 06b (swap 1, KSRE camera) | 06b (swap 0, overlay) |
|---|---|---|
| 4LS / tc_act1 / tc_act3_emi / op_1 / tc_act2_hanako (full screen) | 19–30 video fps, varies by run (23.3, 23.3, 29.1, 29.7, 23.9 in run 1) | **28.4 / 29.9 / 29.8 / 30.0 / 29.6** |
| credits_emi (`Movie` displayable) | 20.0–20.9 | 20.0–22.8 (limit: two full-screen layers) |
| still screen | 30 redraws/s (KSRE camera function) | ~9 redraws/s (same as no camera) |
| dissolve (`locationchange`) / sprite (`charachangealways`) | 9.4 / 9.8 fps | 16–19 / 18 fps |
| ImageDissolve (`dotwipe_up`) / `shorttimeskip` | 7.1 / 10.2 fps | 12.7–14 / 14.5 fps |
| text at 40 cps / `heartattack` dissolve | 19.6 / 7.1 fps | 18–21 / 15–16 fps |

* Cause of the transition slowdown: KSRE's `camera … function tf_parallax` returns 0 (redraw
  every frame) even with parallax off, so every transition re-rendered both scenes each frame.
  The colour-blind `matrixcolor` on the camera costs nothing measurable.
* Swap interval 0 is the launcher default now (ports/renpy). The PS5 flip still waits for the
  vblank; the maintainer saw no stutter or tearing and the sound in sync.
* Audio: music, sound and KSRE's `ambient` channel play together; positions advance with
  wall time (4.01 s and 8.02–8.06 s at 4 s and 8 s); default mixers 1.0, none muted. Volume
  sliders were used with the pad in 06a.
* Tried, no gain: `LP_NUM_THREADS=8` (slightly slower), `config.mipmap = False`.
* Remaining limit: software GL at 1080p. Transitions run at 13–19 fps, text at ~20 fps.
* Memory: flexible memory 89–92 MiB free during video (123 MiB in menus); maxrss 1.57 GB.
