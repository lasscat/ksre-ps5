# Building

## Supported development hosts

| Host | Status | Evidence |
|---|---|---|
| macOS on Apple Silicon | Supported | ps5-payload-sdk documents Homebrew LLVM and runs CI on `macos-latest`; the probe builds and runs natively here |
| Debian/Ubuntu x86-64 or arm64 | Supported | SDK README and CI (`ubuntu-latest`, clang-18/lld-18); this repository's CI |
| Windows | Use WSL2 with Ubuntu (untested) | — |

Host tools (compilers, Python, Cython) are native host binaries — arm64 on
Apple Silicon. Everything that runs on the PS5 is `x86_64-sie-ps5` and is
built only through the SDK's `prospero-*` wrappers, into `build/` or the SDK's
`target/` sysroot. Never mix the two: no Homebrew or system library is ever
linked into a payload.

A Linux VM/container is **not** needed for milestones 1–4 as planned: the SDK
binary release and the pacbrew dependency bundles are architecture-
independent archives. A Linux container (Ubuntu 24.04) becomes useful only if
a pacbrew package (for example Mesa) must be rebuilt, because pacbrew's
recipes use `makepkg`/`pacman` and assume a Debian host.

## Milestone 1: toolchain and runtime probe

macOS:

```sh
brew install llvm@18                       # includes ld.lld; same LLVM major as the SDK's CI
export LLVM_CONFIG="$(brew --prefix llvm@18)/bin/llvm-config"
```

Debian/Ubuntu:

```sh
sudo apt-get install clang-18 lld-18 llvm-18 make unzip curl python3
```

Newer LLVM releases (up to 23) are accepted by the SDK wrappers but are not
what upstream CI tests.

Then, on either host:

```sh
tools/check-env.sh                         # verifies the above
tools/fetch-sdk.sh                         # downloads + verifies SDK v0.45 into external/toolchains/
export PS5_PAYLOAD_SDK="$PWD/external/toolchains/ps5-payload-sdk-v0.45/ps5-payload-sdk"

make -C experiments/01-runtime-probe run-host        # host logic test
make -C experiments/01-runtime-probe ps5             # -> build/experiments/01-runtime-probe/ps5/ksre-ps5-probe.elf
make -C experiments/01-runtime-probe link-inventory  # link tests for CPython's configure
```

Without LLVM installed you can still run a compile-only check for the PS5
target on macOS (Apple clang knows the target; it cannot link):

```sh
make -C experiments/01-runtime-probe ps5-compile-check
```

CI builds the probe payload on every push and keeps it as the artifact
`ksre-ps5-probe-elf` for 14 days:

```sh
gh run download --repo lasscat/ksre-ps5 --name ksre-ps5-probe-elf --dir build/ci-probe
```

## Milestone 3a: CPython

```sh
source .local-config/env.sh              # or export PS5_PAYLOAD_SDK as above
tools/fetch-pacbrew.sh swgl              # zlib, bzip2, xz for the PS5 build
ports/cpython/build.sh host              # CPython 3.12.8 for this machine -> build/host-python
ports/cpython/build.sh ps5               # static PS5 build -> build/cpython/ps5
make -C experiments/03-python-hello ps5 run-host
```

The host build needs only a C compiler and make (on Linux also `zlib1g-dev`); it is the
`--with-build-python` for the cross build. Details: `ports/cpython/README.md`.

## Ren'Py C dependencies

`ports/build-all-deps.sh` (needs nasm, the SDK and `tools/fetch-pacbrew.sh swgl`) fills
`build/sysroot`; see `ports/ffmpeg/README.md`.

## Ren'Py payload (milestone 4a)

```sh
tools/fetch-upstreams.sh renpy pacbrew
ports/renpy/build.sh            # -> build/ports/renpy/ksre-ps5-renpy.elf, runtime/, testgame/
```

Needs the CPython and sysroot builds above and a host `python3` with `venv`/`ensurepip` and
network access: Cython 3.1.4 is installed into `build/ports/renpy/venv`, not globally.
`ports/renpy/build.sh host` and `selftest-host` build and run the same code on the
development machine. Details: `ports/renpy/README.md`.

## Host tools

`tools/check-env.sh --milestone 3` lists the additional host tools: meson,
pkg-config, nasm (FFmpeg x86 assembly), autoconf/automake/libtool (for the C
libraries of 3c; CPython itself needs none of them).

## Publication safety

Run before staging or pushing (CI runs it too):

```sh
tools/install-hooks.sh                     # pre-commit + pre-push hooks
python3 tools/publication_check.py --staged
python3 tools/publication_check.py --outgoing
```
