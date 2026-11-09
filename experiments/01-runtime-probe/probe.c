/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ksre-ps5 experiment 01: runtime capability probe.
 *
 * A single self-contained payload that answers, with evidence from the
 * actual runtime, the questions that decide whether CPython 3.12 and Ren'Py
 * can run as a PS5 payload: threads, thread-local storage (emulated TLS on
 * the PS5 SDK), clocks, signals, executable memory, dynamic loading, writable
 * and persistent storage, locale/UTF-8, IPC/sockets/kqueue (asyncio's
 * selectors), entropy, floating point formatting, and memory headroom.
 *
 * The same source builds for the host (Linux/macOS) so the probe logic is
 * tested in CI; host results are NOT evidence about the PS5.
 *
 * Output: one line per check, written to stdout (streamed back to the host
 * by ps5-payload-elfldr) and to a log file under the probe directory:
 *
 *   PROBE <STATUS> <check-id> :: <detail>
 *
 * STATUS is PASS, FAIL, WARN, SKIP or INFO. The run is bracketed by
 * KSRE_PS5_PROBE_BEGIN and KSRE_PS5_PROBE_END lines. Risky checks run under a
 * SIGSEGV/SIGBUS/SIGILL guard so a fault is reported instead of killing the
 * payload. The memory-headroom check runs last because the system may
 * terminate the process instead of failing an allocation.
 *
 * Privacy: the probe never prints serial numbers, account IDs, network
 * addresses or directory listings outside its own directory.
 */

#define _GNU_SOURCE 1

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/sysctl.h>
#define PROBE_HAVE_KQUEUE 1
#define PROBE_HAVE_SYSCTLBYNAME 1
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define PROBE_VERSION "1"

#if defined(__PROSPERO__)
#define PROBE_TARGET "ps5"
#define PROBE_DEFAULT_DIR "/data/ksre-ps5"
#ifndef PROBE_MEM_CAP_MB
#define PROBE_MEM_CAP_MB 2048
#endif
#else
#define PROBE_TARGET "host"
#define PROBE_DEFAULT_DIR "probe-data"
#ifndef PROBE_MEM_CAP_MB
#define PROBE_MEM_CAP_MB 512
#endif
#endif

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

static FILE *g_log;
static int g_pass, g_fail, g_warn, g_skip, g_info;
static char g_dir[512];

static void sanitize(char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 || c == 0x7f) {
      *s = '?';
    }
  }
}

static void emit_line(const char *line) {
  fputs(line, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  if (g_log) {
    fputs(line, g_log);
    fputc('\n', g_log);
    fflush(g_log);
  }
}

static void emit(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  emit_line(buf);
}

static void report(const char *status, const char *id, const char *fmt, ...) {
  char detail[1536];
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof detail, fmt, ap);
  va_end(ap);
  sanitize(detail);
  snprintf(line, sizeof line, "PROBE %s %s :: %s", status, id, detail);
  emit_line(line);
  if (!strcmp(status, "PASS")) g_pass++;
  else if (!strcmp(status, "FAIL")) g_fail++;
  else if (!strcmp(status, "WARN")) g_warn++;
  else if (!strcmp(status, "SKIP")) g_skip++;
  else g_info++;
}

#define PASS(id, ...) report("PASS", id, __VA_ARGS__)
#define FAIL(id, ...) report("FAIL", id, __VA_ARGS__)
#define WARN(id, ...) report("WARN", id, __VA_ARGS__)
#define SKIP(id, ...) report("SKIP", id, __VA_ARGS__)
#define INFO(id, ...) report("INFO", id, __VA_ARGS__)

