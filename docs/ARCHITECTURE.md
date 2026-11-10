# Architecture

Status: **experimental, pre-alpha.** Nothing in this document has been
verified on a PS5 yet unless a line says *console-verified*. Research date:
2026-11-08. Pinned revisions live in [`upstreams.lock`](../upstreams.lock).

Evidence labels used below:

| Label | Meaning |
|---|---|
| **source** | Read in upstream source code at the pinned revision |
| **doc** | Stated by upstream documentation or README, not independently checked |
| **host** | Built or run on a development machine (not evidence about the PS5) |
| **ci** | Built in this repository's CI |
| **console** | Observed on a real PS5 (none yet) |
| **inference** | Reasoned from the above; not tested |

## Goal

Run Katawa Shoujo: Re-Engineered (KSRE) v2.0.5 on a PS5 as a **native PS5
homebrew payload**, preserving the real engine (Ren'Py 8.5.3 on CPython 3.12)
and therefore KSRE's own Python logic, screen language, ATL transforms,
menus, saves, rollback, persistent data and media. KSRE's scripts are not
rewritten and no replacement `.rpy` interpreter is written.

## Execution routes considered

| Route | What executes | Engine preserved | Assessment |
|---|---|---|---|
| **A. Native PS5 payload (chosen)** | An `x86_64-sie-ps5` ELF built with ps5-payload-sdk, loaded by ps5-payload-elfldr into a PS5 OS process | Yes — CPython and Ren'Py compiled for the target | Every layer has an open-source PS5 component (SDK, SDL2 port, Mesa recipes, FFmpeg recipe, launcher). Main risks: GL for Ren'Py's renderer and the first-ever CPython build for this target. |
| B. PS4 homebrew via backward compatibility (OpenOrbis) | PS4 fake-signed package run in the PS5's PS4 mode | Would need the same CPython/Ren'Py port, for Orbis | Not chosen. Needs fake-PKG support from a PS5 HEN; the toolchain README marks itself beta with GPU rendering support listed as a future roadmap item (doc); Ren'Py generates GLSL at run time, which needs a run-time shader compiler (inference: not available to typical PS4 homebrew GLES). Duplicates route A's work with a weaker GPU path. |
| C. Linux on the PS5 (ps5-linux loader) | KSRE's stock Linux x86-64 build | Yes, unmodified | Most practical *for playing*, if the console's model and firmware fall inside the loader's window (press reports describe a hypervisor exploit for early firmware on non-Pro models; not verified here). It replaces the PS5 OS session rather than being PS5 homebrew, so it does not meet this project's goal. Documented as the alternative. |
| D. Browser (KSRE's Ren'Py web build in the PS5's WebKit) | WebAssembly + WebGL in the system browser | Yes | Untested. Unknown WebAssembly/WebGL 2 support and performance in the console browser; 1.5 GB of assets over a local web server. Not pursued. |

Route A is the only route that is both a PS5 application and keeps the
engine intact. Route C is the fallback recommendation for someone who only
wants to play and has eligible hardware/firmware.

## Layers (route A)

```
+---------------------------------------------------------------+
| KSRE v2.0.5 game/ directory (user-supplied, unmodified)       |
| + ksre-ps5 platform overlay .rpy (ours; see "KSRE integration")|
+---------------------------------------------------------------+
| Ren'Py 8.5.3 pure Python (renpy/, renpy/common) + small,      |
| reviewed platform patches                                     |
+---------------------------------------------------------------+
| ~70 Ren'Py native modules (Cython/C), generated on the host   |
| with RENPY_STATIC=1 and linked statically with an inittab     |
+---------------------------------------------------------------+
| CPython 3.12.8: static libpython, built-in extension modules, |
| pure-Python stdlib as files/zip on console storage            |
+---------------------------------------------------------------+
| C libraries: SDL2 (PS5 port), SDL2_image, FFmpeg (LGPL build), |
| FreeType + HarfBuzz, FriBidi, libpng, libjpeg-turbo, libwebp, |
| zlib, bzip2, xz, ...                                          |
+---------------------------------------------------------------+
| GL: Mesa (EGL/zink/RADV on AGC, or OSMesa llvmpipe fallback), |
| loaded at run time by SDL with dlopen                         |
+---------------------------------------------------------------+
| ps5-payload-sdk v0.45: crt, userland dynamic linker (dlopen   |
| of .so and system .sprx), libc supplements, SCE stubs,        |
| emulated TLS                                                  |
+---------------------------------------------------------------+
| PS5 system libraries (libkernel, libSceLibcInternal, VideoOut,|
| Pad, AudioOut, ...) — provided by the console, never shipped  |
+---------------------------------------------------------------+
| Loader: elfldr (development, TCP 9021) / websrv launcher       |
+---------------------------------------------------------------+
```

## Key findings and decisions

### Engine and game revisions (source)

* KSRE's canonical repository is Codeberg `fhs/katawa-shoujo-re-engineered`;
  GitHub `fleetingheart/ksre` describes itself as a mirror and matched
  Codeberg `master` at research time. Pinned: tag `v2.0.5`.
* KSRE's release workflow sets `RENPY_VERSION: 8.5.3` (also on `master`).
  The official 8.5.3 SDK reports `8.5.3.26051504`; the matching Ren'Py and
  renpy-build tags are pinned. Ren'Py `master` is deliberately not used.
* renpy-build for 8.5.3 uses Python 3.12.8, Cython 3.1.4, SDL2 (an SDL3
  branch exists in renpy-build but is not used by 8.5.3), FFmpeg 4.3.1,
  FreeType 2.13.3, HarfBuzz 8.0.1, FriBidi 1.0.7.
* renpy-build compiles Ren'Py's native modules **statically on every
  platform**: `RENPY_STATIC=1 setup.py generate` on the host, compile the
  generated C, archive into `librenpy.a`, and register modules through a
  generated inittab. This is exactly what a PS5 payload needs, so the port
  follows the same method rather than inventing one.

### Target ABI and C runtime (source)

* Target triple `x86_64-sie-ps5` (clang defines `__PROSPERO__`, `__SCE__`,
  `__FreeBSD__=9`, `__ELF__`, `__AVX2__`). Headers are FreeBSD's. Most libc
  comes from the console's `libSceLibcInternal`; the SDK adds a static
  `libc.a` of FreeBSD-derived supplements (`localtime`, `nl_langinfo`,
  `uname`, `getaddrinfo`, `mkstemp`, `popen`, `regex`, ...).
* Threads come from `libkernel` (`pthread_*`). The SDK's wrapper compiles
  with `-femulated-tls`, so `__thread`/`thread_local` work through
  `__emutls_get_address` (pthread keys). CPython 3.12's thread-local thread
  state will therefore compile; its cost must be measured.
* The SDK CRT contains a userland dynamic linker: `dlopen`/`dlsym` work for
  `.so` files and for system `.sprx` modules. SDL uses this to load Mesa.
* No `getentropy` stub was found; CPython must use its `/dev/urandom`
  fallback (the probe checks `/dev/urandom` on the console).
* Payload stdio is connected by elfldr to the TCP connection that sent the
  payload, so logs stream back to the developer machine.
* Host builds use any LLVM ≥ 15 with `lld`; the SDK's own CI uses LLVM 18 on
  Ubuntu and macOS. Apple's clang can compile (not link) for the target.

### Graphics — the largest risk

Ren'Py 8.5.3 renders through its GL2 model-based renderer (`gl2` desktop
OpenGL, or `gles2`). A software renderer (`sw`) still exists but is a
fallback that lacks model-based features. KSRE uses those features
heavily: `blur` (62 uses), `matrixcolor` (34), perspective camera (15),
and a camera-level colorblind transform (source). **A real GL context is
therefore required for a faithful port.** An SDL framebuffer demo does not
establish Ren'Py compatibility.

Available PS5 GL paths (source; none console-verified by this project):

1. **Hardware: Mesa 26.2 RADV-on-AGC + zink + EGL "videoout"** (pacbrew
   bundle v0.41.1, Oct 2026). SDL2 is patched to create GL/GLES contexts
   through Mesa's EGL and dlopen `libGL.so.1`/`libGLESv2.so.2`. The recipe
   itself warns that its patches are AI-generated and need expert review.
   GL windows report a 3840×2160 swapchain.
2. **Software fallback: Mesa 22.1.7 OSMesa (gallium swrast with LLVM)**
   (pacbrew bundle v0.40.3 and earlier). SDL renders into a CPU surface.
   An LLVM-JIT rasteriser needs executable memory; the probe tests whether
   payloads can create it. 1080p software rendering with blurs is expected
   to be slow (inference).
3. **Last resort: Ren'Py `sw` renderer** — would run without GL but would
   not render KSRE faithfully. Only acceptable as a diagnostic mode.

Decision: try path 1 first in a dedicated GL experiment that renders with
GLSL shaders comparable to Ren'Py's, measure it, and keep path 2 as the
fallback. Ren'Py's renderer is selected with `RENPY_RENDERER=gl2|gles2`.

**Update (milestone 02b, console evidence):** on firmware 7.61 path 1 does not
get past Mesa's device creation (`sceAgcInit(8)` fails with 0x8a6c002f, so
there is no EGL display and no GL context of any kind). Path 2 works: OSMesa /
llvmpipe gives a GL 4.5 compatibility context, the Ren'Py-style shaders and
draw patterns read back correctly, and 1080p frame times are 9.9 ms for a
plain scene up to 69 ms for a re-rendered blur. **Decision: path 2, renderer
`gl2` (OSMesa has no OpenGL ES), 1080p.** Two build/runtime rules come with
it: every payload that loads a Mesa library must be linked with `-rdynamic`
(the libraries import the SDK's `kernel_mprotect` weakly and the loader only
resolves it against exported executable symbols), and a GL payload should end
with `_exit()` rather than returning from `main`. See [GRAPHICS.md](GRAPHICS.md).

