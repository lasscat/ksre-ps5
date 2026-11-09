# CLAUDE.md — ksre-ps5 project conventions

## Start of every session

1. If `.local-work/HANDOFF.md` and `.local-work/PLAN.md` exist, read them
   first (they are ignored by git and hold the maintainer's local plan and the
   previous session's handoff). Follow the milestone prompt you were given;
   do not start a different milestone.
2. Inspect `git status`, the current branch and recent commits before
   changing anything. Preserve unrelated work.
3. Do not re-research decisions recorded in `docs/ARCHITECTURE.md` unless new
   evidence contradicts them; if it does, record the evidence and the change.

## What this project is

A native PS5 homebrew payload port of Katawa Shoujo: Re-Engineered v2.0.5
that preserves Ren'Py 8.5.3 and CPython 3.12.8. Read `docs/ARCHITECTURE.md`
for the design and `docs/STATUS.md` for verified capabilities.

## Hard rules

* Never commit or push anything under `.local-work/`, `.local-config/`,
  `external/`, `game-data/`, `build/`, `dist/`, logs, saves, crash dumps,
  console addresses, personal paths, credentials, or KSRE/Katawa Shoujo assets
  or narrative text. Stage explicit paths only (never `git add -A`/`.`).
* Run `python3 tools/publication_check.py --staged` before committing and
  `--outgoing` before pushing (`tools/install-hooks.sh` installs both).
* Never use leaked Sony SDKs, keys, firmware files or commercial game
  binaries. Only open-source homebrew tooling.
* Pin upstream revisions in `upstreams.lock`; do not build from moving
  branches.
* Keep host tools and PS5 target artifacts separate. PS5 code is built only
  with the SDK's `prospero-*` wrappers.
* Do not rewrite KSRE's story or write a replacement `.rpy` interpreter.
  Prefer an overlay file to patching KSRE; never patch narrative text.
* Do not recommend updating or downgrading console firmware.

## Evidence and status vocabulary

Keep these separate in docs, commits and handoffs: `host` (built/run on a
development machine), `compiles`, `links`, `loader` (elfldr accepted it),
`console` (ran on a real PS5 with captured output), `game` (full KSRE).
Never claim a console result without a captured console log. Label
statements as source / doc / host / ci / console / inference.

## Layout

* `upstreams.lock` — pins (parsed by `tools/lib/common.sh`, never sourced)
* `experiments/NN-name/` — one question per experiment, each with a README
  holding its results table
* `tools/` — `check-env.sh`, `fetch-sdk.sh`, `fetch-upstreams.sh`,
  `deploy.sh` + `ps5_send.py`, `publication_check.py`, `install-hooks.sh`
* `docs/` — public technical documentation
* `.local-config/console.env` — private console address (see
  `config/console.env.example`)

## Code style

* C: C11 (`-std=gnu11`), warnings on, SPDX header
  `GPL-3.0-or-later`. Code must also build for the host where practical so
  CI can test logic without a console.
* Shell: bash, `set -euo pipefail`, must work with macOS bash 3.2 and pass
  shellcheck. Python tools: stdlib only, Python ≥ 3.8.
* Patches to MPL-2.0 KSRE files are MPL-2.0; patches to Ren'Py should be
  written so they could be offered upstream under Ren'Py's licence.

## Commits

Small, reviewed commits with explicit paths. End commit messages with the
attribution trailer requested by the maintainer's environment.