static double now_mono(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

/* ------------------------------------------------------------------------ */
/* Fault guard                                                              */
/* ------------------------------------------------------------------------ */

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guard_active;
static volatile sig_atomic_t g_caught_sig;

static void guard_handler(int sig) {
  if (g_guard_active) {
    g_caught_sig = sig;
    g_guard_active = 0;
    siglongjmp(g_jmp, 1);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static void install_guards(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = guard_handler;
  sigemptyset(&sa.sa_mask);
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
    if (sigaction(sigs[i], &sa, NULL) != 0) {
      WARN("guard.install", "sigaction(%d) failed: %s", sigs[i], strerror(errno));
    }
  }
}

/* Runs fn(arg). Returns 0 on normal return, or the caught signal number. */
static int run_guarded(void (*fn)(void *), void *arg) {
  g_caught_sig = 0;
  if (sigsetjmp(g_jmp, 1) == 0) {
    g_guard_active = 1;
    fn(arg);
    g_guard_active = 0;
    return 0;
  }
  return g_caught_sig ? (int)g_caught_sig : -1;
}

/* ------------------------------------------------------------------------ */
/* Platform information                                                     */
/* ------------------------------------------------------------------------ */

struct sw_version_buf {
  uint64_t size;
  char str[0x1c];
  uint32_t version;
  uint8_t slack[0xd8]; /* generous slack in case the real struct is larger */
};

#if defined(__PROSPERO__)
struct call_ctx {
  void *fn;
  void *buf;
  int rc;
};

static void call_sw_version(void *p) {
  struct call_ctx *c = p;
  c->rc = ((int (*)(void *))c->fn)(c->buf);
}

static void call_model_name(void *p) {
  struct call_ctx *c = p;
  c->rc = ((int (*)(char *))c->fn)((char *)c->buf);
}
#endif

static void probe_platform(int argc, char **argv) {
  struct utsname u;
  if (uname(&u) == 0) {
    INFO("platform.uname", "sysname=%s release=%s machine=%s", u.sysname, u.release, u.machine);
    INFO("platform.uname.version", "%s", u.version);
  } else {
    WARN("platform.uname", "uname failed: %s", strerror(errno));
  }

  INFO("platform.target", "target=%s sizeof(void*)=%zu sizeof(long)=%zu sizeof(wchar_t)=%zu "
       "sizeof(long double)=%zu", PROBE_TARGET, sizeof(void *), sizeof(long), sizeof(wchar_t),
       sizeof(long double));
  INFO("platform.process", "pid=%ld ppid=%ld uid=%ld gid=%ld", (long)getpid(), (long)getppid(),
       (long)getuid(), (long)getgid());
  INFO("platform.argv", "argc=%d argv0=%s", argc, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");

  char cwd[512];
  INFO("platform.cwd", "%s", getcwd(cwd, sizeof cwd) ? cwd : strerror(errno));

  static const char *envs[] = {"HOME", "PATH", "LANG", "LC_ALL", "TMPDIR", "PWD"};
  for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++) {
    const char *v = getenv(envs[i]);
    INFO("platform.env", "%s=%s", envs[i], v ? v : "(unset)");
  }

  INFO("platform.sysconf", "nprocessors_onln=%ld pagesize=%ld", sysconf(_SC_NPROCESSORS_ONLN),
       sysconf(_SC_PAGESIZE));

  static const struct {
    int res;
    const char *name;
  } lims[] = {{RLIMIT_STACK, "stack"}, {RLIMIT_DATA, "data"}, {RLIMIT_NOFILE, "nofile"},
#ifdef RLIMIT_AS
              {RLIMIT_AS, "as"},
#endif
  };
  for (size_t i = 0; i < sizeof lims / sizeof lims[0]; i++) {
    struct rlimit rl;
    if (getrlimit(lims[i].res, &rl) == 0) {
      INFO("platform.rlimit", "%s cur=%lld max=%lld", lims[i].name, (long long)rl.rlim_cur,
           (long long)rl.rlim_max);
    } else {
      INFO("platform.rlimit", "%s getrlimit failed: %s", lims[i].name, strerror(errno));
    }
  }

#ifdef PROBE_HAVE_SYSCTLBYNAME
  static const char *str_mibs[] = {"kern.ostype", "kern.osrelease", "hw.model", "hw.machine"};
  for (size_t i = 0; i < sizeof str_mibs / sizeof str_mibs[0]; i++) {
    char buf[256] = {0};
    size_t len = sizeof buf - 1;
    if (sysctlbyname(str_mibs[i], buf, &len, NULL, 0) == 0) {
      INFO("platform.sysctl", "%s=%s", str_mibs[i], buf);
    } else {
      INFO("platform.sysctl", "%s unavailable: %s", str_mibs[i], strerror(errno));
    }
  }
  static const char *int_mibs[] = {"hw.ncpu", "hw.physmem", "hw.usermem", "kern.sdk_version"};
  for (size_t i = 0; i < sizeof int_mibs / sizeof int_mibs[0]; i++) {
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(int_mibs[i], &v, &len, NULL, 0) == 0) {
      if (len == 4) {
        v &= 0xffffffffu;
      }
      INFO("platform.sysctl", "%s=%llu (0x%llx, %zu bytes)", int_mibs[i], (unsigned long long)v,
           (unsigned long long)v, len);
    } else {
      INFO("platform.sysctl", "%s unavailable: %s", int_mibs[i], strerror(errno));
    }
  }
#endif

#if defined(__PROSPERO__)
  /* Optional SCE queries, resolved at run time so a missing export cannot
   * prevent the payload from loading. The version struct layout is the one
   * used by PS4/PS5 homebrew (size, string, packed version) and is treated
   * as unverified; output is sanitized and bounded. */
  void *fn = dlsym(RTLD_DEFAULT, "sceKernelGetProsperoSystemSwVersion");
  if (fn) {
    struct sw_version_buf v;
    memset(&v, 0, sizeof v);
    v.size = 0x28;
    struct call_ctx c = {fn, &v, -1};
    int sig = run_guarded(call_sw_version, &c);
    if (sig) {
      WARN("platform.firmware", "sceKernelGetProsperoSystemSwVersion faulted (signal %d)", sig);
    } else {
      v.str[sizeof v.str - 1] = '\0';
      INFO("platform.firmware", "rc=%d string=%s version=0x%08x (API layout unverified)", c.rc,
           v.str, v.version);
    }
  } else {
    INFO("platform.firmware", "sceKernelGetProsperoSystemSwVersion not resolvable: %s", dlerror());
  }

  fn = dlsym(RTLD_DEFAULT, "sceKernelGetHwModelName");
  if (fn) {
    char model[256];
    memset(model, 0, sizeof model);
    struct call_ctx c = {fn, model, -1};
    int sig = run_guarded(call_model_name, &c);
    model[sizeof model - 1] = '\0';
    if (sig) {
      WARN("platform.model", "sceKernelGetHwModelName faulted (signal %d)", sig);
    } else {
      INFO("platform.model", "rc=%d model=%s", c.rc, model);
    }
  } else {
    INFO("platform.model", "sceKernelGetHwModelName not resolvable (libkernel_sys only?)");
  }

  static const char *paths[] = {"/data", "/user/data", "/data/homebrew", "/user/homebrew",
                                "/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/system_ex/app"};
  for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    struct stat st;
    int rc = stat(paths[i], &st);
    INFO("platform.path", "%s %s", paths[i],
         rc == 0 ? (S_ISDIR(st.st_mode) ? "exists(dir)" : "exists") : strerror(errno));
  }
#endif
}

