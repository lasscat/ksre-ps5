/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experiment 02c: controller events, a generated 440 Hz tone, storage round-trip.
 *
 * Questions this answers (milestone 02c):
 *   1. Which SDL joysticks does the PS5 port report, and does SDL treat the PS5 pad as a
 *      GameController (natively through the driver's mapping hook, or only after we add a
 *      mapping string)?  Ren'Py navigates with CONTROLLER* events only.
 *   2. Which events arrive for every face button, d-pad direction, stick, trigger, shoulder,
 *      stick click, Options and touchpad (and what raw/controller values do triggers rest at)?
 *   3. Does SDL audio at 48 kHz play a tone generated in code (no audio files), with the
 *      callback running at the real-time rate?
 *   4. Does /data/ksre-ps5/ survive a write -> fsync -> rename -> read-back round-trip,
 *      and does it persist across runs?
 *
 * Timeline (PS5): storage test, device listing, 3 s tone ("listen"), then the input window
 * (default 45 s) in which every event is logged.  The run ends when the window is over or
 * when L1+R1+Options are held together for 1 s, then prints exactly one
 * "KSRE_PS5_02C_END fail=N" line last and leaves with _exit().
 *
 * Options:  --window-ms N     input window length (default 45000)
 *           --tone-ms N       length of the tone ("listen") phase (default 3000)
 *           --combo-ms N      L1+R1+Options hold time that ends the window (default 1000)
 *           --data-dir DIR    storage directory (PS5 default /data/ksre-ps5/02c)
 *           --mapping STR     GameController mapping to add if the pad is not recognised
 *           --require-coverage  count a never-seen required control as a failure
 *           --no-audio / --no-window  skip a phase
 *           --selftest-only   tone math + storage only, no SDL device (host/CI)
 *           --virtual-pad     host aid: attach an SDL virtual gamepad and press everything
 *           --normal-exit     return from main instead of _exit()
 *           --stall-ms N      watchdog: no progress for N ms ends the run (default 20000)
 */
#define _GNU_SOURCE 1 /* glibc declares dladdr()/Dl_info only with this */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__PROSPERO__)
#include <ucontext.h>
#endif
#include <unistd.h>

#define VERSION "1"
#define VW 1920
#define VH 1080
#define TONE_HZ 440.0
#define TONE_RATE 48000
#define TONE_AMP 0.25 /* about -12 dBFS: audible, not loud */

#if defined(__PROSPERO__)
#define TARGET "x86_64-sie-ps5"
#else
#define TARGET "host"
#endif

/* ===========================================================================
 * Logging, watchdog, crash marker (same pattern as 02b)
 * ======================================================================== */
static int g_fail;
static FILE *g_logfile;
static volatile const char *g_stage = "start";
static volatile unsigned long g_tick;
static volatile int g_main_done;
static long g_stall_ms = 20000, g_total_ms = 240000;

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
  if (g_logfile) {
    va_start(ap, fmt);
    vfprintf(g_logfile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logfile);
    fflush(g_logfile);
  }
}

#define OK(what, ...) say("02C PASS " what " :: " __VA_ARGS__)
#define INFO(what, ...) say("02C INFO " what " :: " __VA_ARGS__)
#define EVT(what, ...) say("02C EVT " what " :: " __VA_ARGS__)
#define FAIL(what, ...)                                                        \
  do {                                                                         \
    g_fail++;                                                                  \
    say("02C FAIL " what " :: " __VA_ARGS__);                                  \
  } while (0)
#define FAIL_SDL(what)                                                         \
  do {                                                                         \
    const char *e_ = SDL_GetError();                                           \
    g_fail++;                                                                  \
    say("02C FAIL " what " :: SDL_GetError: %s", e_ && *e_ ? e_ : "(empty)");  \
  } while (0)

static void stage(const char *s) {
  g_stage = s;
  g_tick++;
}

static void log_output(void *ud, int cat, SDL_LogPriority pri, const char *msg) {
  (void)ud;
  say("02C SDLLOG [cat=%d pri=%d] %s", cat, (int)pri, msg);
}

#if defined(__PROSPERO__)
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
static void notify(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
static void notify(const char *msg) { (void)msg; }
#endif

static void *watchdog_main(void *arg) {
  (void)arg;
  unsigned long last = g_tick;
  long idle = 0, total = 0;
  while (!g_main_done) {
    usleep(250 * 1000);
    total += 250;
    if (g_tick != last) {
      last = g_tick;
      idle = 0;
    } else {
      idle += 250;
    }
    if (idle >= g_stall_ms || total >= g_total_ms) {
      say("02C FAIL watchdog :: %s (no progress for %ld ms, total %ld ms) in stage '%s'; _exit(3)",
          total >= g_total_ms ? "total time limit" : "stall", idle, total, (const char *)g_stage);
      say("KSRE_PS5_02C_END fail=%d watchdog=1 stage=%s", g_fail + 1, (const char *)g_stage);
      _exit(3);
    }
  }
  return NULL;
}

#if defined(__PROSPERO__)
static int g_peek_pipe[2] = {-1, -1};

static int peek_bytes(const void *addr, unsigned char *out, size_t n) {
  if (g_peek_pipe[0] < 0 || !addr) return -1;
  ssize_t w = write(g_peek_pipe[1], addr, n);
  if (w != (ssize_t)n) return -1;
  return read(g_peek_pipe[0], out, n) == (ssize_t)n ? 0 : -1;
}

/* Fault details.  The PS5 signal frame is not the header's layout (02b): rip = word 26,
 * cs 0x43 = word 27, rflags 28, rsp 29, rbp 9 of uc_mcontext. */
static void crash_action(int sig, siginfo_t *si, void *ucv) {
  char buf[640];
  const ucontext_t *uc = ucv;
  const unsigned long *w = (const unsigned long *)&uc->uc_mcontext;
  int raw = (w[27] & 0xffff) == 0x43;
  void *rip = raw ? (void *)w[26] : (void *)uc->uc_mcontext.mc_rip;
  void *rsp = raw ? (void *)w[29] : (void *)uc->uc_mcontext.mc_rsp;
  Dl_info di;
  memset(&di, 0, sizeof di);
  int have = dladdr(rip, &di);
  int n = snprintf(buf, sizeof buf,
                   "\n02C FAIL crash :: signal %d code %d fault_addr=%p rip=%p rsp=%p rbp=%p (context=%s) in %s+0x%lx : %s\n"
                   "02C FAIL crash :: stage '%s'\n",
                   sig, si ? si->si_code : -1, si ? si->si_addr : NULL, rip, rsp, (void *)w[9], raw ? "raw" : "header",
                   have && di.dli_fname ? di.dli_fname : "(unknown module)",
                   have ? (unsigned long)((char *)rip - (char *)di.dli_fbase) : 0UL,
                   have && di.dli_sname ? di.dli_sname : "(no symbol)", (const char *)g_stage);
  if (n > 0) {
    (void)!write(1, buf, (size_t)n);
    if (g_logfile) (void)!write(fileno(g_logfile), buf, (size_t)n);
  }
  {
    char big[4096];
    int m = 0;
    m += snprintf(big + m, sizeof big - (size_t)m, "02C FAIL crash :: mcontext words:");
    for (int i = 0; i < 32; i++) m += snprintf(big + m, sizeof big - (size_t)m, " %lx", w[i]);
    unsigned char code[48];
    m += snprintf(big + m, sizeof big - (size_t)m, "\n02C FAIL crash :: code at rip-16 (48 bytes):");
    if (peek_bytes((const unsigned char *)rip - 16, code, sizeof code) == 0)
      for (int i = 0; i < 48; i++) m += snprintf(big + m, sizeof big - (size_t)m, " %02x", code[i]);
    else
      m += snprintf(big + m, sizeof big - (size_t)m, " (unreadable)");
    m += snprintf(big + m, sizeof big - (size_t)m, "\n");
    unsigned long st[48];
    size_t room = 4096 - ((size_t)(uintptr_t)rsp & 4095);
    size_t want = room < sizeof st ? room : sizeof st;
    int nst = (int)(want / sizeof st[0]);
    if (nst > 0 && peek_bytes(rsp, (unsigned char *)st, (size_t)nst * sizeof st[0]) == 0) {
      for (int i = 0; i < nst && m < (int)sizeof big - 200; i++) {
        Dl_info d2;
        memset(&d2, 0, sizeof d2);
        if (st[i] > 0x10000 && dladdr((void *)st[i], &d2) && d2.dli_fname) {
          const char *b = strrchr(d2.dli_fname, '/');
          m += snprintf(big + m, sizeof big - (size_t)m, "02C FAIL crash :: stack[%d] %lx %s+0x%lx\n", i, st[i],
                        b ? b + 1 : d2.dli_fname, (unsigned long)(st[i] - (unsigned long)d2.dli_fbase));
        } else if (i < 28) {
          m += snprintf(big + m, sizeof big - (size_t)m, "02C FAIL crash :: stack[%d] %lx\n", i, st[i]);
        }
      }
    } else {
      m += snprintf(big + m, sizeof big - (size_t)m, "02C FAIL crash :: stack unreadable\n");
    }
    m += snprintf(big + m, sizeof big - (size_t)m, "KSRE_PS5_02C_END fail=%d crash=%d stage=%s\n", g_fail + 1, sig,
                  (const char *)g_stage);
    (void)!write(1, big, (size_t)m);
    if (g_logfile) (void)!write(fileno(g_logfile), big, (size_t)m);
  }
  _exit(4);
}
#endif

static void crash_handler(int sig) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "\n02C FAIL crash :: signal %d in stage '%s'\nKSRE_PS5_02C_END fail=%d crash=%d stage=%s\n", sig,
                   (const char *)g_stage, g_fail + 1, sig, (const char *)g_stage);
  if (n > 0) {
    (void)!write(1, buf, (size_t)n);
    if (g_logfile) (void)!write(fileno(g_logfile), buf, (size_t)n);
  }
  _exit(4);
}

