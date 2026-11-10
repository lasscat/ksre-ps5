# Console testing

Every result in this project is labelled by where it was observed. Only
output captured from a real PS5 counts as console evidence.

## What the console needs

1. A PS5 that can already run homebrew ELF payloads built with
   [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk). How a console
   reaches that state depends on its model and firmware and changes over
   time; this project does not provide or document exploits. The SDK lists
   loaders known to work: ps5-payload-elfldr, ps5-payload-websrv,
   bdj-ipv6-hen, elfloader via ps5-jar-loader, and remote_lua_loader.
2. An ELF loader listening on TCP **9021**. With an entry point that only
   offers a rudimentary loader on 9020, first send
   [ps5-payload-elfldr](https://github.com/ps5-payload-dev/elfldr) to 9020
   (pinned release `v0.27`, SHA-256 in `upstreams.lock`). elfldr keeps
   running in the background and connects each payload's stdout/stderr to
   the TCP connection that sent it, so output streams back to your computer.
   Other loaders may not stream output; the probe also writes its log to the
   console (below).
3. The console and the development computer on the same LAN, with the
   console's IP address known. Do not expose the loader port to the internet.
4. Optional: an FTP payload (for example ps5-payload-dev's ftpsrv on port
   2121) to copy log files back if your loader does not stream stdout.
5. Optional, later milestones only: a USB drive (exFAT) for game data.

**Do not update or downgrade firmware for this project.**

## One-time local configuration

```sh
mkdir -p .local-config
cp config/console.env.example .local-config/console.env
$EDITOR .local-config/console.env          # set PS5_HOST to the console's LAN address
```

`.local-config/` and `.local-work/` are ignored by git.

## Running the runtime probe (milestone 1b)

1. On the PS5, start your homebrew enabler so that the loader listens on 9021.
   Stay on the home screen; close any running game.
2. On the computer, get the payload — build it (`make -C
   experiments/01-runtime-probe ps5`) or download the CI artifact (see
   BUILDING.md).
3. Send it:

   ```sh
   tools/deploy.sh build/experiments/01-runtime-probe/ps5/ksre-ps5-probe.elf
   # or, for the CI artifact:
   tools/deploy.sh build/ci-probe/ksre-ps5-probe.elf
   ```

   Output is printed and saved to `.local-work/logs/ksre-ps5-probe-<UTC>.log`.
4. On the TV, a notification "ksre-ps5 probe: started" appears, then
   "ksre-ps5 probe: N pass, M fail, K warn. Log in /data/ksre-ps5/logs" after
   roughly 5–30 seconds (most of it is the memory check).
5. Run it a second time. `storage.persistence` should report run #2, proving
   that `/data/ksre-ps5` survives payload exit. Rebooting the console between
   runs additionally proves persistence across power cycles.

If nothing streams back but the notifications appear, fetch
`/data/ksre-ps5/logs/probe-*.log` with FTP and place it in
`.local-work/logs/`.

### Expected output shape

```
KSRE_PS5_PROBE_BEGIN version=1 target=ps5
PROBE INFO log.file :: /data/ksre-ps5/logs/probe-<time>-<pid>.log
PROBE INFO platform.uname :: sysname=... release=... machine=...
PROBE INFO platform.firmware :: rc=0 string=... version=0x........ (API layout unverified)
PROBE PASS time.monotonic_sleep :: nanosleep(20ms) measured 2x.xx ms
PROBE PASS threads.tls :: __thread values isolated per thread (main=-1, distinct addresses)
...
PROBE PASS dl.sprx :: loaded libSceRandom.sprx and called sceRandomGetRandomNumber
PROBE INFO dl.symbols :: fork=? execv=? ... (run-time resolvability)
PROBE PASS storage.rw :: 1 MiB write+fsync+read verified in ... ms
PROBE INFO storage.persistence :: this is run #1 of the probe in /data/ksre-ps5 (previous runs: 0)
PROBE PASS|WARN jit.wx :: ...
KSRE_PS5_PROBE_CHECKPOINT before=memory pass=.. fail=.. warn=.. skip=.. info=..
PROBE PASS memory.headroom :: allocated and touched 2048 MiB (cap 2048 MiB) in ... ms
KSRE_PS5_PROBE_END version=1 target=ps5 pass=.. fail=.. warn=.. skip=.. info=..
PROBE INFO exit.atexit :: atexit handler ran
```

Values marked `...` or `?` are what the experiment exists to discover; there
is no "correct" answer for INFO lines. Interpretation:

* **FAIL** in `threads.*`, `storage.*`, `ipc.*`, `float.*` or `time.*` is a
  blocker for CPython and must be investigated before milestone 3.
* `jit.wx` WARN/FAIL means a JIT-based software GL rasteriser (llvmpipe) is
  not viable; the hardware GL path becomes mandatory.
* `memory.headroom` WARN means less than the 2 GiB cap was available.
* Output stopping after the CHECKPOINT line means the system killed the
  payload during the memory check (useful evidence, not a crash of the console).
* Output stopping elsewhere: the last line names the check that crashed.

The log contains the console model and firmware version. Keep it local; do
not attach raw logs to public issues.

## Payloads that draw to the TV (milestone 2a onwards)

**Do not send graphics payloads straight to elfldr.** A payload sent to the
loader on port 9021 is a background process. Experiment 02a, sent that way,
presented 600 frames at 59.9 fps with no SDL error and nothing appeared on the
TV (console log + maintainer observation). Started through the websrv
*homebrew launcher* instead, the same program runs as a foreground
application (`sceSystemServiceGetAppIdOfRunningBigApp` non-zero, pad and
keyboard open successfully) and the maintainer reported the picture appeared.
The launcher source (`websrv/src/ps5/hbldr.c`) explains why: it creates a
fake application (`FAKE00000`), launches it with `sceSystemServiceLaunchApp`
and substitutes the homebrew ELF.

Requirements on the console: ps5-payload-websrv (HTTP 8080) and ftpsrv (FTP
2121) running. websrv listens **without authentication**: use it only on a
trusted LAN, and note it keeps listening until the console restarts.

```sh
tools/fetch-websrv.sh                       # pinned, sha256-verified ELF
tools/deploy.sh "$(tools/fetch-websrv.sh)"  # one-time: start websrv via elfldr
tools/deploy-hb.sh ksre-02a build/experiments/02a-sdl-display/ps5/ksre-ps5-02a-sdl-display.elf
tools/deploy-hb.sh ksre-02a build/experiments/02a-sdl-display/ps5/ksre-ps5-02a-sdl-display.elf --no-upload   # run again
```

`deploy-hb.sh` uploads `eboot.elf` over FTP to `/data/homebrew/<name>/`,
launches it with `GET /hbldr?path=...&pipe=1`, and saves the streamed output
to `.local-work/logs/<name>-hb-<UTC>.log`. Remove the directory afterwards
with any FTP client if you want a clean `/data/homebrew`.

Further options (milestone 2b): `--extra LOCAL[:NAME]` uploads more files next
to `eboot.elf` (skipped when the remote size already matches; `--reupload`
forces), `--env "A=1 B=2"` sets the payload's environment (the launcher's `env=`
parameter), `--args "..."` passes arguments. The runner returns by itself:
as soon as the payload prints a `KSRE_PS5_<name>_END` line (`--until REGEX`
changes the pattern), when the connection closes, after `--idle-timeout`
seconds without any output (default 90; raise it for a payload that is quiet for
longer), or at `--timeout SECONDS` (hard limit, default 120). Payloads should
therefore print one end marker as their last line and then exit.
Payloads that load Mesa or any shared object that maps executable memory must
be linked with `-rdynamic` and should leave via `_exit()` (see
[GRAPHICS.md](GRAPHICS.md)). The payload's working directory is `/data/homebrew/<name>`, and the
SDK's run-time linker searches the working directory for shared libraries a
payload `dlopen()`s, so Mesa's `.so` files go there (no `/user/homebrew`
needed). Graphics payloads can leave the console's loader services stopped
after a crash; if `websrv`/`ftpsrv`/`elfldr` stop answering, restart them with
your usual enabler before the next run.

## Cleanup

The probe only writes inside `/data/ksre-ps5/`. Delete that directory (for
example over FTP) to remove everything it created.