/* ------------------------------------------------------------------------ */
/* Clocks                                                                   */
/* ------------------------------------------------------------------------ */

static void probe_time(void) {
  double t0 = now_mono();
  sleep_ms(20);
  double dt = (now_mono() - t0) * 1000.0;
  if (dt >= 15.0 && dt < 1000.0) {
    PASS("time.monotonic_sleep", "nanosleep(20ms) measured %.2f ms", dt);
  } else {
    FAIL("time.monotonic_sleep", "nanosleep(20ms) measured %.2f ms", dt);
  }

  struct timespec res;
  if (clock_getres(CLOCK_MONOTONIC, &res) == 0) {
    INFO("time.monotonic_res", "%ld ns", (long)res.tv_nsec + (long)res.tv_sec * 1000000000L);
  }

  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  long skew = (long)tv.tv_sec - (long)rt.tv_sec;
  struct tm tm;
  char when[64] = "?";
  if (localtime_r(&rt.tv_sec, &tm)) {
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
  }
  if (rt.tv_sec > 1704067200L && skew >= -1 && skew <= 1) { /* after 2024-01-01 */
    PASS("time.realtime", "local=%s gettimeofday_skew=%lds", when, skew);
  } else {
    WARN("time.realtime", "unexpected realtime %lld (local=%s) skew=%lds", (long long)rt.tv_sec,
         when, skew);
  }
  tzset();
  INFO("time.tz", "tzname=%s/%s", tzname[0] ? tzname[0] : "?", tzname[1] ? tzname[1] : "?");
}

/* ------------------------------------------------------------------------ */
/* Threads and TLS                                                          */
/* ------------------------------------------------------------------------ */

#define N_THREADS 4
#define N_ITERS 200000

static __thread int tls_value = 7;
static atomic_long g_atomic_counter;
static long g_mutex_counter;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_key;
static atomic_int g_key_destructor_calls;

struct thread_arg {
  int id;
  int tls_ok;
  int tss_ok;
  uintptr_t tls_addr;
};

static void key_destructor(void *v) {
  (void)v;
  atomic_fetch_add(&g_key_destructor_calls, 1);
}

static void *thread_main(void *p) {
  struct thread_arg *a = p;
  int want = 1000 * (a->id + 1);
  int initial_ok = (tls_value == 7);
  tls_value = want;
  a->tls_addr = (uintptr_t)&tls_value;
  pthread_setspecific(g_key, (void *)(intptr_t)want);
  for (int i = 0; i < N_ITERS; i++) {
    atomic_fetch_add(&g_atomic_counter, 1);
    pthread_mutex_lock(&g_mutex);
    g_mutex_counter++;
    pthread_mutex_unlock(&g_mutex);
    if ((i & 0xffff) == 0) {
      sched_yield();
    }
  }
  sleep_ms(10);
  a->tls_ok = initial_ok && (tls_value == want);
  a->tss_ok = ((intptr_t)pthread_getspecific(g_key) == want);
  return NULL;
}

static void probe_threads(void) {
  pthread_t th[N_THREADS];
  struct thread_arg args[N_THREADS];
  int created = 0;

  if (pthread_key_create(&g_key, key_destructor) != 0) {
    FAIL("threads.tss_key", "pthread_key_create failed");
    return;
  }
  tls_value = -1;
  double t0 = now_mono();
  for (int i = 0; i < N_THREADS; i++) {
    memset(&args[i], 0, sizeof args[i]);
    args[i].id = i;
    int rc = pthread_create(&th[i], NULL, thread_main, &args[i]);
    if (rc != 0) {
      FAIL("threads.create", "pthread_create #%d failed: %s", i, strerror(rc));
      break;
    }
    created++;
  }
  for (int i = 0; i < created; i++) {
    pthread_join(th[i], NULL);
  }
  double dt = (now_mono() - t0) * 1000.0;
  if (created != N_THREADS) {
    return;
  }

  long expect = (long)N_THREADS * N_ITERS;
  long got_atomic = atomic_load(&g_atomic_counter);
  if (got_atomic == expect && g_mutex_counter == expect) {
    PASS("threads.atomic_mutex", "%d threads x %d iters ok in %.1f ms", N_THREADS, N_ITERS, dt);
  } else {
    FAIL("threads.atomic_mutex", "atomic=%ld mutex=%ld expected=%ld", got_atomic, g_mutex_counter,
         expect);
  }

  int tls_ok = (tls_value == -1);
  int distinct = 1;
  int tss_ok = 1;
  for (int i = 0; i < N_THREADS; i++) {
    tls_ok = tls_ok && args[i].tls_ok;
    tss_ok = tss_ok && args[i].tss_ok;
    for (int j = 0; j < i; j++) {
      if (args[i].tls_addr == args[j].tls_addr) {
        distinct = 0;
      }
    }
  }
  if (tls_ok && distinct) {
    PASS("threads.tls", "__thread values isolated per thread (main=%d, distinct addresses)",
         tls_value);
  } else {
    FAIL("threads.tls", "tls_ok=%d distinct_addresses=%d main=%d", tls_ok, distinct, tls_value);
  }

  int dcalls = atomic_load(&g_key_destructor_calls);
  if (tss_ok && dcalls == N_THREADS) {
    PASS("threads.tss", "pthread_key values isolated; destructor ran %d times", dcalls);
  } else if (tss_ok) {
    WARN("threads.tss", "values isolated but destructor ran %d/%d times", dcalls, N_THREADS);
  } else {
    FAIL("threads.tss", "pthread_getspecific mismatch (destructor calls %d)", dcalls);
  }

  /* Condition variable with a monotonic clock, as CPython prefers. */
  pthread_condattr_t ca;
  pthread_cond_t cond;
  int clk_rc = -1;
  pthread_condattr_init(&ca);
#if !defined(__APPLE__) /* macOS has no pthread_condattr_setclock */
  clk_rc = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&cond, &ca);
  struct timespec deadline;
  clock_gettime(clk_rc == 0 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += 50 * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  double c0 = now_mono();
  pthread_mutex_lock(&g_mutex);
  int wrc = pthread_cond_timedwait(&cond, &g_mutex, &deadline);
  pthread_mutex_unlock(&g_mutex);
  double cdt = (now_mono() - c0) * 1000.0;
  if (wrc == ETIMEDOUT && cdt >= 30.0 && cdt < 2000.0) {
    PASS("threads.cond_timedwait", "timed out after %.1f ms (setclock(MONOTONIC) rc=%d)", cdt,
         clk_rc);
  } else {
    FAIL("threads.cond_timedwait", "rc=%d after %.1f ms (setclock rc=%d)", wrc, cdt, clk_rc);
  }
  pthread_cond_destroy(&cond);
  pthread_condattr_destroy(&ca);

  /* Default and large thread stacks (CPython recursion depth depends on them). */
  pthread_attr_t attr;
  size_t defstack = 0;
  pthread_attr_init(&attr);
  pthread_attr_getstacksize(&attr, &defstack);
  INFO("threads.default_stack", "%zu KiB", defstack / 1024);
  pthread_attr_destroy(&attr);
}

