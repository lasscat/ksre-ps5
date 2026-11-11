# ksre-ps5 test game

An original, tiny Ren'Py 8.5.3 project for checking the port (text, script and generated
assets by this project, GPL-3.0-or-later). `ports/renpy/build.sh testgame` copies `game/` to
`build/ports/renpy/testgame/` and runs `make_assets.py` there (stdlib only, deterministic:
two PNG images, two WAV sounds). Generated files are never committed.

* **Interactive path** (`script.rpy`, normal start): dialogue, a two-way choice with different
  marker motion, a chime, then instructions to save, load, roll back and restart (the start
  count is persistent). This is the 04b console checklist. Menus are Ren'Py's built-in classic
  layouts, switched on with `layout.defaults()` in `options.rpy` (without it the game menu does
  nothing and there is no main menu), plus a gold focus bar so the pad's selection is visible.
  Pad: Cross = advance/select, D-pad = move, Options or Circle = game menu (opens on Save),
  L1 = roll back.
* **Self-test** (`selftest.rpy`): set `KSRE_TESTGAME_SELFTEST=1`. The game skips the main
  menu, prints `TESTGAME PASS|FAIL|INFO name detail` lines, ends with
  `TESTGAME END pass=N fail=M ...` and quits with status 0 or 1. Checks: renderer is gl2/gles2
  with models, saves go to `RENPY_PATH_TO_SAVES`, persistent file written (`runs=` count
  goes up per run), a game menu screen exists (`gamemenu`), ATL runs over time, screenshot pixels match the background colours
  (GL draw + read-back), WAV playback position advances, both choice outcomes, save,
  load restores the store, rollback restores the store.

Reference run with the official SDK (macOS; saves kept under `build/`):

```sh
KSRE_TESTGAME_SELFTEST=1 RENPY_PATH_TO_SAVES="$PWD/build/ports/renpy/sdk-saves" \
  external/toolchains/renpy-sdk/renpy-8.5.3-sdk/renpy.sh "$PWD/build/ports/renpy/testgame"
```

The SDK zip is pinned in `upstreams.lock` (`RENPY_SDK_URL`, `RENPY_SDK_SHA256`) and unpacked to
`external/toolchains/renpy-sdk/`; it is used only on the development machine.
