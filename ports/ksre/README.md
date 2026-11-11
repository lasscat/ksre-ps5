# KSRE v2.0.5 on the PS5 (platform overlay + staging)

KSRE's own files stay unmodified. This directory adds:

| File | Purpose |
|---|---|
| `zz_ksre_ps5.rpy` | Platform overlay (GPL-3.0-or-later), copied into `game/`: replaces KSRE's Discord Rich Presence thread (`rpc`) before `init 999` starts it, makes self-voicing TTS spawn nothing (`config.tts_function`), starts in full screen (`config.default_fullscreen`) |
| `stage.sh` | Copies the maintainer's `game/` (docs/GAME_DATA.md) to `build/ports/ksre/stage/game`, adds the overlay and `script_version.txt`, compiles `.rpyc` + `cache/` on the host twin |
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
