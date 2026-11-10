# ports/ffmpeg — LGPL-only FFmpeg for the PS5 (milestone 03c)

`ports/ffmpeg/build.sh` cross-builds FFmpeg 7.1.5 (pinned in `upstreams.lock`) as static
libraries into `build/sysroot/` with the SDK's `prospero-clang`, `--target-os=freebsd`,
x86 assembly through `nasm` and runtime CPU detection. `ports/build-all-deps.sh` runs it and
stages the other Ren'Py C dependencies from the pinned pacbrew bundle (pacbrew's own FFmpeg
is GPL with x264/OpenSSL and is not used).

* Libraries: libavcodec, libavformat, libavutil, libswresample, libswscale (Ren'Py's
  `ffmedia.c` uses nothing else). No programs, avfilter, avdevice, network, autodetected
  system libraries or external codec libraries; zlib only (Matroska header compression).
* Demuxers: au avi flac m4v matroska mov mp3 mpegps mpegts mpegvideo ogg wav.
* Decoders (FFmpeg-native): vp8 vp9 theora vp3 opus vorbis flac mp2 mp3 mp3on4 mpeg1/2/4
  msmpeg4v1-3 pcm_*. No AV1 (renpy-build's libaom is not built).
* Why 7.1.5 and not renpy-build's 4.3.1: the two renpy-build patches (yuv2rgb unaligned
  stores, `ff_seek_frame_binary` uninitialised variables) are upstream fixes already in 7.1,
  and Ren'Py 8.5.3 `ffmedia.c` has code paths for libavformat 61 / libavutil 59.
* Licence gate: the script fails unless `config.h` has `CONFIG_GPL`, `CONFIG_VERSION3` and
  `CONFIG_NONFREE` all 0; `build/sysroot/ffmpeg-config.txt` records `LGPL version 2.1 or later`.
  Static linking into a GPL-3.0-or-later payload is fine; anyone distributing a binary must
  also offer the FFmpeg source and allow relinking (08b).

## Results

| Check | Result |
|---|---|
| configure + build + install (macOS arm64 host, LLVM 18) | compiles: no patches needed |
| `HAVE_X86ASM`, `HAVE_PTHREADS` in config.h | 1, 1 (assembly used: 300 `ff_vp9_*` SSSE3/AVX2 symbols) |
| `libavcodec.a` VP9 symbols | 1052 |
| `external/renpy/src/ffmedia.c` and `ftsupport.c` compile against `build/sysroot` | compiles (deprecation warnings only) |
| `ports/deps-link-check.c` links against every dependency into an ELF | links |
| Decode benchmark (synthetic VP9 1080p30) on the console | not run (optional; left for 06b) |

Open risk: FFmpeg decoder threads are plain pthreads; threads on the console start with
MXCSR FTZ/DAZ set (03b). Not yet observed to matter; clear MXCSR in a thread-start hook if a
benchmark shows float problems.

## Other dependencies (staged from pacbrew v0.40.3 by `build-all-deps.sh`)

Versions and licences are written to `build/sysroot/VERSIONS.txt`: zlib 1.3.2, bzip2 1.0.8,
xz 5.4.6, libpng 1.6.43, libjpeg-turbo 3.0.2, libwebp 1.4.0, FreeType 2.13.2 (built without
HarfBuzz; Ren'Py's `ftsupport.c` uses FreeType and HarfBuzz separately), HarfBuzz 9.0.0,
FriBidi 1.0.15, SDL2_image 2.8.2 (PNG/JPEG/WebP; no AVIF). Not built: assimp (3D model
import; KSRE does not use it, 04a stubs the module), Live2D (proprietary, excluded), libavif
and libyuv (AVIF only; KSRE ships none).