#define BIG_STACK_BYTES (16u * 1024u * 1024u)
#define STACK_USE_BYTES (4u * 1024u * 1024u)

static int recurse(int depth) {
  volatile char pad[4096];
  pad[0] = (char)depth;
  pad[sizeof pad - 1] = (char)depth;
  if (depth <= 0) {
    return pad[0];
  }
  return recurse(depth - 1) + pad[sizeof pad - 1] - (char)depth + 1;
}

static void *big_stack_main(void *p) {
  int *out = p;
  *out = recurse((int)(STACK_USE_BYTES / 4096u));
  return NULL;
}

static void probe_big_stack(void) {
  pthread_attr_t attr;
  pthread_t th;
  int out = -12345;
  pthread_attr_init(&attr);
  int src = pthread_attr_setstacksize(&attr, BIG_STACK_BYTES);
  if (src != 0) {
    WARN("threads.big_stack", "pthread_attr_setstacksize(16 MiB) failed: %s", strerror(src));
    pthread_attr_destroy(&attr);
    return;
  }
  int rc = pthread_create(&th, &attr, big_stack_main, &out);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    FAIL("threads.big_stack", "pthread_create with 16 MiB stack failed: %s", strerror(rc));
    return;
  }
  pthread_join(th, NULL);
  if (out != -12345) {
    PASS("threads.big_stack", "used ~%u KiB of a 16 MiB thread stack", STACK_USE_BYTES / 1024u);
  } else {
    FAIL("threads.big_stack", "deep recursion thread did not complete");
  }
}

/* ------------------------------------------------------------------------ */
/* Signals                                                                  */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_usr1;

static void usr1_handler(int sig) {
  (void)sig;
  g_usr1 = 1;
}

static void probe_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = usr1_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) != 0) {
    FAIL("signals.sigaction", "sigaction(SIGUSR1) failed: %s", strerror(errno));
    return;
  }
  raise(SIGUSR1);
  if (g_usr1) {
    PASS("signals.raise", "SIGUSR1 handler ran");
  } else {
    FAIL("signals.raise", "SIGUSR1 handler did not run");
  }
  signal(SIGUSR1, SIG_DFL);
  /* Ignoring SIGPIPE is what CPython does at start-up. */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    WARN("signals.sigpipe", "signal(SIGPIPE, SIG_IGN) failed: %s", strerror(errno));
  } else {
    PASS("signals.sigpipe", "SIGPIPE ignored");
  }
}

/* ------------------------------------------------------------------------ */
/* Executable memory (JIT feasibility, e.g. Mesa llvmpipe)                  */
/* ------------------------------------------------------------------------ */

#if defined(__x86_64__)
static const unsigned char k_ret42[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3}; /* mov eax,42; ret */
#define PROBE_CAN_RUN_CODE 1
#elif defined(__aarch64__)
static const unsigned char k_ret42[] = {0x40, 0x05, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6};
#define PROBE_CAN_RUN_CODE 1
#endif

#ifdef PROBE_CAN_RUN_CODE
struct exec_ctx {
  void *code;
  int result;
};

static void call_code(void *p) {
  struct exec_ctx *e = p;
  e->result = ((int (*)(void))e->code)();
}

static void try_exec(const char *id, void *mem, size_t len) {
  __builtin___clear_cache((char *)mem, (char *)mem + len);
  struct exec_ctx e = {mem, -1};
  int sig = run_guarded(call_code, &e);
  if (sig) {
    FAIL(id, "executing generated code faulted (signal %d)", sig);
  } else if (e.result == 42) {
    PASS(id, "generated code executed and returned 42");
  } else {
    FAIL(id, "generated code returned %d", e.result);
  }
}
#endif