/* ===========================================================================
 * Options
 * ======================================================================== */
typedef struct {
  long window_ms, tone_ms, combo_ms;
  const char *data_dir;
  const char *mapping;
  int require_coverage, no_audio, no_window, selftest_only, virtual_pad, normal_exit;
} Opts;

/* ===========================================================================
 * Tone generator (shared by the audio callback and the self-test)
 * ======================================================================== */
typedef struct {
  double phase;
  double gain; /* 0..1, ramped to avoid clicks */
} Tone;

/* Renders `frames` frames of interleaved float samples.  `on` selects the gain target. The gain
 * moves at most 1/(0.02 s * rate) per sample, i.e. a 20 ms fade in/out. */
static void tone_render(Tone *t, float *out, int frames, int channels, int on) {
  const double step = 2.0 * M_PI * TONE_HZ / TONE_RATE;
  const double ramp = 1.0 / (0.020 * TONE_RATE);
  for (int i = 0; i < frames; i++) {
    if (on) {
      t->gain += ramp;
      if (t->gain > 1.0) t->gain = 1.0;
    } else {
      t->gain -= ramp;
      if (t->gain < 0.0) t->gain = 0.0;
    }
    float s = (float)(TONE_AMP * t->gain * sin(t->phase));
    t->phase += step;
    if (t->phase >= 2.0 * M_PI) t->phase -= 2.0 * M_PI;
    for (int c = 0; c < channels; c++) out[i * channels + c] = s;
  }
}

/* Host-checkable logic: frequency by positive-going zero crossings, amplitude, no clicks. */
static void tone_selftest(void) {
  static float buf[TONE_RATE];
  Tone t = {0, 0};
  tone_render(&t, buf, TONE_RATE, 1, 1);
  int cross = 0;
  float peak = 0, maxjump = 0;
  for (int i = 1; i < TONE_RATE; i++) {
    if (buf[i - 1] < 0 && buf[i] >= 0) cross++;
    float a = fabsf(buf[i]);
    if (a > peak) peak = a;
    float j = fabsf(buf[i] - buf[i - 1]);
    if (j > maxjump) maxjump = j;
  }
  /* 440 Hz over exactly 1 s has 440 rising crossings (one fewer if the first lands on i=0). */
  if (cross >= 439 && cross <= 441 && peak > 0.24f && peak <= 0.26f && fabsf(buf[0]) < 0.001f && maxjump < 0.1f)
    OK("tone.selftest", "%.0f Hz sine: %d rising zero crossings in 1 s, peak %.3f, max sample step %.3f, starts at %.4f", TONE_HZ, cross,
       (double)peak, (double)maxjump, (double)buf[0]);
  else
    FAIL("tone.selftest", "crossings=%d peak=%.3f maxjump=%.3f first=%.4f", cross, (double)peak, (double)maxjump, (double)buf[0]);
}

/* ===========================================================================
 * Audio device
 * ======================================================================== */
static Tone g_tone;
static volatile int g_tone_on;
static SDL_AudioSpec g_have;
static volatile uint64_t g_cb_count, g_cb_frames, g_cb_first, g_cb_last, g_cb_first_len, g_cb_maxgap;

static void audio_cb(void *ud, Uint8 *stream, int len) {
  (void)ud;
  static float tmp[8192];
  int ch = g_have.channels ? g_have.channels : 2;
  int bps = SDL_AUDIO_BITSIZE(g_have.format) / 8;
  int frames = len / (bps * ch);
  if (frames * ch > (int)(sizeof tmp / sizeof tmp[0])) frames = (int)(sizeof tmp / sizeof tmp[0]) / ch;
  tone_render(&g_tone, tmp, frames, ch, g_tone_on);
  if (SDL_AUDIO_ISFLOAT(g_have.format)) {
    memcpy(stream, tmp, (size_t)frames * (size_t)ch * sizeof(float));
  } else {
    int16_t *o = (int16_t *)stream;
    for (int i = 0; i < frames * ch; i++) o[i] = (int16_t)(tmp[i] * 32767.0f);
  }
  if (frames * ch * bps < len) memset(stream + frames * ch * bps, 0, (size_t)(len - frames * ch * bps));
  uint64_t now = SDL_GetPerformanceCounter();
  if (g_cb_count == 0) {
    g_cb_first = now;
    g_cb_first_len = (uint64_t)frames;
  } else {
    uint64_t gap = (now - g_cb_last) * 1000000ull / SDL_GetPerformanceFrequency();
    if (gap > g_cb_maxgap) g_cb_maxgap = gap; /* microseconds */
  }
  g_cb_last = now;
  g_cb_frames += (uint64_t)frames;
  g_cb_count++;
}

/* ===========================================================================
 * Storage round-trip
 * ======================================================================== */
