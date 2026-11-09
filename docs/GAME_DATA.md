# Supplying KSRE's game files

This repository never contains KSRE's scripts, images, audio, video, fonts
or translations. Users supply them locally, into the ignored `game-data/`
directory, from KSRE's official sources. The original assets are licensed
CC BY-NC-ND 3.0: do not redistribute modified assets and do not use them
commercially.

## Which version

The port targets **KSRE v2.0.5** (commit
`95b905bab8a9e8c51d02afd9ee8bc0e3762397ca`), which is built for Ren'Py 8.5.3.
Other KSRE versions may require a different Ren'Py version and are not
supported.

## Option 1 — tagged source checkout (development)

About 1.6 GB of blobs (video ~540 MB, images ~810 MB, audio ~80 MB):

```sh
git clone --depth 1 --branch v2.0.5 \
  https://codeberg.org/fhs/katawa-shoujo-re-engineered.git game-data/ksre-v2.0.5
git -C game-data/ksre-v2.0.5 rev-parse HEAD   # expect 95b905ba...
```

The GitHub mirror `https://github.com/fleetingheart/ksre.git` carries the same
tag. A source checkout contains `.rpy` files that Ren'Py compiles to `.rpyc`
on first start; whether compilation happens on the console or ahead of time
on the host with the official Ren'Py 8.5.3 SDK is decided in milestone 5.

## Option 2 — official release build

Download the official Linux release of v2.0.5 from the project page
<https://www.fhs.sh/projects/ksre> and extract its `game/` directory into
`game-data/ksre-v2.0.5-release/game/`. (At research time the Codeberg
release entries had no attached files, and the project page could not be
fetched non-interactively, so the exact download URL is not recorded here.)
Release builds are expected to contain compiled scripts for Ren'Py 8.5.3;
this is checked in milestone 5.

## Where it goes on the console

Decided in milestone 5 (candidates: next to the launcher entry under
`/data/homebrew/KSRE/game/`, or on a USB drive). Transfer instructions will be
added then.
