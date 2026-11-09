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

## Later milestones (preview)

`tools/check-env.sh --milestone 3` lists the additional host tools: meson,
pkg-config, nasm (FFmpeg x86 assembly), autoconf/automake/libtool, and a host
Python 3.12 for CPython's `--with-build-python`. Cython 3.1.4 will be
installed into a project-local virtual environment, not globally.

## Publication safety

Run before staging or pushing (CI runs it too):

```sh
tools/install-hooks.sh                     # pre-commit + pre-push hooks
python3 tools/publication_check.py --staged
python3 tools/publication_check.py --outgoing
```