static uint32_t fnv1a(const unsigned char *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static int mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

static double now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

/* write -> fflush -> fsync -> close -> rename (the way Ren'Py writes a save), then read back
 * through stdio and through SDL_RWops and compare checksums. */
static int write_atomic(const char *dir, const char *name, const unsigned char *data, size_t n) {
  char tmp[600], fin[600];
  snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, name);
  snprintf(fin, sizeof fin, "%s/%s", dir, name);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  if (fwrite(data, 1, n, f) != n) {
    fclose(f);
    return -2;
  }
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
    fclose(f);
    return -3;
  }
  if (fclose(f) != 0) return -4;
  if (rename(tmp, fin) != 0) return -5;
  return 0;
}

static void storage_test(const char *dir) {
  stage("storage");
  if (mkdir_p(dir) != 0) {
    FAIL("storage.mkdir", "mkdir -p %s failed: %s", dir, strerror(errno));
    return;
  }
  OK("storage.mkdir", "%s exists and is a directory", dir);

  /* Persistence across runs: a counter file that outlives the process. */
  char path[600];
  snprintf(path, sizeof path, "%s/run-counter", dir);
  long prev = -1;
  {
    FILE *f = fopen(path, "rb");
    if (f) {
      if (fscanf(f, "%ld", &prev) != 1) prev = -2;
      fclose(f);
    }
  }
  char num[32];
  int nn = snprintf(num, sizeof num, "%ld\n", prev >= 0 ? prev + 1 : 1);
  if (write_atomic(dir, "run-counter", (const unsigned char *)num, (size_t)nn) == 0)
    OK("storage.persist", "run counter file: previous value %s, written %ld (it survives only if the number grows across runs)",
       prev == -1 ? "none (first run)" : (prev < 0 ? "unreadable" : "present"), prev >= 0 ? prev + 1 : 1L);
  else
    FAIL("storage.persist", "cannot write %s: %s", path, strerror(errno));
  if (prev >= 0) INFO("storage.persist", "previous run counter = %ld", prev);

  /* 1 MiB pseudo-random pattern (a save file or small image is in this range). */
  const size_t N = 1 << 20;
  unsigned char *buf = malloc(N), *back = malloc(N);
  if (!buf || !back) {
    FAIL("storage.alloc", "out of memory");
    free(buf);
    free(back);
    return;
  }
  uint32_t x = 0x1234567u;
  for (size_t i = 0; i < N; i++) {
    x = x * 1664525u + 1013904223u;
    buf[i] = (unsigned char)(x >> 24);
  }
  uint32_t want = fnv1a(buf, N);
  double t0 = now_ms();
  int rc = write_atomic(dir, "roundtrip.bin", buf, N);
  double t1 = now_ms();
  if (rc != 0) {
    FAIL("storage.write", "write_atomic rc=%d errno=%s", rc, strerror(errno));
    free(buf);
    free(back);
    return;
  }
  OK("storage.write", "%zu bytes via tmp + fsync + rename in %.1f ms", N, t1 - t0);

  struct stat st;
  snprintf(path, sizeof path, "%s/roundtrip.bin", dir);
  if (stat(path, &st) == 0 && (size_t)st.st_size == N)
    OK("storage.stat", "size %lld matches", (long long)st.st_size);
  else
    FAIL("storage.stat", "stat failed or size %lld != %zu", (long long)st.st_size, N);

  t0 = now_ms();
  FILE *f = fopen(path, "rb");
  size_t got = f ? fread(back, 1, N, f) : 0;
  if (f) fclose(f);
  t1 = now_ms();
  if (got == N && fnv1a(back, N) == want)
    OK("storage.read.stdio", "%zu bytes, checksum %08x matches, %.1f ms", N, want, t1 - t0);
  else
    FAIL("storage.read.stdio", "read %zu of %zu bytes, checksum %08x want %08x", got, N, got ? fnv1a(back, got) : 0u, want);

  memset(back, 0, N);
  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  size_t got2 = rw ? SDL_RWread(rw, back, 1, N) : 0;
  Sint64 sz = rw ? SDL_RWsize(rw) : -1;
  if (rw) SDL_RWclose(rw);
  if (got2 == N && sz == (Sint64)N && fnv1a(back, N) == want)
    OK("storage.read.rwops", "SDL_RWFromFile read %zu bytes, size %lld, checksum matches", N, (long long)sz);
  else
    FAIL("storage.read.rwops", "read %zu of %zu, size %lld, SDL_GetError: %s", got2, N, (long long)sz, SDL_GetError());

  /* Overwrite with a different payload and confirm the old content is gone (save-slot reuse). */
  buf[0] ^= 0xff;
  if (write_atomic(dir, "roundtrip.bin", buf, N) == 0) {
    f = fopen(path, "rb");
    got = f ? fread(back, 1, N, f) : 0;
    if (f) fclose(f);
    if (got == N && memcmp(back, buf, N) == 0)
      OK("storage.overwrite", "rename over an existing file replaced the content");
    else
      FAIL("storage.overwrite", "content after overwrite differs");
  } else {
    FAIL("storage.overwrite", "second write failed: %s", strerror(errno));
  }

  /* Directory listing and delete. */
  int entries = 0, found = 0;
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      entries++;
      if (!strcmp(e->d_name, "roundtrip.bin")) found = 1;
    }
    closedir(d);
  }
  if (found) OK("storage.readdir", "%d entries, roundtrip.bin listed", entries);
  else FAIL("storage.readdir", "%d entries, roundtrip.bin missing", entries);
  if (unlink(path) == 0 && stat(path, &st) != 0 && errno == ENOENT)
    OK("storage.unlink", "file removed");
  else
    FAIL("storage.unlink", "unlink failed or file still present");

  /* Where would SDL put preferences?  Informational only: Ren'Py uses RENPY_PATH_TO_SAVES. */
  char *pref = SDL_GetPrefPath("sh.fhs.ksre", "ksre-ps5");
  INFO("storage.prefpath", "SDL_GetPrefPath = %s", pref ? pref : "(NULL)");
  if (pref) SDL_free(pref);
  char *base = SDL_GetBasePath();
  INFO("storage.basepath", "SDL_GetBasePath = %s", base ? base : "(NULL)");
  if (base) SDL_free(base);

  free(buf);
  free(back);
}

/* ===========================================================================
 * Controller coverage
 * ======================================================================== */
#define MAX_SLOTS 4
#define MAX_JBTN 32
#define MAX_JAX 8
#define STICK_SEEN 16000 /* |value| a deliberate push must exceed to count as seen */

typedef struct {
  SDL_JoystickID inst; /* -1 = free */
  SDL_Joystick *js;
  SDL_GameController *gc;
  char name[96];
  unsigned char cbtn[SDL_CONTROLLER_BUTTON_MAX];
  unsigned char cax_pos[SDL_CONTROLLER_AXIS_MAX], cax_neg[SDL_CONTROLLER_AXIS_MAX];
  unsigned char jbtn[MAX_JBTN];
  unsigned char jax_pos[MAX_JAX], jax_neg[MAX_JAX];
  unsigned char hat_seen[16]; /* bitmask values 1,2,4,8 */
  int jax_last[MAX_JAX], jax_min[MAX_JAX], jax_max[MAX_JAX], jax_n[MAX_JAX], jax_logged[MAX_JAX];
  int cax_last[SDL_CONTROLLER_AXIS_MAX], cax_min[SDL_CONTROLLER_AXIS_MAX], cax_max[SDL_CONTROLLER_AXIS_MAX];
  int cax_n[SDL_CONTROLLER_AXIS_MAX], cax_logged[SDL_CONTROLLER_AXIS_MAX];
  long events, jevents, cevents;
} Slot;

