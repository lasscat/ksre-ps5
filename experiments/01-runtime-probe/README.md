# Experiment 01 — runtime capability probe

**Question:** does the PS5 payload runtime provide what CPython 3.12 and
Ren'Py 8.5.3 need, and which GL fallback is possible?

One payload checks, with evidence from the real runtime:

| Area | Checks | Why it matters |
|---|---|---|
| platform | uname, sysctl, rlimits, firmware version and model (via run-time `dlsym`), presence of `/data`, `/user/homebrew`, USB mounts | compatibility records; storage layout |
| time | monotonic sleep accuracy, realtime clock, local time, tz | CPython `time`, Ren'Py frame pacing |
| signals | `sigaction`+`raise`, ignoring SIGPIPE | CPython start-up, `signal` module |
| threads | 4 threads with atomics/mutex, `__thread` (emulated TLS), pthread keys, monotonic `pthread_cond_timedwait`, 16 MiB thread stack | CPython thread state, GIL, Ren'Py worker threads |
| locale | `setlocale`, `nl_langinfo(CODESET)`, UTF-8 `mbstowcs` | CPython filesystem encoding / UTF-8 mode |
| ipc/net | pipe, poll, select, kqueue, AF_UNIX socketpair, loopback TCP, getaddrinfo | `asyncio` (imported by KSRE), `selectors` |
| entropy | `/dev/urandom`, `arc4random_buf`, `getentropy` | `os.urandom`, hash randomisation |
| float | `%.17g`, `strtod` subnormals, libm | CPython float repr/parsing |
| dl | `dlsym(RTLD_DEFAULT)`, missing-library error, loading `libSceRandom.sprx`, run-time resolvability of ~60 POSIX names | SDL loads Mesa with `dlopen`; CPython `posixmodule` |
| storage | mkdir, 1 MiB write+fsync+read, rename/unlink, mkstemp, readdir, free space, run counter | saves, persistent data, `.rpyc` cache |
| jit | W^X (RW→RX) and RWX executable memory | viability of an LLVM-JIT software rasteriser (llvmpipe) |
| memory | allocate and touch up to 2 GiB | Ren'Py image cache, video buffers |

Risky checks run under a SIGSEGV/SIGBUS/SIGILL guard. The memory check runs
last. The probe writes only inside `/data/ksre-ps5/` and prints no serial
numbers, account data or network addresses.

The same source builds for Linux and macOS so CI can test the probe logic.
**Host results are not PS5 evidence.**

## Build and run

See `docs/BUILDING.md` and `docs/CONSOLE_TESTING.md`. In short:

```sh
make run-host            # host logic test
make ps5                 # payload (needs PS5_PAYLOAD_SDK and LLVM + lld)
make ps5-compile-check   # compile-only for the PS5 target with Apple clang
make link-inventory      # link tests used to prepare CPython's configure
make deploy              # send to the console configured in .local-config/console.env
```

## Results

| Date | Where | Result |
|---|---|---|
| 2026-11-08 | host: macOS 27.2 arm64 | 28 pass, 0 fail, 1 skip (logic test only) |
| 2026-11-08 | compile-only for `x86_64-sie-ps5` (Apple clang 21, SDK v0.45 headers) | object file produced; not linked |
| 2026-11-09 | CI ubuntu-24.04 and macos-15 (host) | 28 pass, 0 fail (logic test only) |
| 2026-11-09 | CI ps5-cross-build (LLVM 18, SDK v0.45), commit d2e7aae | links: ELF64 x86-64 PIE, NEEDED `libkernel_web.sprx`, `libSceLibcInternal.sprx`, `libSceNet.sprx`; sha256 `47af42e1a688284e2fb5e286c0223c664359a5f7370eeabeaefceff61488a3dd` |
| 2026-11-10 | **PS5 console** (firmware 7.61; elfldr on 9021; CI ELF `47af42e1…`) | **ran to `KSRE_PS5_PROBE_END` twice: 29 pass, 1 fail, 0 warn, 0 skip, 49 info** (loader `yes`, console `yes`) |