### Python runtime plan (inference, to be validated in milestone 3)

* CPython 3.12.8 cross-compiled with `--host=x86_64-unknown-freebsd`,
  `CC=prospero-clang`, `--disable-shared`, a `config.site` holding answers
  to run-time tests, and a static module list modelled on renpy-build's
  `Python-3.12.8-Setup.stdlib` (read from the pinned checkout, not copied).
* Host build Python 3.12 is required (`--with-build-python`); host tools
  stay arm64/x86-64 host binaries and never mix with target libraries.
* The probe's dynamic-symbol inventory and the CI link inventory feed the
  `config.site` answers (`fork`, `posix_spawn`, `getentropy`, ...).
* A custom `main()` follows renpy-build's launcher structure
  (`PyConfig`, module search paths, `Py_RunMain`) and sets
  `RENPY_PATH_TO_SAVES`, `RENPY_RENDERER` and the Python home.

**Update (milestone 03a, host + ci):** the static build works as planned, with
`--host=x86_64-unknown-freebsd` (two small configure patches; `sys.platform` is `freebsd`),
`MODULE_BUILDTYPE=static`, math from the console's `libSceLibcInternal` (`--without-libm`)
and zlib/bzip2/xz from the pinned pacbrew bundle. The pure-Python stdlib ships as
`/data/ksre-ps5/python/lib/python312.zip` with hash-based `.pyc` files. The launcher runs
the interpreter on a 16 MiB thread, in UTF-8 mode, and handles `SystemExit` itself instead
of using `Py_RunMain` (which would `exit()` past the payload's `_exit()`). Module choices:
[ports/cpython/README.md](../ports/cpython/README.md). Console start-up is milestone 03b.

### Media (source)

* KSRE video: Matroska, **VP9 profile 0, 1920×1080, 30 fps + Opus 48 kHz**.
  Music/sfx: **Ogg Vorbis 44.1 kHz**. Played through Ren'Py's own FFmpeg
  integration (`renpy.movie_cutscene`, `Movie` displayables).
* Ren'Py's `ffmedia.c` has guards for FFmpeg 7/8 APIs. The pacbrew FFmpeg
  recipe (7.0.1) proves FFmpeg cross-builds for the PS5 but enables GPL
  components (x264); the port builds FFmpeg itself with an LGPL decoder
  set modelled on renpy-build's (matroska/ogg demuxers; vp9/opus/vorbis
  decoders, x86 asm via nasm).
