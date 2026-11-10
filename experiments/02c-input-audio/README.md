# Experiment 02c — controller events, generated tone, storage round-trip

**Questions:** (1) does SDL treat the PS5 pad as a GameController and which events
arrive for every control; (2) does SDL audio play a tone generated in code at 48 kHz;
(3) does `/data/ksre-ps5/` survive a write → fsync → rename → read round-trip and
persist across runs.

`main.c` runs: tone self-test (zero-crossing check of the generator), storage test,
device listing with the SDL mapping string, a 3 s 440 Hz tone (callback rate measured),
then a 45 s input window that logs every joystick/controller event (axes are logged on
steps of ≥ 6000 and summarised) and tracks a coverage checklist. The window ends after
the time limit or when **L1+R1+Options** are held for 1 s. The TV shows a pad-shaped
display (tiles turn green once seen, yellow while held). A watchdog, a crash marker
and the PS5 signal-frame decode are copied from 02b; the log also goes to
`/data/ksre-ps5/02c/last-run.log`; the run ends with `KSRE_PS5_02C_END fail=N` and `_exit()`.

Options: `--window-ms`, `--tone-ms`, `--combo-ms`, `--data-dir`, `--mapping STR`
(used only if the pad is not recognised), `--require-coverage`, `--no-audio`,
`--no-window`, `--selftest-only`, `--virtual-pad` (host aid), `--normal-exit`.

## Build and run

```sh
source .local-config/env.sh
make -C experiments/02c-input-audio host && make -C experiments/02c-input-audio run-host   # dummy drivers + virtual pad
make -C experiments/02c-input-audio ps5                                                    # swgl bundle v0.40.3
tools/deploy-hb.sh ksre-02c build/experiments/02c-input-audio/ps5/ksre-ps5-02c-input-audio.elf \
  --timeout 110 --idle-timeout 40 "--args=--require-coverage"                               # ~52 s, stops itself
```

## Results

"—" = not attempted. Console rows cite logs in `.local-work/logs/` (not committed).

| Date | Variant | host | compiles | links | loader | console | notes |
|---|---|---|---|---|---|---|---|
| 2026-11-10 | macOS arm64, SDL 2.32, dummy drivers + virtual pad | yes: tone self-test, storage, 23/23 required controls, combo ends the window | — | — | — | — | |
| 2026-11-10 | PS5 cross-build, bundle v0.40.3, SDK v0.45, `-rdynamic` | — | yes | yes | — | — | |
| 2026-11-10 | **console**, homebrew launcher, 45 s window (`ksre-02c-hb-20261110T074443Z.log`) | — | — | — | yes | **PASS `END fail=0`** | pad `User1` is a GameController natively (no mapping string needed); 23/23 required controls plus touchpad produced controller events; tone callback 48000.7 frames/s over 3 s; the maintainer heard the tone; storage round-trip passes; run 52 s, TV back at the home screen afterwards |
| 2026-11-10 | console, short rerun (`ksre-02c-hb-20261110T074607Z.log`) | — | — | — | yes | `END fail=0` | run counter 1 → 2: `/data/ksre-ps5/` persists across payload runs |

### Findings (console)

* **Devices:** `#0 User1` (GUID `0300d0424c050000e60c000011810000`, 17 buttons, 6 axes, 1 hat,
  rumble + LED) and `#1 PS5 Remote Control` (GUID `030000004c050000cec0000011810000`, 17 buttons,
  0 axes). Both are GameControllers; the second sent no events. The pad's instance id was 1.
* **Mapping (reported by SDL, from the driver's hook):**
  `0300d0424c050000e60c000011810000,User1,a:b0,b:b1,x:b2,y:b3,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,touchpad:b4,platform:FreeBSD`.
  No `back`, `guide`, `misc1` or paddle binding; the PS and Create buttons are not delivered
  (the driver has no entry; not pressed in the test).
* **Triggers:** joystick axes 4/5 rest at −32768/−32767 and reach +32767; the GameController
  value is rescaled to 0…32767 (0 at rest after use). L2/R2 also appear as joystick buttons 15/16
  (not controller buttons). Sticks reach ±32767 and rest within ±150. The D-pad is both buttons
  11–14 and hat 0 (values 1/2/4/8).
* **Audio:** device opened as 48000 Hz, F32, 2 channels, 1024 samples (8192 bytes); driver `ps5`;
  longest gap between callbacks 21.4 ms (period 21.3 ms). The driver forces 48 kHz.
* **Storage:** 1 MiB written with fsync + rename in 47 ms, read back by stdio and `SDL_RWops`
  with matching checksums, overwrite-by-rename, directory listing and unlink all pass.
  `SDL_GetBasePath` is the payload directory under `/data/homebrew/`.
