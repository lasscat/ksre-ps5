# Graphics: OpenGL for Ren'Py on the PS5

Milestone 02b. Evidence labels: **source** (read in code), **host**, **ci**,
**console** (captured log), **inference**. Statuses follow
[STATUS.md](STATUS.md) and are kept separate.

## Summary

| Path | Status | Evidence |
|---|---|---|
| Software GL: pacbrew bundle v0.40.3 (Mesa 22.1.7 OSMesa / llvmpipe, LLVM 14) | **console: works.** Desktop GL 4.5 compatibility context, all 7 read-back probes pass (texture + premultiplied blend, FBO copy + matrixcolor, mipmapped blur, orientation), 0 GL errors. 1080p: 60 fps for a typical scene, 12–30 fps for the heavy effects (table below) | console logs (private), 300 frames per test |
| Hardware GL: pacbrew bundle v0.41.1 (Mesa 26.2: zink on RADV-on-AGC, EGL "videoout") | **console: fails on firmware 7.61** — no Vulkan device, so no EGL display and no GL context for any API or request. Same result with and without the `-rdynamic` fix below | console logs: `sceAgcInit(8) failed: 0x8a6c002f` |
| Ren'Py's own `sw` renderer | not needed so far | — |

The 02b payload (shaders, draw path, read-back checks) also passes on a host
GPU, so a failure on the console is attributable to the console's GL stack and
not to the test.

## Decision

