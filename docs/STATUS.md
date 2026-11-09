# Status

Last updated: 2026-11-08. Statuses are deliberately separate: compiling is
not loading, loading is not running, running a test is not playing the game.

| Status | Meaning |
|---|---|
| `host` | Built and run on a development machine (not PS5 evidence) |
| `compiles` | Compiles for `x86_64-sie-ps5` (object files only) |
| `links` | Links into a PS5 payload ELF |
| `loader` | ps5-payload-elfldr accepted and started the payload |
| `console` | Ran to completion on a real PS5 with the expected output |
| `game` | Works across the full KSRE game |
| — | not attempted |

## Components

| Component | host | compiles | links | loader | console | game |
|---|---|---|---|---|---|---|
| Runtime probe (`experiments/01-runtime-probe`) | yes (macOS arm64) | yes (Apple clang, SDK v0.45 headers) | CI | — | — | n/a |
| Link inventory for CPython configure | n/a | n/a | CI | n/a | n/a | n/a |
| SDL2 display / GL / input / audio experiments | — | — | — | — | — | — |
| CPython 3.12.8 | — | — | — | — | — | — |
| Ren'Py 8.5.3 native modules | — | — | — | — | — | — |
| Ren'Py original test game | — | — | — | — | — | — |
| KSRE v2.0.5 | — | — | — | — | — | — |
| Launcher / packaging | — | — | — | — | — | — |

"CI" means the GitHub Actions job `ps5-cross-build` performs the step; check
the latest run for its result.

## Known blockers

See the blocker table in [ARCHITECTURE.md](ARCHITECTURE.md#open-blockers-and-the-smallest-experiment-for-each).
