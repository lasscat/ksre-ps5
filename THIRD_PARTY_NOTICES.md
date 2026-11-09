# Third-party notices

This repository contains only original code, documentation and small
original test fixtures. It does **not** redistribute KSRE, Katawa Shoujo,
Ren'Py, CPython, SDL, Mesa, FFmpeg, the PS5 payload SDK, or any game asset.
Upstream sources are fetched at build time at the revisions pinned in
`upstreams.lock`. This file records the licences that govern what users
build and the obligations a binary distribution would carry. Verified at the
pinned revisions on 2026-11-08 unless marked otherwise.

## Game

**Katawa Shoujo: Re-Engineered** — Fleeting Heartbeat Studios,
<https://codeberg.org/fhs/katawa-shoujo-re-engineered> (tag `v2.0.5`).

* Root `LICENSE`: Mozilla Public License 2.0. The README states: "The code is
  licensed under the Mozilla Public License 2.0, while the assets are under
  by their original CC-BY-NC-ND license" (linking CC BY-NC-ND 3.0).
* Script files, including the story scripts (`game/script-*.rpy`), carry
  MPL-2.0 file headers, but they contain the original Katawa Shoujo
  narrative by Four Leaf Studios, which is licensed CC BY-NC-ND 3.0. This
  project therefore treats narrative text as CC BY-NC-ND content and never
  copies or modifies it.
* Translation files (`game/tl/*`) and the bundled mod (`game/mods/`) carry no
  licence header; their terms are unclear and they are not redistributed.
* Fonts shipped by KSRE (Gentium, OpenDyslexic, VL PGothic, Xiaolai SC,
  Playtime, Seto Font) come without licence files in the repository; their
  terms were not verified here and they are never redistributed.
* Bundled Python package `pypresence` 4.6.1 — MIT.
* Any patch this project makes to an MPL-2.0 KSRE file is itself MPL-2.0
  (MPL file-level copyleft). New files we add (e.g. a platform overlay) are
  our own work under GPL-3.0-or-later.

Katawa Shoujo is a work of Four Leaf Studios. Users must obtain KSRE's files
themselves (see `docs/GAME_DATA.md`); CC BY-NC-ND forbids commercial use and
distribution of modified assets.

## Engine

**Ren'Py** <https://github.com/renpy/renpy> (`8.5.3.26051504`) — MIT, with
LGPL-licensed components. Ren'Py's licence page asks that games be
distributed "in a manner that satisfies the LGPL" and suggests the notice
"This program contains free software licensed under a number of licenses,
including the GNU Lesser General Public License." Bundled libraries and
their licences are listed at <https://www.renpy.org/doc/html/license.html>.

**renpy-build** <https://github.com/renpy/renpy-build>
(`renpy-8.5.3.26051504`) — no licence file or licence statement was found at
the pinned revision. This project therefore does not copy any renpy-build
file; it reads renpy-build only from a user-fetched checkout and writes its
own equivalents.

**CPython** 3.12.8 — PSF License Version 2. Derivative works must retain the
PSF copyright notice and summarise changes.

## PS5 homebrew toolchain and libraries

| Project | Licence | Notes |
|---|---|---|
| ps5-payload-sdk <https://github.com/ps5-payload-dev/sdk> | GPL-3.0-or-later (FreeBSD headers BSD; some libc files BSD; `emutls.c` Apache-2.0 WITH LLVM-exception) | The CRT linked into every payload is GPLv3+ with no linking exception; any distributed payload must comply with GPLv3 |
| ps5-payload-dev SDL (SDL2 2.30 port) | Zlib | |
| pacbrew-repo recipes / bundles | GPL-3.0 (recipes); each package keeps its own licence | Mesa: MIT and others; FFmpeg: LGPL/GPL depending on configuration |
| ps5-payload-elfldr, websrv | GPL-3.0-or-later | run on the console; not linked |
| FFmpeg | LGPL-2.1-or-later as configured by this project (no GPL components) | |
| FreeType | FreeType Licence (BSD-style, GPLv3-compatible) or GPLv2 | |
| HarfBuzz | "Old MIT" | |
| FriBidi | LGPL-2.1-or-later | |
| libpng / zlib / bzip2 / xz | PNG / Zlib / bzip2 / 0BSD-public domain | |
| libjpeg-turbo | IJG, BSD-3-Clause, Zlib | binaries must state they are "based in part on the work of the Independent JPEG Group" |
| libwebp | BSD-3-Clause + patent grant | |

Live2D and Steam integrations of Ren'Py are proprietary and are not built.

## Sony

No Sony SDK, header, key, firmware file or system module is included or
required. System libraries are provided by the console at run time; the
open-source SDK supplies only stubs.