| Question | Decision | Basis |
|---|---|---|
| HW or SW path | **SW: OSMesa / llvmpipe (bundle v0.40.3)** | works on the console (below); the HW path cannot create a device on fw 7.61 |
| `gl2` or `gles2` | **`gl2`** (`RENPY_RENDERER=gl2`) | OSMesa offers desktop GL only; the context it gives is GL 4.5 compatibility (SDL's requested 2.0 is ignored), which covers Ren'Py's `gl2` needs. The `gles2` dialect is untested on any console path |
| Target resolution | **1080p** | the OSMesa surface is the 1920×1080 VideoOut framebuffer (console: the only display mode is 1920×1080@60) |
| Link flags for every payload that loads Mesa | **`-rdynamic`** | see "The `kernel_mprotect` import" |
| Exit path | **flush logs, then `_exit()`** | returning from `main` crashed after SDL unloaded the GL library (below) |

Frame times are measured, not assumed; the viability judgement for KSRE is
at the end of "Software bundle results".

## What Ren'Py's renderer needs (source: Ren'Py 8.5.3.26051504)

* `gl2`: desktop OpenGL **2.0 compatibility** profile; `gles2`: OpenGL ES
  **3.0**. Both request RGBA 8/8/8/8 and a **24-bit depth buffer**
  (`gl2draw.pyx select_gl_attributes`, `config.depth_size = 24`).
* Shaders are assembled from parts and are `#version 120` (gl2) or
  `#version 100` + precision header (gles2), including in an ES 3.0 context
  (`gl2shadercache.py source()`, parts in `common/_shaders.rpym`).
* 58 required entry points (`gl2functions.py`), resolved at run time.
* Draws from **client-side** vertex arrays with **client-side `GL_UNSIGNED_INT`
  indices** (`gl2shader.pyx`), premultiplied alpha blending, no VBOs.
* Render-to-texture draws into a renderbuffer FBO of up to 4096×4096 RGBA8 +
  depth and copies with **`glCopyTexImage2D`**, then `glGenerateMipmap`
  (`gl2texture.pyx`, `gl2draw.pyx init_fbo`).
* The **blur** shader samples with a mip **LOD bias** (`texture2D(tex, uv,
  bias)`), so every blurred layer needs a mip chain.
* KSRE's hot features through these paths: blur (62 uses), matrixcolor (34),
  perspective camera (15; this is what needs the depth buffer).

## Software bundle results (console, v0.40.3)

Setup: homebrew launcher, `libOSMesa.so.8` (92 MB) uploaded next to
`eboot.elf`, payload linked with `-rdynamic`, default llvmpipe (JIT), no
environment overrides. Context: `llvmpipe (LLVM 14.0.6, 256 bits)`, GL
`4.5 (Compatibility Profile) Mesa 22.1.7`, GLSL 4.50, 295 extensions, max
texture 16384, max renderbuffer 16384, 62/62 GL functions resolved, all four
Ren'Py shader programs (texture, alpha, matrixcolor, blur) compile and link,
the 4096×4096 RGBA8 + depth renderbuffer FBO is complete.

Benchmark, 300 frames per test at 1920×1080, vsync on. "Render" is wall time
from the first draw call to `glFinish()`; the frame period is quantised to
multiples of 16.7 ms because the swap waits for the 60 Hz VideoOut flip.

| Test | Render avg (p95) | Frame period | Rate |
|---|---|---|---|
| `scene_window`: 1080p background + two sprites + text box, alpha blended, straight to the window | 9.9 ms (10.1) | 16.7 ms | **59.9 fps** |
| `fill8_window`: eight full-screen alpha layers | 36.5 ms (37.0) | 50.1 ms | 20.0 fps |
| `fbo_matrixcolor`: scene → FBO → `glCopyTexImage2D` → `matrixcolor` pass | 26.7 ms (27.2) | 33.4 ms | 30.0 fps |
| `blur_cached`: blur of an already mipmapped texture | 49.0 ms (49.7) | 53.3 ms | 18.8 fps |
| `blur_rerender`: re-render + copy + mipmap + blur every frame (worst case) | 69.4 ms (70.6) | 83.4 ms | 12.0 fps |

Worst case split (60 frames, `glFinish` after each stage): render scene +
`glCopyTexImage2D` 15.2 ms, `glGenerateMipmap` 5.5 ms, blur draw 48.0 ms. The
blur draw dominates; the mip-bias blur samples several levels per pixel.

Reading: an ordinary dialogue screen (one background, sprites, text box)
renders in 9.9 ms of the 16.7 ms frame and holds 60 fps. Anything that draws a lot of
full-screen layers, applies matrixcolor to a full-screen layer, or blurs one
costs 25–50 ms per such layer per frame, so full-screen blurs and long
cross-dissolves over several layers will run at 12–30 fps unless they are
cached or made cheaper. These are synthetic scenes of my own, not KSRE frames,
and llvmpipe's rasteriser threads probably use every core the payload may use
(inference: its default is one thread per core), which a game that also
decodes video and runs Python will not have to itself. **Whether KSRE is
playable on this path is not established**; it is established that the path
is functional and not obviously too slow for the common case. Measure with
real KSRE frames in milestone 04b/06b.

### The `kernel_mprotect` import (root cause of the first crashes)

The first SW runs crashed with SIGSEGV in the first draw-like call
(`glGenerateMipmap`, later the first draw), with or without the JIT
(`GALLIUM_DRIVER=softpipe DRAW_USE_LLVM=0` crashed the same way).

Cause (console + source + host disassembly): Mesa's translate/rtasm code
generator allocates executable memory with `mmap(PROT_EXEC)`. In the pacbrew
build `mmap` is the SDK's own (`libc/mman.c`), linked into `libOSMesa.so.8`.
On this console an executable mapping is made in two steps: map without
`PROT_EXEC`, then call the SDK's **`kernel_mprotect(-1, addr, size, prot)`**.
Shared objects do not link `crt1.o`, so the library declares
`kernel_mprotect` as a **weak undefined import** and relies on the SDK's
run-time linker to resolve it against the *executable's exported symbols*.
A payload linked without `-rdynamic` defines the function (it comes from
`crt1.o`) but does not export it, the weak import stays NULL, and the library
calls address 0 (`rip=0`, the return address was inside the library's `mmap`).

Fix: link the payload with `-rdynamic` (the SDK's `hello_dlfcn` sample does
too). **Every payload that loads a Mesa/OSMesa library, or any shared object
that maps executable memory, needs it.** The HW bundle's `libgallium` and
`libvulkan` import the same symbol; the HW retest below was done with the fix.

The crash handler in `main.c` also taught us that the PS5 signal frame has
extra words before `mc_rip`; the header's field offsets are wrong (user cs
0x43 is word 27, rip is word 26, rsp word 29), so the handler decodes the raw
words and dumps the stack.

### Exit

With `-rdynamic` fixed, the payload printed its end marker and then crashed
with SIGSEGV when it returned from `main`: a thread-exit callback in the call
chain jumped into the GL library's address range, which `SDL_Quit` had already
unloaded (fault address equal to the instruction pointer). `_exit()` after
flushing the logs avoids it (default for the PS5 build; `--normal-exit` brings
the old behaviour back). The earlier HW run's SIGSEGV at exit, followed by the
loader services disappearing, is probably the same mechanism; with `_exit()`
the services stayed up after the HW retest. Causality for the service
disappearance is **not** established. What the console shows on screen after
`_exit()` was not reported.

### Other facts from the SW runs

* `SDL_GL_GetProcAddress` returns addresses `0x29f4000` above `dlsym`'s for the
  same symbol (the library is mapped twice, an executable alias and a data
  alias); `dladdr` does not know the alias, so offline analysis must subtract
  the delta (logged as `gl.alias`).
* Environment variables from the launcher's `env=` reach Mesa's own libc:
  `GALLIUM_DRIVER=softpipe` switched the renderer to `softpipe`.
* Mesa expects an 8 MiB stack. The payload runs on a 64 MiB thread (PS5
  pthreads default to 64 KiB); this was not the cause of the crashes but is
  kept as a safe default (`--stack-mb`).
* `swap_interval` reports 0 and the swap blocks for the flip regardless
  (vsync 0 and 1 gave identical frame periods in the 60-frame runs).

## Hardware bundle v0.41.1 (console)

Logs: private. Libraries uploaded next to `eboot.elf`; the SDK run-time linker
finds them there and `dlopen("libEGL.so.1")` succeeds.

* Loading the chain prints (from the system's `libSceAgcDriver`/`libSceAgc`):
  `[AgcDriver] Info - Initialized, submit.mode= 1.` and `[Agc] FS Table offset
  has shifted. This is not survivable.`
* Creating a context then fails for **every** request (desktop GL 2.0 compat,
  SDL default, 3.3 core; ES 3.0, ES 2.0, ES 3.2; fullscreen and windowed):
  `radv/ps5: sceAgcInit(8) failed: 0x8a6c002f`, `radv/ps5: the platform did
  not initialise; no device`, `ZINK: vkEnumerateInstanceLayerProperties
  failed`, `ZINK: failed to choose pdev`, `egl: failed to create dri2 screen`,
  SDL `Could not initialize EGL`. The TV stayed black.
* Run twice: once without `-rdynamic` and once with it. Identical failure, so
  the unresolved `kernel_mprotect` import is not the cause.

**What this does and does not show.** On this console (firmware 7.61) the
pinned Mesa build cannot create its Vulkan device, hence no GL at all. It says
nothing about zink's GL/GLES versions, speed or shader support. The `[Agc]`
message is printed by Sony's `libSceAgc`, not by Mesa. **Inference
(unverified):** the Mesa patches were developed against a different firmware
and `libSceAgc` on 7.61 does not match what they assume
(`PS5_AGC_INIT_VERSION` is 8). The recipe and release notes do not state a
supported firmware. This project does not recommend changing console
firmware.

## Known limitations

* HW GL context creation fails on firmware 7.61 (above); 4K output and a
  possible faster GPU path are therefore unavailable for now.
* The SW path is CPU-bound and shares the CPU with everything else Ren'Py
  does; see the numbers above for the cost per full-screen layer.
* OSMesa has no OpenGL ES, so `gles2` cannot be used on this path.
* The context SDL's OSMesa driver returns ignores the requested version and
  profile (always GL 4.5 compatibility here); Ren'Py's `gl2` should be fine
  with that but has not been run yet.
* The output surface is a CPU copy into the VideoOut framebuffer on every swap
  (about 7–14 ms of the frame in the tables above).
* Exit must go through `_exit()` (above); the screen after exit is unverified.
* websrv and ftpsrv are unauthenticated: LAN-only, development use.

## Proposed upstream issues (not filed; the maintainer decides)

**1. ps5-payload-dev/pacbrew-repo — HW GL on firmware 7.61.**

> **Title:** mesa 26.2 (v0.41.1): `sceAgcInit(8)` fails with 0x8a6c002f on firmware 7.61, no Vulkan/GL device
>
> **Setup:** pacbrew bundle `v0.41.1` (Mesa 26.2.0 with the 28 PS5 patches, SDL2 with
> patch 0001), ps5-payload-sdk v0.45, ps5-payload-websrv homebrew launcher, console
> firmware 7.61. Payload started with `GET /hbldr?path=...&pipe=1` (foreground app,
> working directory next to `libEGL.so.1`, `libGL.so.1`, `libGLESv2.so.2`,
> `libgallium-26.2.0.so`, `libvulkan.so.1`), linked with `-rdynamic`.
>
> **What happens:** `dlopen("libEGL.so.1", RTLD_NOW|RTLD_GLOBAL)` succeeds. The loader
> chain prints `[AgcDriver] Info - Initialized, submit.mode= 1.` and `[Agc] FS Table
> offset has shifted. This is not survivable.` The first `SDL_CreateWindow(SDL_WINDOW_OPENGL)`
> then logs `radv/ps5: sceAgcInit(8) failed: 0x8a6c002f`, `radv/ps5: the platform did
> not initialise; no device`, `ZINK: vkEnumerateInstanceLayerProperties failed`, `ZINK:
> failed to choose pdev`, `egl: failed to create dri2 screen`, SDL `Could not initialize
> EGL`. Same result for desktop GL 2.0 compat / 3.3 core and ES 2.0 / 3.0 / 3.2.
>
> **Question:** which firmware range are the AGC patches (init version 8) written for,
> and is 7.61 expected to work? Is there a prerequisite call the payload should make?
> Happy to test patches or logging.
>
> Reproducer and logs: `experiments/02b-gl-shaders` in the ksre-ps5 repository.

**2. Documentation note for pacbrew/the SDK — executables must be linked with `-rdynamic`.**

> Libraries from the Mesa bundles (e.g. `libOSMesa.so.8`) contain the SDK's `mmap`,
> which calls the weak import `kernel_mprotect`. An executable linked without
> `-rdynamic` does not export it, the import resolves to NULL, and the first
> executable-memory allocation (Mesa's rtasm / LLVM JIT) crashes with `rip=0`. A line
> in the recipe notes or the SDK's `mman.c` comment ("declare deps as weak") pointing
> out `-rdynamic`, or a NULL check that returns `EPERM`, would save the next user an
> afternoon.

## Reproducing

See [experiments/02b-gl-shaders/README.md](../experiments/02b-gl-shaders/README.md).