static Slot g_slot[MAX_SLOTS];
static int g_nslots;
static const char *g_phase = "init";

static Slot *slot_for(SDL_JoystickID inst, int create) {
  for (int i = 0; i < g_nslots; i++)
    if (g_slot[i].inst == inst) return &g_slot[i];
  if (!create || g_nslots >= MAX_SLOTS) return NULL;
  Slot *s = &g_slot[g_nslots++];
  memset(s, 0, sizeof *s);
  s->inst = inst;
  for (int a = 0; a < MAX_JAX; a++) s->jax_min[a] = 40000, s->jax_max[a] = -40000, s->jax_logged[a] = -100000;
  for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) s->cax_min[a] = 40000, s->cax_max[a] = -40000, s->cax_logged[a] = -100000;
  return s;
}

static const char *cbtn_name(int b) {
  const char *n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
  return n ? n : "?";
}
static const char *cax_name(int a) {
  const char *n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a);
  return n ? n : "?";
}

/* Axis values are logged when they cross a threshold-sized step, so a stick sweep prints a
 * handful of lines instead of hundreds; min/max/count are kept for the summary. */
static int axis_should_log(int *logged, int v) {
  int extreme = (v >= 32000 || v <= -32000), was_extreme = (*logged >= 32000 || *logged <= -32000);
  if (abs(v - *logged) >= 6000 || (extreme && !was_extreme) || (v == 0 && *logged != 0 && abs(*logged) > 2000)) {
    *logged = v;
    return 1;
  }
  return 0;
}

static void handle_event(const SDL_Event *e) {
  g_tick++;
  switch (e->type) {
  case SDL_JOYDEVICEADDED: {
    SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(e->jdevice.which);
    EVT("joy.added", "[%s] device_index=%d instance=%d name='%s'", g_phase, e->jdevice.which, (int)inst, SDL_JoystickNameForIndex(e->jdevice.which));
    break;
  }
  case SDL_JOYDEVICEREMOVED: EVT("joy.removed", "[%s] instance=%d", g_phase, (int)e->jdevice.which); break;
  case SDL_CONTROLLERDEVICEADDED: EVT("controller.added", "[%s] device_index=%d", g_phase, e->cdevice.which); break;
  case SDL_CONTROLLERDEVICEREMOVED: EVT("controller.removed", "[%s] instance=%d", g_phase, (int)e->cdevice.which); break;
  case SDL_CONTROLLERDEVICEREMAPPED: EVT("controller.remapped", "[%s] instance=%d", g_phase, (int)e->cdevice.which); break;
  case SDL_JOYBUTTONDOWN:
  case SDL_JOYBUTTONUP: {
    Slot *s = slot_for(e->jbutton.which, 1);
    int down = e->type == SDL_JOYBUTTONDOWN, b = e->jbutton.button;
    if (s) {
      s->events++;
      s->jevents++;
      if (down && b < MAX_JBTN) s->jbtn[b] = 1;
    }
    EVT("joy.button", "[%s] inst=%d button=%d %s", g_phase, (int)e->jbutton.which, b, down ? "DOWN" : "up");
    break;
  }
  case SDL_JOYHATMOTION: {
    Slot *s = slot_for(e->jhat.which, 1);
    if (s) {
      s->events++;
      s->jevents++;
      s->hat_seen[e->jhat.value & 15] = 1;
    }
    EVT("joy.hat", "[%s] inst=%d hat=%d value=0x%x", g_phase, (int)e->jhat.which, e->jhat.hat, e->jhat.value);
    break;
  }
  case SDL_JOYAXISMOTION: {
    Slot *s = slot_for(e->jaxis.which, 1);
    int a = e->jaxis.axis, v = e->jaxis.value;
    if (s && a < MAX_JAX) {
      s->events++;
      s->jevents++;
      s->jax_n[a]++;
      s->jax_last[a] = v;
      if (v < s->jax_min[a]) s->jax_min[a] = v;
      if (v > s->jax_max[a]) s->jax_max[a] = v;
      if (v > STICK_SEEN) s->jax_pos[a] = 1;
      if (v < -STICK_SEEN) s->jax_neg[a] = 1;
      if (axis_should_log(&s->jax_logged[a], v)) EVT("joy.axis", "[%s] inst=%d axis=%d value=%d", g_phase, (int)e->jaxis.which, a, v);
    }
    break;
  }
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    Slot *s = slot_for(e->cbutton.which, 1);
    int down = e->type == SDL_CONTROLLERBUTTONDOWN, b = e->cbutton.button;
    if (s) {
      s->events++;
      s->cevents++;
      if (down && b < SDL_CONTROLLER_BUTTON_MAX) s->cbtn[b] = 1;
    }
    EVT("controller.button", "[%s] inst=%d button=%d (%s) %s", g_phase, (int)e->cbutton.which, b, cbtn_name(b), down ? "DOWN" : "up");
    break;
  }
  case SDL_CONTROLLERAXISMOTION: {
    Slot *s = slot_for(e->caxis.which, 1);
    int a = e->caxis.axis, v = e->caxis.value;
    if (s && a < SDL_CONTROLLER_AXIS_MAX) {
      s->events++;
      s->cevents++;
      s->cax_n[a]++;
      s->cax_last[a] = v;
      if (v < s->cax_min[a]) s->cax_min[a] = v;
      if (v > s->cax_max[a]) s->cax_max[a] = v;
      if (v > STICK_SEEN) s->cax_pos[a] = 1;
      if (v < -STICK_SEEN) s->cax_neg[a] = 1;
      if (axis_should_log(&s->cax_logged[a], v))
        EVT("controller.axis", "[%s] inst=%d axis=%d (%s) value=%d", g_phase, (int)e->caxis.which, a, cax_name(a), v);
    }
    break;
  }
  default: break;
  }
}

/* Required controls = what Ren'Py 8.5.3's default pad_bindings (00keymap.rpy) use, plus every
 * physical control the task lists.  Back/Guide/Misc1 are not delivered by this SDL port
 * (driver source: btn_map has no PS/Share entry; mapping has no `back`). */
typedef struct {
  int is_axis, idx, positive; /* button index, or axis index + direction */
  const char *label;
} Req;
static const Req g_req[] = {
    {0, SDL_CONTROLLER_BUTTON_A, 0, "A (Cross)"},
    {0, SDL_CONTROLLER_BUTTON_B, 0, "B (Circle)"},
    {0, SDL_CONTROLLER_BUTTON_X, 0, "X (Square)"},
    {0, SDL_CONTROLLER_BUTTON_Y, 0, "Y (Triangle)"},
    {0, SDL_CONTROLLER_BUTTON_DPAD_UP, 0, "dpup"},
    {0, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, "dpdown"},
    {0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, "dpleft"},
    {0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, "dpright"},
    {0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0, "leftshoulder (L1)"},
    {0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, "rightshoulder (R1)"},
    {0, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0, "leftstick (L3)"},
    {0, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0, "rightstick (R3)"},
    {0, SDL_CONTROLLER_BUTTON_START, 0, "start (Options)"},
    {1, SDL_CONTROLLER_AXIS_LEFTX, 1, "leftx+ (left stick right)"},
    {1, SDL_CONTROLLER_AXIS_LEFTX, 0, "leftx- (left stick left)"},
    {1, SDL_CONTROLLER_AXIS_LEFTY, 1, "lefty+ (left stick down)"},
    {1, SDL_CONTROLLER_AXIS_LEFTY, 0, "lefty- (left stick up)"},
    {1, SDL_CONTROLLER_AXIS_RIGHTX, 1, "rightx+ (right stick right)"},
    {1, SDL_CONTROLLER_AXIS_RIGHTX, 0, "rightx- (right stick left)"},
    {1, SDL_CONTROLLER_AXIS_RIGHTY, 1, "righty+ (right stick down)"},
    {1, SDL_CONTROLLER_AXIS_RIGHTY, 0, "righty- (right stick up)"},
    {1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1, "lefttrigger (L2)"},
    {1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1, "righttrigger (R2)"},
};
#define NREQ ((int)(sizeof g_req / sizeof g_req[0]))
static const int g_opt_btn[] = {SDL_CONTROLLER_BUTTON_TOUCHPAD, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
                                SDL_CONTROLLER_BUTTON_MISC1};