static void probe_exec_memory(void) {
#ifndef PROBE_CAN_RUN_CODE
  SKIP("jit.wx", "no code sample for this CPU");
#else
  size_t len = (size_t)sysconf(_SC_PAGESIZE);
  if (len == 0 || len > (1u << 20)) {
    len = 16384;
  }

  /* W^X: write RW pages, then flip to RX. */
  void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) {
    FAIL("jit.wx", "mmap(RW) failed: %s", strerror(errno));
  } else {
    memcpy(m, k_ret42, sizeof k_ret42);
    if (mprotect(m, len, PROT_READ | PROT_EXEC) != 0) {
      WARN("jit.wx", "mprotect(RX) refused: %s (W^X JIT unavailable)", strerror(errno));
    } else {
      try_exec("jit.wx", m, sizeof k_ret42);
    }
    munmap(m, len);
  }

  /* RWX mapping (some JITs want it; expected to be refused on hardened systems). */
  m = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) {
    INFO("jit.rwx", "mmap(RWX) refused: %s", strerror(errno));
  } else {
    memcpy(m, k_ret42, sizeof k_ret42);
    try_exec("jit.rwx", m, sizeof k_ret42);
    munmap(m, len);
  }
#endif
}

/* ------------------------------------------------------------------------ */
/* Dynamic loading                                                          */
/* ------------------------------------------------------------------------ */

#if defined(__PROSPERO__)
struct rand_ctx {
  int (*fn)(uint8_t *, size_t);
  uint8_t buf[16];
  int rc;
};

static void call_rand(void *p) {
  struct rand_ctx *r = p;
  r->rc = r->fn(r->buf, sizeof r->buf);
}
#endif

static void probe_dynamic_loading(void) {
  void *p = dlsym(RTLD_DEFAULT, "printf");
  if (p) {
    PASS("dl.dlsym_default", "dlsym(RTLD_DEFAULT, \"printf\") resolved");
  } else {
    FAIL("dl.dlsym_default", "dlsym(RTLD_DEFAULT, \"printf\") failed: %s", dlerror());
  }

  char missing[600];
  snprintf(missing, sizeof missing, "%s/does-not-exist.so", g_dir);
  void *h = dlopen(missing, RTLD_LAZY);
  if (!h) {
    const char *e = dlerror();
    PASS("dl.dlopen_missing", "missing library reported gracefully: %s", e ? e : "(no dlerror)");
  } else {
    FAIL("dl.dlopen_missing", "dlopen of a missing file returned a handle");
    dlclose(h);
  }

#if defined(__PROSPERO__)
  /* Same mechanism SDL uses to load Mesa at run time (dlopen + dlsym). */
  h = dlopen("libSceRandom.sprx", RTLD_LAZY);
  if (!h) {
    FAIL("dl.sprx", "dlopen(libSceRandom.sprx) failed: %s", dlerror());
  } else {
    struct rand_ctx r;
    memset(&r, 0, sizeof r);
    r.fn = (int (*)(uint8_t *, size_t))dlsym(h, "sceRandomGetRandomNumber");
    if (!r.fn) {
      FAIL("dl.sprx", "dlsym(sceRandomGetRandomNumber) failed: %s", dlerror());
    } else {
      int sig = run_guarded(call_rand, &r);
      int nonzero = 0;
      for (size_t i = 0; i < sizeof r.buf; i++) {
        nonzero |= r.buf[i];
      }
      if (sig) {
        FAIL("dl.sprx", "sceRandomGetRandomNumber faulted (signal %d)", sig);
      } else if (r.rc == 0 && nonzero) {
        PASS("dl.sprx", "loaded libSceRandom.sprx and called sceRandomGetRandomNumber");
      } else {
        FAIL("dl.sprx", "sceRandomGetRandomNumber rc=%d nonzero=%d", r.rc, nonzero);
      }
    }
  }
#else
  SKIP("dl.sprx", "PS5 system modules only exist on the console");
#endif

  /* Run-time resolvability of functions CPython's configure/posixmodule and
   * Ren'Py care about. "yes" means some loaded module exports the name. */
  static const char *syms[] = {
      "fork",           "execv",          "execve",        "posix_spawn",      "waitpid",
      "kill",           "pipe2",          "dup3",          "openat",           "fstatat",
      "utimensat",      "futimens",       "realpath",      "readlink",         "symlink",
      "mkfifo",         "getpwuid",       "getpwnam",      "getgrgid",         "getlogin",
      "openpty",        "ttyname",        "sigaltstack",   "setitimer",        "getrusage",
      "sem_open",       "sem_timedwait",  "sched_yield",   "kqueue",           "epoll_create1",
      "poll",           "socketpair",     "getaddrinfo",   "getnameinfo",      "if_nameindex",
      "getentropy",     "getrandom",      "arc4random_buf", "nl_langinfo",     "iconv_open",
      "wcsftime",       "mbrtowc",        "clock_nanosleep", "pthread_condattr_setclock",
      "pthread_getcpuclockid", "dlopen", "dladdr",       "backtrace",        "mremap",
      "madvise",        "posix_fadvise",  "fdatasync",     "truncate",         "statvfs",
      "getloadavg",     "uname",          "chroot",        "setsid",           "tcgetattr",
  };
  char line[1200];
  size_t off = 0;
  int yes = 0;
  line[0] = '\0';
  for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
    int ok = dlsym(RTLD_DEFAULT, syms[i]) != NULL;
    yes += ok;
    char entry[96];
    int n = snprintf(entry, sizeof entry, "%s=%s", syms[i], ok ? "y" : "n");
    if (n < 0) {
      continue;
    }
    if (off + (size_t)n + 2 > 900) { /* keep each output line short */
      INFO("dl.symbols", "%s", line);
      off = 0;
      line[0] = '\0';
    }
    off += (size_t)snprintf(line + off, sizeof line - off, "%s%s", off ? " " : "", entry);
  }
  if (off) {
    INFO("dl.symbols", "%s", line);
  }
  INFO("dl.symbols_summary", "%d/%zu names resolvable via dlsym(RTLD_DEFAULT)", yes,
       sizeof syms / sizeof syms[0]);
}