### Console results (firmware 7.61, 2026-11-10)

The probe loaded and ran to completion under ps5-payload-elfldr, which streamed
its stdout back over the loader socket. Two runs, reproducible; the second was
after a console reboot.

* **Pass (29):** time (monotonic sleep, realtime), signals (raise, SIGPIPE),
  threads (atomics/mutex, `__thread` emulated-TLS isolation, pthread keys,
  monotonic `pthread_cond_timedwait`, 16 MiB stack), locale UTF-8 `mbstowcs`,
  ipc/net (pipe, poll, select, **kqueue**, AF_UNIX socketpair, loopback TCP),
  entropy (`/dev/urandom`, `arc4random_buf`), dynamic loading (`dlsym`,
  graceful missing-library, `dlopen` of a system `.sprx` + call), storage
  (mkdir, 1 MiB write+fsync+read, rename/unlink, mkstemp, readdir), **JIT
  `jit.wx` and `jit.rwx`** (W^X and RWX executable memory), and
  **`memory.headroom` — the full 2 GiB cap allocated and touched**.
* **Fail (1):** `float.ieee754` — the FPU runs in flush-to-zero mode, so the
  smallest subnormal (`strtod("4.9e-324")`) reads back as `0`. `printf`,
  `strtod` of normal values, `fma`, `DBL_MIN` and the libm spot-checks are all
  correct. Tracked as a deviation to handle before the CPython milestone
  (clear MXCSR FTZ/DAZ at start-up); it is not a loader or stability problem.
* **Storage:** `/data` is writable with ample free space, and the probe's run
  counter survived both payload exit and a reboot (persistent `/data`).
* **GL consequence:** because JIT works, a software OpenGL rasteriser
  (Mesa llvmpipe) is a viable fallback if the hardware GL path is not usable.

The platform/firmware/model INFO lines and the free-space figure are kept in
the maintainer's private notes, not here.

Status after this milestone: `loader` **yes**, `console` **yes** (see
[docs/STATUS.md](../../docs/STATUS.md)).

### Link inventory (CI, 2026-11-09)

152 of 173 functions link. Not linkable: `nice copy_file_range dup3 mremap
memfd_create pthread_getattr_np sem_clockwait sched_setaffinity
getgrouplist ttyname openpty forkpty ctermid accept4 if_nameindex
epoll_create1 iconv_open getentropy getrandom uname confstr`.

The test is the same as autoconf's `AC_CHECK_FUNCS` (declare a dummy
prototype, take the address, link), so it predicts what CPython's
`configure` will conclude. One known false negative: `uname` is a
`static inline` wrapper around `__xuname` in the SDK's `sys/utsname.h`, so
it works when the header is included (the probe itself calls it) but needs
`ac_cv_func_uname=yes` in CPython's `config.site`. Several others
(`confstr`, `accept4`, `dup3`, `ttyname`, `nice`) are declared in the SDK
headers but have no linkable symbol; configure will correctly treat them as
absent. Whether linkable names also resolve at run time is checked by the
probe's `dl.symbols` inventory on the console.

The console run (2026-11-10) showed that `dl.symbols` — which uses
`dlsym(RTLD_DEFAULT)` — only sees symbols exported by loaded shared modules
(`.sprx`), not symbols pulled from the static `libc.a`. Several functions
report `=n` there while being perfectly callable when linked statically
(`getaddrinfo`, `nl_langinfo`, `arc4random_buf`, `uname` were all exercised
successfully in the same run). So the **link** inventory, not the dlsym list,
is what predicts CPython's `configure`; the dlsym list matters for
`dlopen`-based lookups such as SDL resolving Mesa. Functions absent at both
link and run time (e.g. `getentropy`, `getrandom`, `epoll_create1`,
`iconv_open`) become `ac_cv_func_*=no` in `config.site`.