static int req_seen(const Slot *s, const Req *r) {
  if (!r->is_axis) return s->cbtn[r->idx];
  return r->positive ? s->cax_pos[r->idx] : s->cax_neg[r->idx];
}

/* The slot used for coverage: the one with a GameController that has the most events, else
 * the slot with the most events. */
static Slot *best_slot(void) {
  Slot *best = NULL;
  for (int i = 0; i < g_nslots; i++) {
    Slot *s = &g_slot[i];
    if (!best || (s->gc && !best->gc) || ((!!s->gc == !!best->gc) && s->events > best->events)) best = s;
  }
  return best;
}

static int req_missing_count(const Slot *s) {
  int m = 0;
  for (int i = 0; i < NREQ; i++)
    if (!req_seen(s, &g_req[i])) m++;
  return m;
}

/* ===========================================================================
 * Device listing / mapping
 * ======================================================================== */
#define PS5_GUID "0300d0424c050000e60c000011810000"
/* Mapping equal to what SDL_ps5joystick.c's GetGamepadMapping hook builds (driver source), used
 * only if SDL does not treat the pad as a GameController.  Triggers are axes 4/5. */
static const char *g_fallback_mapping =
    PS5_GUID ",PS5 Pad,a:b0,b:b1,x:b2,y:b3,touchpad:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
             "platform:PS5,";

static void open_device(int i) {
  SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
  Slot *s = slot_for(inst, 1);
  if (!s || s->js) return;
  s->js = SDL_JoystickOpen(i);
  const char *nm = SDL_JoystickNameForIndex(i);
  snprintf(s->name, sizeof s->name, "%s", nm ? nm : "(null)");
  if (!s->js) {
    FAIL_SDL("joystick.open");
    return;
  }
  if (SDL_IsGameController(i)) {
    s->gc = SDL_GameControllerOpen(i);
    if (!s->gc) FAIL_SDL("controller.open");
  }
}

static void log_devices(const Opts *o, int after_mapping) {
  int n = SDL_NumJoysticks();
  INFO("devices", "%s SDL_NumJoysticks=%d", after_mapping ? "after adding the mapping:" : "found:", n);
  for (int i = 0; i < n; i++) {
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
    int isgc = SDL_IsGameController(i);
    INFO("device", "#%d instance=%d name='%s' guid=%s is_gamecontroller=%d controller_name='%s'", i, (int)SDL_JoystickGetDeviceInstanceID(i),
         SDL_JoystickNameForIndex(i), guid, isgc, isgc ? SDL_GameControllerNameForIndex(i) : "-");
    if (isgc) {
      char *m = SDL_GameControllerMappingForDeviceIndex(i);
      INFO("device.mapping", "#%d %s", i, m ? m : "(NULL)");
      if (m) SDL_free(m);
    }
  }
  (void)o;
}

static void describe_open(void) {
  for (int i = 0; i < g_nslots; i++) {
    Slot *s = &g_slot[i];
    if (!s->js) continue;
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(s->js), guid, sizeof guid);
    INFO("joystick.caps", "inst=%d name='%s' guid=%s buttons=%d axes=%d hats=%d balls=%d rumble=%d led=%d gamecontroller=%d", (int)s->inst, s->name, guid,
         SDL_JoystickNumButtons(s->js), SDL_JoystickNumAxes(s->js), SDL_JoystickNumHats(s->js), SDL_JoystickNumBalls(s->js),
         SDL_JoystickHasRumble(s->js), SDL_JoystickHasLED(s->js), s->gc != NULL);
    if (s->gc) {
      char *m = SDL_GameControllerMapping(s->gc);
      INFO("controller.mapping", "inst=%d type=%d %s", (int)s->inst, (int)SDL_GameControllerGetType(s->gc), m ? m : "(NULL)");
      if (m) SDL_free(m);
      for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        INFO("controller.binding", "inst=%d button %d (%s) has_binding=%d", (int)s->inst, b, cbtn_name(b),
             SDL_GameControllerHasButton(s->gc, (SDL_GameControllerButton)b));
      for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        INFO("controller.binding", "inst=%d axis %d (%s) has_binding=%d", (int)s->inst, a, cax_name(a),
             SDL_GameControllerHasAxis(s->gc, (SDL_GameControllerAxis)a));
    }
  }
}

static void log_rest(const char *when) {
  for (int i = 0; i < g_nslots; i++) {
    Slot *s = &g_slot[i];
    if (!s->js) continue;
    char line[512];
    int n = snprintf(line, sizeof line, "inst=%d joystick axes:", (int)s->inst);
    for (int a = 0; a < SDL_JoystickNumAxes(s->js) && a < MAX_JAX; a++)
      n += snprintf(line + n, sizeof line - (size_t)n, " a%d=%d", a, SDL_JoystickGetAxis(s->js, a));
    if (s->gc) {
      n += snprintf(line + n, sizeof line - (size_t)n, " | controller axes:");
      for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%d", cax_name(a), SDL_GameControllerGetAxis(s->gc, (SDL_GameControllerAxis)a));
    }
    say("02C INFO %s :: %s", when, line);
  }
}

/* ===========================================================================
 * Drawing (pad-shaped, no font needed)
 * ======================================================================== */
typedef struct {
  int x, y, w, h, cbtn, jbtn;
} Tile;
static const Tile g_tiles[] = {
    {470, 380, 60, 60, SDL_CONTROLLER_BUTTON_DPAD_UP, 11},     {470, 500, 60, 60, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 12},
    {410, 440, 60, 60, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 13},   {530, 440, 60, 60, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 14},
    {1390, 380, 60, 60, SDL_CONTROLLER_BUTTON_Y, 3},           {1390, 500, 60, 60, SDL_CONTROLLER_BUTTON_A, 0},
    {1330, 440, 60, 60, SDL_CONTROLLER_BUTTON_X, 2},           {1450, 440, 60, 60, SDL_CONTROLLER_BUTTON_B, 1},
    {300, 230, 260, 50, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 9}, {1360, 230, 260, 50, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 10},
    {1000, 420, 100, 40, SDL_CONTROLLER_BUTTON_START, 6},      {800, 300, 320, 100, SDL_CONTROLLER_BUTTON_TOUCHPAD, 4},
};
#define NTILES ((int)(sizeof g_tiles / sizeof g_tiles[0]))

static void fillrect(SDL_Renderer *r, int x, int y, int w, int h, int cr, int cg, int cb) {
  SDL_Rect rc = {x, y, w, h};
  SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, 255);
  SDL_RenderFillRect(r, &rc);
}

static int joy_held(const Slot *s, int cbtn, int jbtn) {
  if (s->gc) return SDL_GameControllerGetButton(s->gc, (SDL_GameControllerButton)cbtn) != 0;
  if (s->js && jbtn < SDL_JoystickNumButtons(s->js)) return SDL_JoystickGetButton(s->js, jbtn) != 0;
  return 0;
}