* The websrv release ships an FFplay build, i.e. FFmpeg + SDL playback
  already runs as PS5 homebrew (doc; not tested here).

### Input, audio, text

* SDL2's PS5 port provides joystick (ScePad), audio (SceAudioOut), keyboard
  and filesystem drivers (source). The PS5 pad is exposed with GUID
  `0300d0424c050000e60c000011810000`.
* **Controller mapping (console, milestone 02c):** SDL already treats this pad as a
  GameController, because the driver supplies the mapping itself; **no mapping string or
  `gamecontrollerdb.txt` entry is needed**. SDL reports
  `a:b0,b:b1,x:b2,y:b3,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,touchpad:b4`.
  There is no `back`, `guide`, `misc1` or paddle, and the PS and Create buttons are not
  delivered. Ren'Py's default `config.pad_bindings` therefore work as they are: A/R2 dismiss,
  B/Options menu, L1/L2 rollback, R1 rollforward, Y hide windows, X alternate, D-pad/sticks
  navigate; only the `back`/`guide` bindings are unreachable. Triggers arrive as
  GameController axes 0…32767 (rest 0), above Ren'Py's ±12288 threshold when pulled. A second
  GameController, `PS5 Remote Control` (no axes), is also listed and sends no events.
  Audio opens at a fixed 48 kHz (F32 stereo works); SDL converts other rates.