/* ------------------------------------------------------------------------ */
/* Storage                                                                  */
/* ------------------------------------------------------------------------ */

static int mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static void probe_storage(void) {
  char tmpdir[600], a[700], b[700], runs[700];
  snprintf(tmpdir, sizeof tmpdir, "%s/tmp", g_dir);
  snprintf(a, sizeof a, "%s/rw-test.bin", tmpdir);
  snprintf(b, sizeof b, "%s/rw-test-renamed.bin", tmpdir);
  snprintf(runs, sizeof runs, "%s/probe-runs.txt", g_dir);

  if (mkdir_p(tmpdir) != 0) {
    FAIL("storage.mkdir", "mkdir -p %s failed: %s", tmpdir, strerror(errno));
    return;
  }
  PASS("storage.mkdir", "%s", tmpdir);

  const size_t sz = 1u << 20;
  unsigned char *buf = malloc(sz);
  unsigned char *chk = malloc(sz);
  if (!buf || !chk) {
    FAIL("storage.rw", "malloc failed");
    free(buf);
    free(chk);
    return;
  }
  for (size_t i = 0; i < sz; i++) {
    buf[i] = (unsigned char)(i * 131u + 7u);
  }

  double t0 = now_mono();
  int fd = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  ssize_t w = fd >= 0 ? write(fd, buf, sz) : -1;
  int fs = fd >= 0 ? fsync(fd) : -1;
  if (fd >= 0) {
    close(fd);
  }
  fd = open(a, O_RDONLY);
  ssize_t r = fd >= 0 ? read(fd, chk, sz) : -1;
  if (fd >= 0) {
    close(fd);
  }
  double dt = (now_mono() - t0) * 1000.0;
  if (w == (ssize_t)sz && r == (ssize_t)sz && memcmp(buf, chk, sz) == 0) {
    PASS("storage.rw", "1 MiB write+fsync+read verified in %.1f ms (fsync rc=%d)", dt, fs);
  } else {
    FAIL("storage.rw", "write=%zd read=%zd fsync=%d errno=%s", w, r, fs, strerror(errno));
  }
  free(buf);
  free(chk);

  struct stat st;
  if (rename(a, b) == 0 && stat(b, &st) == 0 && st.st_size == (off_t)sz && unlink(b) == 0 &&
      stat(b, &st) != 0 && errno == ENOENT) {
    PASS("storage.rename_unlink", "rename, stat and unlink behave");
  } else {
    FAIL("storage.rename_unlink", "rename/stat/unlink sequence failed: %s", strerror(errno));
  }

  char tmpl[700];
  snprintf(tmpl, sizeof tmpl, "%s/mkstemp-XXXXXX", tmpdir);
  int tfd = mkstemp(tmpl);
  if (tfd >= 0) {
    close(tfd);
    unlink(tmpl);
    PASS("storage.mkstemp", "mkstemp works in probe tmp dir");
  } else {
    FAIL("storage.mkstemp", "mkstemp failed: %s", strerror(errno));
  }

  DIR *d = opendir(g_dir);
  if (d) {
    int n = 0;
    while (readdir(d)) {
      n++;
    }
    closedir(d);
    PASS("storage.readdir", "%d entries in probe dir", n);
  } else {
    FAIL("storage.readdir", "opendir failed: %s", strerror(errno));
  }

  struct statvfs sv;
  if (statvfs(g_dir, &sv) == 0) {
    unsigned long long freemib =
        (unsigned long long)sv.f_bavail * (unsigned long long)sv.f_frsize / (1024ull * 1024ull);
    INFO("storage.free", "%llu MiB available on the probe filesystem", freemib);
  } else {
    INFO("storage.free", "statvfs failed: %s", strerror(errno));
  }

  /* Persistence across runs: the counter survives payload exit and reboot
   * if the storage is persistent. */
  long prev = 0;
  FILE *f = fopen(runs, "r");
  if (f) {
    if (fscanf(f, "%ld", &prev) != 1) {
      prev = 0;
    }
    fclose(f);
  }
  f = fopen(runs, "w");
  if (f && fprintf(f, "%ld\n", prev + 1) > 0 && fclose(f) == 0) {
    INFO("storage.persistence", "this is run #%ld of the probe in %s (previous runs: %ld)",
         prev + 1, g_dir, prev);
  } else {
    FAIL("storage.persistence", "could not update %s: %s", runs, strerror(errno));
    if (f) {
      fclose(f);
    }
  }
}

/* ------------------------------------------------------------------------ */
/* Locale / text encoding                                                   */
/* ------------------------------------------------------------------------ */