static void draw_axis_box(SDL_Renderer *r, const Slot *s, int cx, int cy, int axx, int axy, int clickbtn, int clickj) {
  const int half = 150;
  int held = s && joy_held(s, clickbtn, clickj);
  fillrect(r, cx - half, cy - half, 2 * half, 2 * half, held ? 200 : 40, held ? 180 : 40, held ? 0 : 40);
  fillrect(r, cx - half + 6, cy - half + 6, 2 * half - 12, 2 * half - 12, 20, 20, 24);
  int vx = 0, vy = 0;
  if (s && s->gc) {
    vx = SDL_GameControllerGetAxis(s->gc, (SDL_GameControllerAxis)axx);
    vy = SDL_GameControllerGetAxis(s->gc, (SDL_GameControllerAxis)axy);
  }
  int px = cx + vx * (half - 20) / 32768, py = cy + vy * (half - 20) / 32768;
  fillrect(r, px - 14, py - 14, 28, 28, 80, 200, 255);
}

static void draw_frame(SDL_Renderer *r, int phase_tone, long left_ms, long total_ms, int all_seen) {
  SDL_SetRenderDrawColor(r, 12, 12, 16, 255);
  SDL_RenderClear(r);
  Slot *s = best_slot();
  /* tone indicator / all-controls-seen indicator along the top */
  if (phase_tone) fillrect(r, 560, 60, 800, 70, 255, 140, 0);
  else fillrect(r, 560, 60, 800, 70, all_seen ? 20 : 45, all_seen ? 160 : 45, all_seen ? 60 : 50);
  for (int i = 0; i < NTILES; i++) {
    const Tile *t = &g_tiles[i];
    int seen = s && s->cbtn[t->cbtn], held = s && joy_held(s, t->cbtn, t->jbtn);
    if (held) fillrect(r, t->x, t->y, t->w, t->h, 255, 220, 0);
    else if (seen) fillrect(r, t->x, t->y, t->w, t->h, 30, 120, 55);
    else fillrect(r, t->x, t->y, t->w, t->h, 70, 70, 76);
  }
  /* triggers: bar length = pulled amount (rest value may be negative: clamp) */
  for (int k = 0; k < 2; k++) {
    int x = k ? 1360 : 300, v = 0;
    int ax = k ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT;
    if (s && s->gc) v = SDL_GameControllerGetAxis(s->gc, (SDL_GameControllerAxis)ax);
    if (v < 0) v = 0;
    int seen = s && s->cax_pos[ax];
    fillrect(r, x, 160, 260, 50, 40, 40, 44);
    fillrect(r, x, 160, v * 260 / 32767, 50, 255, 220, 0);
    if (seen) fillrect(r, x, 212, 260, 8, 30, 120, 55);
  }
  draw_axis_box(r, s, 760, 740, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_BUTTON_LEFTSTICK, 7);
  draw_axis_box(r, s, 1160, 740, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 8);
  /* stick-direction seen markers under each box */
  for (int k = 0; k < 2; k++) {
    int ax = k ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX, bx = k ? 1010 : 610;
    for (int d = 0; d < 4; d++) {
      int seen = 0;
      if (s) seen = d == 0 ? s->cax_neg[ax] : d == 1 ? s->cax_pos[ax] : d == 2 ? s->cax_neg[ax + 1] : s->cax_pos[ax + 1];
      fillrect(r, bx + d * 76, 910, 66, 18, seen ? 30 : 70, seen ? 120 : 70, seen ? 55 : 76);
    }
  }
  /* time left */
  fillrect(r, 360, 1000, 1200, 24, 40, 40, 44);
  if (total_ms > 0 && left_ms > 0) fillrect(r, 360, 1000, (int)(1200 * left_ms / total_ms), 24, 70, 140, 230);
  SDL_RenderPresent(r);
}

/* ===========================================================================
 * Host aid: a virtual gamepad that presses every control (CI / logic check)
 * ======================================================================== */
#if !defined(__PROSPERO__)
static SDL_Joystick *g_vjs;
static int g_vinst = -1;

static void virtual_attach(void) {
  SDL_VirtualJoystickDesc d;
  SDL_zero(d);
  d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  d.naxes = SDL_CONTROLLER_AXIS_MAX;
  d.nbuttons = SDL_CONTROLLER_BUTTON_MISC1 + 1;
  d.name = "ksre virtual pad";
  int idx = SDL_JoystickAttachVirtualEx(&d);
  if (idx < 0) INFO("virtual", "attach failed: %s", SDL_GetError());
  else g_vinst = (int)SDL_JoystickGetDeviceInstanceID(idx);
}

/* t = ms since the start of the input window.  One control every 60 ms, release 30 ms later. */
static void virtual_step(long t) {
  if (!g_vjs) {
    if (g_vinst < 0) return;
    g_vjs = SDL_JoystickFromInstanceID(g_vinst);
    if (!g_vjs) return;
  }
  /* Script: buttons 0..14 (the combo buttons last), then axes (both directions). */
  static const int order[] = {0, 1, 2, 3, 11, 12, 13, 14, 7, 8, 9, 10, 6};
  const int nb = (int)(sizeof order / sizeof order[0]);
  long step = t / 60, phase = t % 60;
  if (step < nb - 3) { /* everything except L1, R1, START which stay for the combo */
    SDL_JoystickSetVirtualButton(g_vjs, order[step], phase < 30 ? 1 : 0);
  } else if (step < nb - 3 + 6 * 2) {
    long k = step - (nb - 3), axis = k / 2;
    int dir = (k % 2) ? 32767 : -32767;
    if (axis >= 4) dir = 32767; /* triggers pull only */
    SDL_JoystickSetVirtualAxis(g_vjs, (int)axis, phase < 30 ? dir : 0);
  } else if (step < nb - 3 + 12 + 30) {
    SDL_JoystickSetVirtualButton(g_vjs, 9, 1);
    SDL_JoystickSetVirtualButton(g_vjs, 10, 1);
    SDL_JoystickSetVirtualButton(g_vjs, 6, 1);
  }
}
#endif

/* ===========================================================================
 * Main
 * ======================================================================== */
static int combo_held(void) {
  for (int i = 0; i < g_nslots; i++) {
    Slot *s = &g_slot[i];
    if (!s->js || SDL_JoystickNumButtons(s->js) < 11) continue;
    if (SDL_JoystickGetButton(s->js, 9) && SDL_JoystickGetButton(s->js, 10) && SDL_JoystickGetButton(s->js, 6)) return 1;
  }
  return 0;
}

