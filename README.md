# ksre-ps5

**Experimental, pre-alpha.** An in-progress homebrew port of
[Katawa Shoujo: Re-Engineered](https://codeberg.org/fhs/katawa-shoujo-re-engineered)
(KSRE) to the PlayStation 5, as a native PS5 payload that keeps the real
engine — Ren'Py 8.5.3 on CPython 3.12 — instead of rewriting the game.

> **Nothing runs on a PS5 yet.** The repository currently contains research,
> the build/publication tooling, and a first bring-up experiment (a runtime
> capability probe). See [docs/STATUS.md](docs/STATUS.md) for exactly what has
> and has not been verified.

This project is not affiliated with Sony Interactive Entertainment, Fleeting
Heartbeat Studios (KSRE), Four Leaf Studios (Katawa Shoujo), or the Ren'Py
project. It contains no game assets, no Sony SDK material, no keys and no
commercial game code.

## Approach in one paragraph

KSRE v2.0.5 targets Ren'Py 8.5.3. The port cross-compiles CPython 3.12.8 and
Ren'Py's ~70 native modules for the `x86_64-sie-ps5` target with the
open-source [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), links
them statically into one payload the same way Ren'Py's own build system does
for its platforms, and uses the PS5 ports of SDL2 and Mesa for display,
input, audio and OpenGL. The unmodified KSRE game directory is supplied by
the user. Details, evidence and alternatives (PS4 backward compatibility,
Linux on PS5, browser) are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Repository layout

| Path | Contents |
|---|---|
| `upstreams.lock` | Pinned upstream URLs, tags, commits and SHA-256 sums |
| `experiments/` | Small executable experiments that settle one question each |
| `ports/` | Build scripts, configure answers and patches for upstream components (CPython, FFmpeg, Ren'Py) and the Ren'Py launcher |
| `fixtures/` | Original test content (a tiny Ren'Py game with generated assets) |
| `tools/` | Environment check, SDK/upstream fetchers, deploy helper, publication check |
| `config/` | Sanitised configuration examples (real config stays in `.local-config/`) |
| `docs/` | Architecture, building, console testing, game data, status |

Ignored, never published: `.local-work/`, `.local-config/`, `external/`,
`game-data/`, `build/`, `dist/`.

## Quick start (developer machine)

```sh
tools/check-env.sh                 # what is installed / missing, with install commands
tools/fetch-sdk.sh                 # pinned ps5-payload-sdk into external/ (prints an export line)
export PS5_PAYLOAD_SDK=...         # as printed
make -C experiments/01-runtime-probe run-host      # logic test on this machine
make -C experiments/01-runtime-probe ps5           # PS5 payload (needs LLVM + lld)
```

Full instructions: [docs/BUILDING.md](docs/BUILDING.md). Console testing:
[docs/CONSOLE_TESTING.md](docs/CONSOLE_TESTING.md). Supplying KSRE's files:
[docs/GAME_DATA.md](docs/GAME_DATA.md).

## Licensing

* Code in this repository: **GPL-3.0-or-later** ([LICENSE](LICENSE)). Every PS5
  payload links the ps5-payload-sdk runtime, which is GPLv3+ without a
  linking exception, so distributed binaries are GPLv3 works regardless; the
  repository uses the same licence for coherence.
* KSRE's code is MPL-2.0 and its original assets are CC BY-NC-ND 3.0; Ren'Py
  is MIT with LGPL components. None of them are redistributed here. See
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