static void probe_locale(void) {
  const char *l = setlocale(LC_CTYPE, "");
  INFO("locale.default", "setlocale(LC_CTYPE, \"\") -> %s", l ? l : "(null)");
  INFO("locale.codeset", "nl_langinfo(CODESET) -> %s", nl_langinfo(CODESET));
  l = setlocale(LC_CTYPE, "C.UTF-8");
  INFO("locale.c_utf8", "setlocale(LC_CTYPE, \"C.UTF-8\") -> %s", l ? l : "(null)");

  wchar_t wbuf[16];
  size_t n = mbstowcs(wbuf, "h\xc3\xa9llo", 16);
  if (n == 5) {
    PASS("locale.mbstowcs_utf8", "UTF-8 multibyte decoding works in current locale");
  } else {
    WARN("locale.mbstowcs_utf8",
         "mbstowcs returned %ld; CPython must run in UTF-8 mode (PYTHONUTF8=1)", (long)n);
  }
  setlocale(LC_ALL, "C");
}

/* ------------------------------------------------------------------------ */
/* IPC, sockets, event notification                                         */
/* ------------------------------------------------------------------------ */

static void probe_ipc(void) {
  int p[2];
  if (pipe(p) != 0) {
    FAIL("ipc.pipe", "pipe failed: %s", strerror(errno));
    return;
  }
  char c = 'x', got = 0;
  if (write(p[1], &c, 1) == 1) {
    struct pollfd pfd = {p[0], POLLIN, 0};
    int prc = poll(&pfd, 1, 1000);
    if (prc == 1 && (pfd.revents & POLLIN)) {
      PASS("ipc.poll", "poll reports readable pipe");
    } else {
      FAIL("ipc.poll", "poll rc=%d revents=0x%x", prc, pfd.revents);
    }
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(p[0], &rf);
    struct timeval tv = {1, 0};
    int src = select(p[0] + 1, &rf, NULL, NULL, &tv);
    if (src == 1 && FD_ISSET(p[0], &rf)) {
      PASS("ipc.select", "select reports readable pipe");
    } else {
      FAIL("ipc.select", "select rc=%d", src);
    }
#ifdef PROBE_HAVE_KQUEUE
    int kq = kqueue();
    if (kq < 0) {
      FAIL("ipc.kqueue", "kqueue failed: %s (Python selectors would fall back to poll)",
           strerror(errno));
    } else {
      struct kevent ev, out;
      EV_SET(&ev, p[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
      struct timespec ts = {1, 0};
      int krc = kevent(kq, &ev, 1, &out, 1, &ts);
      if (krc == 1 && (int)out.ident == p[0]) {
        PASS("ipc.kqueue", "kqueue/kevent EVFILT_READ works (asyncio default selector)");
      } else {
        FAIL("ipc.kqueue", "kevent rc=%d errno=%s", krc, strerror(errno));
      }
      close(kq);
    }
#else
    SKIP("ipc.kqueue", "kqueue not available on this host OS");
#endif
    if (read(p[0], &got, 1) != 1 || got != 'x') {
      FAIL("ipc.pipe", "pipe read mismatch");
    } else {
      PASS("ipc.pipe", "pipe round-trip");
    }
  } else {
    FAIL("ipc.pipe", "pipe write failed: %s", strerror(errno));
  }
  close(p[0]);
  close(p[1]);

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
    char out[5] = {0};
    if (write(sv[0], "ping", 4) == 4 && read(sv[1], out, 4) == 4 && !memcmp(out, "ping", 4)) {
      PASS("ipc.socketpair", "AF_UNIX socketpair round-trip (asyncio self-pipe)");
    } else {
      FAIL("ipc.socketpair", "socketpair round-trip failed: %s", strerror(errno));
    }
    close(sv[0]);
    close(sv[1]);
  } else {
    FAIL("ipc.socketpair", "socketpair(AF_UNIX) failed: %s", strerror(errno));
  }

  /* TCP over loopback only. */
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) {
    FAIL("net.tcp_loopback", "socket failed: %s", strerror(errno));
  } else {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    socklen_t slen = sizeof sa;
    int ok = bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 1) == 0 &&
             getsockname(ls, (struct sockaddr *)&sa, &slen) == 0;
    int cs = ok ? socket(AF_INET, SOCK_STREAM, 0) : -1;
    struct timeval to = {2, 0};
    if (cs >= 0) {
      setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    }
    ok = ok && cs >= 0 && connect(cs, (struct sockaddr *)&sa, sizeof sa) == 0;
    int as = ok ? accept(ls, NULL, NULL) : -1;
    char out[5] = {0};
    ok = ok && as >= 0 && send(as, "pong", 4, 0) == 4 && recv(cs, out, 4, 0) == 4 &&
         !memcmp(out, "pong", 4);
    if (ok) {
      PASS("net.tcp_loopback", "127.0.0.1 listen/connect/accept/send/recv");
    } else {
      FAIL("net.tcp_loopback", "loopback TCP sequence failed: %s", strerror(errno));
    }
    if (as >= 0) close(as);
    if (cs >= 0) close(cs);
    close(ls);
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  int grc = getaddrinfo("localhost", "80", &hints, &res);
  if (grc == 0 && res) {
    int count = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      count++;
    }
    INFO("net.getaddrinfo", "localhost resolved to %d address(es), first family=%d", count,
         res->ai_family);
    freeaddrinfo(res);
  } else {
    INFO("net.getaddrinfo", "getaddrinfo(localhost) failed: %d (%s)", grc, gai_strerror(grc));
  }
}

/* ------------------------------------------------------------------------ */
/* Entropy and floating point                                               */
/* ------------------------------------------------------------------------ */