static void report_coverage(const Opts *o) {
  stage("report");
  for (int i = 0; i < g_nslots; i++) {
    Slot *s = &g_slot[i];
    INFO("summary.slot", "inst=%d name='%s' controller=%d events=%ld (joystick %ld, controller %ld)", (int)s->inst, s->name, s->gc != NULL, s->events,
         s->jevents, s->cevents);
    for (int a = 0; a < MAX_JAX; a++)
      if (s->jax_n[a])
        INFO("summary.joyaxis", "inst=%d axis=%d events=%d min=%d max=%d last=%d", (int)s->inst, a, s->jax_n[a], s->jax_min[a], s->jax_max[a], s->jax_last[a]);
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
      if (s->cax_n[a])
        INFO("summary.ctrlaxis", "inst=%d axis=%d (%s) events=%d min=%d max=%d last=%d", (int)s->inst, a, cax_name(a), s->cax_n[a], s->cax_min[a],
             s->cax_max[a], s->cax_last[a]);
    char list[256] = "";
    for (int b = 0; b < MAX_JBTN; b++)
      if (s->jbtn[b]) snprintf(list + strlen(list), sizeof list - strlen(list), " %d", b);
    INFO("summary.joybuttons", "inst=%d joystick buttons seen:%s", (int)s->inst, list[0] ? list : " none");
    snprintf(list, sizeof list, "%s", "");
    for (int h = 1; h < 16; h++)
      if (s->hat_seen[h]) snprintf(list + strlen(list), sizeof list - strlen(list), " 0x%x", h);
    INFO("summary.hat", "inst=%d hat values seen:%s", (int)s->inst, list[0] ? list : " none");
  }
  Slot *s = best_slot();
  if (!s || s->events == 0) {
    FAIL("coverage", "no joystick or controller event arrived at all");
    return;
  }
  INFO("coverage.slot", "coverage evaluated on instance %d '%s' (controller=%d)", (int)s->inst, s->name, s->gc != NULL);
  int missing = 0;
  for (int i = 0; i < NREQ; i++) {
    int seen = req_seen(s, &g_req[i]);
    if (!seen) missing++;
    INFO("coverage.control", "%-30s %s", g_req[i].label, seen ? "seen" : "NOT SEEN");
  }
  for (size_t i = 0; i < sizeof g_opt_btn / sizeof g_opt_btn[0]; i++)
    INFO("coverage.optional", "%-30s %s", cbtn_name(g_opt_btn[i]), s->cbtn[g_opt_btn[i]] ? "seen" : "not seen");
  if (missing == 0)
    OK("coverage", "all %d required controller controls produced events", NREQ);
  else if (o->require_coverage)
    FAIL("coverage", "%d of %d required controls never produced a controller event", missing, NREQ);
  else
    INFO("coverage", "%d of %d required controls never produced a controller event (not counted: --require-coverage off)", missing, NREQ);
}

static int app_main(int argc, char **argv) {
  Opts o = {.window_ms = 45000, .tone_ms = 3000, .combo_ms = 1000};
#if defined(__PROSPERO__)
  o.data_dir = "/data/ksre-ps5/02c";
#else
  o.data_dir = "ksre-ps5-02c-data";
#endif
  o.mapping = g_fallback_mapping;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--window-ms") && i + 1 < argc) o.window_ms = atol(argv[++i]);
    else if (!strcmp(argv[i], "--tone-ms") && i + 1 < argc) o.tone_ms = atol(argv[++i]);
    else if (!strcmp(argv[i], "--combo-ms") && i + 1 < argc) o.combo_ms = atol(argv[++i]);
    else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) o.data_dir = argv[++i];
    else if (!strcmp(argv[i], "--mapping") && i + 1 < argc) o.mapping = argv[++i];
    else if (!strcmp(argv[i], "--stall-ms") && i + 1 < argc) g_stall_ms = atol(argv[++i]);
    else if (!strcmp(argv[i], "--require-coverage")) o.require_coverage = 1;
    else if (!strcmp(argv[i], "--no-audio")) o.no_audio = 1;
    else if (!strcmp(argv[i], "--no-window")) o.no_window = 1;
    else if (!strcmp(argv[i], "--selftest-only")) o.selftest_only = 1;
    else if (!strcmp(argv[i], "--virtual-pad")) o.virtual_pad = 1;
    else if (!strcmp(argv[i], "--normal-exit")) o.normal_exit = 1;
    else if (i == 1 && argv[i][0] != '-') continue; /* the launcher may pass a program name */
    else INFO("args", "ignored argument '%s'", argv[i]);
  }
  if (o.window_ms < 1000) o.window_ms = 1000;
  g_total_ms = o.window_ms + o.tone_ms + 120000;

#if defined(__PROSPERO__)
  mkdir("/data/ksre-ps5", 0777);
  mkdir_p(o.data_dir);
  char logp[600];
  snprintf(logp, sizeof logp, "%s/last-run.log", o.data_dir);
  g_logfile = fopen(logp, "w");
  if (pipe(g_peek_pipe) != 0) g_peek_pipe[0] = g_peek_pipe[1] = -1;
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_action;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
  }
#else
  signal(SIGSEGV, crash_handler);
  signal(SIGBUS, crash_handler);
  signal(SIGILL, crash_handler);
#endif
  signal(SIGFPE, crash_handler);
  signal(SIGABRT, crash_handler);

  say("KSRE_PS5_02C_BEGIN version=%s target=%s window_ms=%ld tone_ms=%ld combo_ms=%ld data_dir=%s require_coverage=%d", VERSION, TARGET,
      o.window_ms, o.tone_ms, o.combo_ms, o.data_dir, o.require_coverage);

  pthread_t wd;
  int wd_up = 0;
  {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&wd, &at, watchdog_main, NULL) == 0) {
      wd_up = 1;
      INFO("watchdog", "armed: stall limit %ld ms, total limit %ld ms", g_stall_ms, g_total_ms);
    } else {
      INFO("watchdog", "could not start (non-fatal)");
    }
    pthread_attr_destroy(&at);
  }

  SDL_SetMainReady();
  SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
  SDL_LogSetOutputFunction(log_output, NULL);
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if !defined(__PROSPERO__)
  if (o.virtual_pad) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#endif
  {
    SDL_version cv, lv;
    SDL_VERSION(&cv);
    SDL_GetVersion(&lv);
    INFO("sdl.version", "compiled %d.%d.%d linked %d.%d.%d revision=%s platform=%s", cv.major, cv.minor, cv.patch, lv.major, lv.minor, lv.patch,
         SDL_GetRevision(), SDL_GetPlatform());
  }

  SDL_Window *win = NULL;
  SDL_Renderer *ren = NULL;
  SDL_AudioDeviceID adev = 0;
  int sdl_up = 0;
  stage("selftest");
  tone_selftest();

  if (o.selftest_only) {
    storage_test(o.data_dir);
    goto done;
  }

  stage("sdl.init");
  Uint32 flags = SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  if (!o.no_window) flags |= SDL_INIT_VIDEO;
  if (!o.no_audio) flags |= SDL_INIT_AUDIO;
  if (SDL_Init(flags) != 0) {
    FAIL_SDL("sdl.init");
    goto done;
  }
  sdl_up = 1;
  OK("sdl.init", "video=%s audio=%s joystick+gamecontroller", !o.no_window ? SDL_GetCurrentVideoDriver() : "(off)",
     !o.no_audio ? SDL_GetCurrentAudioDriver() : "(off)");

  storage_test(o.data_dir);

  /* ---- devices ---- */
  stage("devices");
#if !defined(__PROSPERO__)
  if (o.virtual_pad) virtual_attach();