* Fonts render with FreeType + HarfBuzz (KSRE ships CJK fonts and
  OpenDyslexic for accessibility).

### Storage and process model

* Writable storage is expected under `/data` (the probe verifies write,
  fsync, rename, persistence across runs and free space).
* Ren'Py honours `RENPY_PATH_TO_SAVES` (source), so saves and persistent data
  go to e.g. `/data/ksre-ps5/saves/sh.fhs.ksre/` without patching Ren'Py.
* Development loading: send *non-graphical* payloads to elfldr on TCP 9021.
  **Correction (console evidence, milestone 2a):** a payload that draws to the
  TV must run as a foreground application; sent to elfldr it presents frames
  but nothing is shown. Use websrv's homebrew launcher (`/hbldr`, which runs
  the ELF as a fake big app); see `docs/CONSOLE_TESTING.md`.
* User-facing launching: websrv's homebrew convention
  `/data/homebrew/<Name>/eboot.elf` + `sce_sys/icon0.png` (doc).
* Installable application: the SDK's `install_app` sample shows an
  open-source route (fake-signed `eboot.bin` + `param.json`, installed with
  `sceAppInstUtilAppInstallTitleDir`). Whether a payload-style runtime works
  inside an installed app is unknown; investigated in milestone 7.

### KSRE integration (source + inference)

KSRE assumes desktop/mobile/web platforms. On the PS5 Ren'Py's platform
detection falls through to `renpy.linux = True`, which would:

* start KSRE's Discord Rich Presence thread (`pypresence`, `asyncio`,
  sockets) — harmless but useless; the overlay replaces it before
  `init 999` starts it;
* select Linux text-to-speech, which spawns a subprocess — must be
  disabled or redirected;
* show the desktop "Quit" button — acceptable (returns to the loader).

The overlay is a separate `.rpy` file shipped by this project; KSRE's own
files stay unmodified wherever possible.

## Open blockers and the smallest experiment for each

| # | Blocker | Smallest resolving experiment | Milestone |
|---|---|---|---|
| 1 | Payload runtime capabilities unknown (TLS, threads, storage, JIT, kqueue, memory) | `experiments/01-runtime-probe` on the console | 1b |
| 2 | Does a GL context with GLSL shaders work, and how fast? | SDL2 + EGL/zink window drawing textured quads with Ren'Py-like shaders at 1080p; then OSMesa fallback | 2b |
| 3 | Does the PS5 pad map to an SDL GameController? | SDL2 test payload printing controller events — **yes, native GameController (console 02c)** | 2c |
| 4 | Can CPython 3.12 build and start? | static CPython that prints `sys.version`, imports `asyncio`, `zlib`, `json`, writes a file | 3a–3b |
| 5 | Do Ren'Py's native modules build and import? | link `librenpy.a` + inittab, `import renpy` | 4a |
| 6 | Does Ren'Py run a game? | original tiny test game (dialogue, two choices, transform, audio, save/load, persistent, rollback) | 4b |
| 7 | Do KSRE's videos play smoothly? | software VP9 1080p30 decode timing on the console | 6b |

## Effort estimate

Assumptions: one developer with Claude Code sessions, console available for
tests within a day, the pacbrew bundles usable without rebuilding Mesa, and
no blocker that requires fixing Mesa's PS5 patches. Ranges are focused
working days.

| Milestone | Range | Main uncertainty |
|---|---|---|
| 1 Toolchain + probe on console | 0.5–2 | console access |
| 2 Display, GL, input, audio experiments | 3–10 | GL path (could grow to weeks if Mesa needs fixes) |
| 3 CPython + C dependencies | 5–15 | libc gaps, configure answers |
| 4 Ren'Py on the test game | 5–15 | renderer, static module build |
| 5 KSRE title screen + opening scene | 3–8 | memory, video, platform assumptions |
| 6 Controller UX, media, saves, text | 10–20 | VP9 performance, UI polish |
| 7 Packaging | 2–6 | installed-app runtime differences |
| 8 Regression + release candidate | 5–10 | breadth of KSRE content |

Total: roughly 35–85 focused days. If the hardware GL path fails and the
software path is too slow, the project is blocked on Mesa work that is out
of this repository's scope.