static void probe_entropy(void) {
  unsigned char b[16] = {0};
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, b, sizeof b) : -1;
  if (fd >= 0) {
    close(fd);
  }
  if (n == (ssize_t)sizeof b) {
    PASS("entropy.dev_urandom", "read 16 bytes from /dev/urandom");
  } else {
    WARN("entropy.dev_urandom", "/dev/urandom unavailable (%s); CPython needs another source",
         strerror(errno));
  }
  arc4random_buf(b, sizeof b);
  PASS("entropy.arc4random_buf", "arc4random_buf returned");
  INFO("entropy.getentropy", "getentropy %s",
       dlsym(RTLD_DEFAULT, "getentropy") ? "resolvable" : "not resolvable");
}

static void probe_float(void) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.17g", 0.1);
  volatile double tiny_in = 4.9e-324;
  double sub = strtod("4.9e-324", NULL);
  double dmin = strtod("2.2250738585072014e-308", NULL);
  volatile double one = 1.0;
  int ok = !strcmp(buf, "0.10000000000000001") && sub > 0.0 && sub == tiny_in && dmin == DBL_MIN &&
           fma(2.0, 3.0, 4.0) == 10.0 && fabs(sin(one) - 0.8414709848078965) < 1e-15 &&
           isnan(NAN) && isinf(INFINITY) && pow(2.0, 10.0) == 1024.0;
  if (ok) {
    PASS("float.ieee754", "printf/strtod/libm round-trips and subnormals behave");
  } else {
    FAIL("float.ieee754", "printf(0.1)=%s subnormal=%g dbl_min_ok=%d fma=%g", buf, sub,
         dmin == DBL_MIN, fma(2.0, 3.0, 4.0));
  }
  INFO("float.env", "FLT_EVAL_METHOD=%d DBL_MANT_DIG=%d", (int)FLT_EVAL_METHOD, DBL_MANT_DIG);
}

/* ------------------------------------------------------------------------ */
/* Memory headroom (runs last)                                              */
/* ------------------------------------------------------------------------ */

static void probe_memory(void) {
  const size_t chunk = 128u * 1024u * 1024u;
  const int max_chunks = PROBE_MEM_CAP_MB / 128;
  void *chunks[64];
  int got = 0;
  double t0 = now_mono();
  for (; got < max_chunks && got < 64; got++) {
    void *m = malloc(chunk);
    if (!m) {
      break;
    }
    memset(m, 0x5a, chunk); /* touch every page */
    chunks[got] = m;
  }
  double dt = (now_mono() - t0) * 1000.0;
  for (int i = 0; i < got; i++) {
    free(chunks[i]);
  }
  if (got == max_chunks) {
    PASS("memory.headroom", "allocated and touched %d MiB (cap %d MiB) in %.0f ms", got * 128,
         PROBE_MEM_CAP_MB, dt);
  } else if (got * 128 >= 1024) {
    WARN("memory.headroom", "malloc stopped at %d MiB (cap %d MiB)", got * 128, PROBE_MEM_CAP_MB);
  } else {
    FAIL("memory.headroom", "malloc stopped at %d MiB (cap %d MiB)", got * 128, PROBE_MEM_CAP_MB);
  }
}

/* ------------------------------------------------------------------------ */
/* Notifications (PS5 only)                                                 */
/* ------------------------------------------------------------------------ */

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
static void notify(const char *msg) {
  (void)msg;
}
#endif

static void on_exit_handler(void) {
  emit("PROBE INFO exit.atexit :: atexit handler ran");
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *dir = getenv("KSRE_PS5_PROBE_DIR");
  snprintf(g_dir, sizeof g_dir, "%s", (dir && *dir) ? dir : PROBE_DEFAULT_DIR);

  char logdir[600], logpath[700];
  snprintf(logdir, sizeof logdir, "%s/logs", g_dir);
  int dir_ok = mkdir_p(logdir) == 0;
  if (dir_ok) {
    snprintf(logpath, sizeof logpath, "%s/probe-%lld-%ld.log", logdir, (long long)time(NULL),
             (long)getpid());
    g_log = fopen(logpath, "w");
  }

  notify("ksre-ps5 probe: started");
  emit("KSRE_PS5_PROBE_BEGIN version=%s target=%s", PROBE_VERSION, PROBE_TARGET);
  if (g_log) {
    INFO("log.file", "%s", logpath);
  } else {
    WARN("log.file", "could not open a log file under %s (%s); stdout only", logdir,
         strerror(errno));
  }
  atexit(on_exit_handler);
  install_guards();

  probe_platform(argc, argv);
  probe_time();
  probe_signals();
  probe_threads();
  probe_big_stack();
  probe_locale();
  probe_ipc();
  probe_entropy();
  probe_float();
  probe_dynamic_loading();
  probe_storage();
  probe_exec_memory();
  emit("KSRE_PS5_PROBE_CHECKPOINT before=memory pass=%d fail=%d warn=%d skip=%d info=%d", g_pass,
       g_fail, g_warn, g_skip, g_info);
  probe_memory();

  emit("KSRE_PS5_PROBE_END version=%s target=%s pass=%d fail=%d warn=%d skip=%d info=%d",
       PROBE_VERSION, PROBE_TARGET, g_pass, g_fail, g_warn, g_skip, g_info);

  char msg[160];
  snprintf(msg, sizeof msg, "ksre-ps5 probe: %d pass, %d fail, %d warn. Log in %s/logs", g_pass,
           g_fail, g_warn, g_dir);
  notify(msg);
  return g_fail ? 1 : 0;
}