#endif
  SDL_JoystickUpdate();
  log_devices(&o, 0);
  if (SDL_NumJoysticks() < 1) {
    FAIL("joystick.found", "SDL reports no joystick");
  } else {
    OK("joystick.found", "%d joystick(s)", SDL_NumJoysticks());
  }
  /* The PS5 pad must be a GameController.  If SDL does not treat it as one, add our mapping. */
  {
    int needs_mapping = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
      char guid[64];
      SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
      if (!strcmp(guid, PS5_GUID)) {
        if (SDL_IsGameController(i)) {
          OK("controller.recognised", "#%d guid %s is a GameController natively (no mapping string needed)", i, guid);
        } else {
          needs_mapping = 1;
          INFO("controller.recognised", "#%d guid %s is NOT a GameController; adding mapping", i, guid);
        }
      }
    }
    if (needs_mapping) {
      int rc = SDL_GameControllerAddMapping(o.mapping);
      INFO("controller.addmapping", "SDL_GameControllerAddMapping rc=%d (1=added 0=updated -1=error) %s", rc, o.mapping);
      log_devices(&o, 1);
      for (int i = 0; i < SDL_NumJoysticks(); i++) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
        if (!strcmp(guid, PS5_GUID)) {
          if (SDL_IsGameController(i)) OK("controller.recognised", "#%d is a GameController after adding the mapping", i);
          else FAIL("controller.recognised", "#%d is still not a GameController after adding the mapping", i);
        }
      }
    }
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++) open_device(i);
  describe_open();
  {
    int any_gc = 0;
    for (int i = 0; i < g_nslots; i++) any_gc += g_slot[i].gc != NULL;
    if (any_gc) OK("controller.open", "%d GameController(s) open", any_gc);
    else FAIL("controller.open", "no GameController could be opened");
  }
  log_rest("rest.before");

  /* ---- window ---- */
  if (!o.no_window) {
    stage("window");
    win = SDL_CreateWindow("ksre-ps5 02c", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VW, VH, SDL_WINDOW_SHOWN);
    if (!win) {
      FAIL_SDL("window.create");
    } else {
      ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
      if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
      if (!ren) FAIL_SDL("render.create");
      else {
        SDL_RenderSetLogicalSize(ren, VW, VH);
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        OK("window.create", "%dx%d software renderer", w, h);
      }
    }
  }

  /* ---- audio ---- */
  if (!o.no_audio) {
    stage("audio.open");
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = TONE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    for (int i = 0; i < SDL_GetNumAudioDrivers(); i++) INFO("audio.driver", "#%d %s", i, SDL_GetAudioDriver(i));
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &g_have, 0);
    if (!adev) {
      FAIL_SDL("audio.open");
    } else {
      OK("audio.open", "device %u: freq=%d format=0x%04x channels=%d samples=%d size=%u (wanted %d Hz F32 stereo 1024)", (unsigned)adev, g_have.freq,
         (unsigned)g_have.format, g_have.channels, g_have.samples, (unsigned)g_have.size, TONE_RATE);
      if (g_have.freq != TONE_RATE) FAIL("audio.rate", "device runs at %d Hz, not %d", g_have.freq, TONE_RATE);
    }
  }

  /* ---- timeline ---- */
  int quit = 0;
  long combo_since = -1;
  long window_left = o.window_ms;
  Uint32 t_start = SDL_GetTicks(), t_window = 0;
  int in_tone = (adev != 0) && o.tone_ms > 0;
  say("02C INFO prompt :: %s", in_tone ? "LISTEN: a 440 Hz tone plays now" : "input window starts now");
  notify("ksre-ps5 02c: listen for the tone, then press every button");
  stage("tone");
  g_phase = in_tone ? "prelude" : "window";
  if (in_tone) {
    g_tone_on = 1;
    SDL_PauseAudioDevice(adev, 0);
  } else {
    t_window = SDL_GetTicks();
  }
  int all_seen = 0, all_seen_logged = 0;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        INFO("quit", "SDL_QUIT received; stopping early");
        quit = 1;
      }
      handle_event(&e);
    }
    Uint32 now = SDL_GetTicks();
    g_tick++;
    if (in_tone && (long)(now - t_start) >= o.tone_ms) {
      in_tone = 0;
      g_tone_on = 0;
      g_phase = "window";
      t_window = now;
      uint64_t cnt = g_cb_count, fr = g_cb_frames;
      double dt = cnt > 1 ? (double)(g_cb_last - g_cb_first) / (double)SDL_GetPerformanceFrequency() : 0.0;
      double rate = dt > 0 ? (double)(fr - g_cb_first_len) / dt : 0.0;
      INFO("audio.stats", "tone phase: %llu callbacks, %llu frames, %.1f frames/s, longest gap between callbacks %.1f ms (period %.1f ms)",
           (unsigned long long)cnt, (unsigned long long)fr, rate, (double)g_cb_maxgap / 1000.0, 1000.0 * g_have.samples / TONE_RATE);
      if (cnt >= 5 && rate > 0.9 * TONE_RATE && rate < 1.1 * TONE_RATE)
        OK("audio.tone", "callback delivered %.0f frames/s (target %d) during the %ld ms tone", rate, TONE_RATE, o.tone_ms);
      else
        FAIL("audio.tone", "callback rate %.0f frames/s with %llu callbacks (target %d)", rate, (unsigned long long)cnt, TONE_RATE);
      say("02C INFO prompt :: tone finished; INPUT WINDOW: press every button, move both sticks, pull both triggers (%ld s)", o.window_ms / 1000);
      notify("ksre-ps5 02c: now press every button and move the sticks");
      log_rest("rest.window-start");
    }
    if (!in_tone) {
      long el = (long)(now - t_window);
      window_left = o.window_ms - el;
#if !defined(__PROSPERO__)
      if (o.virtual_pad) virtual_step(el);
#endif
      if (combo_held()) {
        if (combo_since < 0) {
          combo_since = (long)now;
          INFO("combo", "L1+R1+Options held; ending in %ld ms if kept", o.combo_ms);
        } else if ((long)now - combo_since >= o.combo_ms && el > 500) {
          INFO("combo", "combo held for %ld ms: ending the input window early (%ld ms in)", o.combo_ms, el);
          break;
        }
      } else {
        combo_since = -1;
      }
      if (window_left <= 0) {
        INFO("window", "input window over (%ld ms)", o.window_ms);
        break;
      }
      Slot *bs = best_slot();
      all_seen = bs && req_missing_count(bs) == 0;
      if (all_seen && !all_seen_logged) {
        all_seen_logged = 1;
        INFO("coverage", "all required controls have produced events at %ld ms; keep going or hold L1+R1+Options to finish", el);
      }
    }
    if (ren) draw_frame(ren, in_tone, window_left, o.window_ms, all_seen);
    SDL_Delay(ren ? 1 : 8); /* with vsync the present call already paces the loop */
  }

  /* ---- finish ---- */
  g_tone_on = 0;
  stage("finish");
  log_rest("rest.after");
  if (adev) {
    SDL_Delay(60); /* let the 20 ms fade-out reach the device before closing */
    SDL_PauseAudioDevice(adev, 1);
    SDL_CloseAudioDevice(adev);
    adev = 0;
    if (in_tone) FAIL("audio.tone", "run ended during the tone phase");
  }
  report_coverage(&o);

done:
  stage("shutdown");
  if (ren) SDL_DestroyRenderer(ren);
  if (win) SDL_DestroyWindow(win);
  if (sdl_up) SDL_Quit();
  INFO("shutdown", "SDL_Quit returned");
  g_main_done = 1;
  if (wd_up) pthread_join(wd, NULL);
  say("KSRE_PS5_02C_END fail=%d", g_fail);
  {
    char msg[96];
    snprintf(msg, sizeof msg, "ksre-ps5 02c: finished (%s)", g_fail ? "FAILURES" : "ok");
    notify(msg);
  }
  if (g_logfile) fclose(g_logfile);
  g_logfile = NULL;
  if (!o.normal_exit) _exit(g_fail ? 1 : 0);
  return g_fail ? 1 : 0;
}

int main(int argc, char **argv) { return app_main(argc, argv); }
