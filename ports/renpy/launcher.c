// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ksre-ps5 Ren'Py launcher (milestone 04a).
 *
 * Starts Ren'Py 8.5.3 on the static CPython 3.12.8 build: registers Ren'Py's built-in
 * native modules (librenpy.a, generated inittab), configures an isolated interpreter
 * whose module path is the stdlib zip + Ren'Py's pure-Python tree, sets the environment
 * Ren'Py reads, and runs renpy.py as __main__ with the game directory as its argument
 * (the equivalent of `python renpy.py GAMEDIR`).
 *
 * Console layout (all under --root, default /data/ksre-ps5):
 *   python/lib/python312.zip  stdlib (ports/cpython)
 *   renpy/                    Ren'Py base: renpy.py, renpy/, lib/python/ (six, ecdsa)
 *   testgame/                 default game base directory (contains game/)
 *   saves/  logs/  tmp/       created on start; RENPY_PATH_TO_SAVES, RENPY_LOG_BASE, TMPDIR
 *
 * Usage: ksre-ps5-renpy [--root DIR] [--python-home DIR] [--renpy-base DIR] [--game DIR]
 *                       [--renderer NAME] [--env KEY=VALUE]... [--max-seconds N]
 *                       [--main-thread | --python-thread] [--verbose] [--] [Ren'Py arguments...]
 * Loaders pass no arguments: then they are read (whitespace-separated) from
 * <root>/renpy-args.txt if it exists.
 *
 * Project rules carried over from experiments 02b/03: Python runs on a 16 MiB thread
 * (payload threads default to 64 KiB, main thread RLIMIT_STACK 2 MiB; macOS builds use the
 * main thread instead, because Cocoa only creates windows there), MXCSR FTZ/DAZ is
 * cleared on the threads started here, SystemExit is handled here instead of letting
 * CPython call exit(), and the process ends with flush + _exit() (R11). stdout and stderr
 * are copied to <root>/logs/launcher-last.log so a run leaves a log on the console even
 * when the loader's socket is gone. The last line is always `KSRE_PS5_RENPY_END rc=N`.
 * Builds for the host too (ports/renpy/build.sh host).
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif
#if defined(__FreeBSD__)
#include <ucontext.h>
#endif

#ifndef KSRE_ROOT
#define KSRE_ROOT "/data/ksre-ps5"
#endif
#define PY_THREAD_STACK (16u * 1024u * 1024u)
#define FINALIZE_LIMIT_S 10
#define MAX_ENV 32
#define MAX_PATH_LEN 1024

int init_librenpy(void);            /* generated inittab (build.sh) */
extern int ksre_renpy_module_count; /* same file */
extern const char *const ksre_imports[]; /* generated imports.c: the ELF's undefined symbols */
extern const int ksre_import_count;
int main(int argc, char **argv);

struct job {
    char root[MAX_PATH_LEN];
    char python_home[MAX_PATH_LEN];
    char renpy_base[MAX_PATH_LEN];
    char game[MAX_PATH_LEN];
    const char *renderer;
    const char *env[MAX_ENV];
    int n_env;
    double max_seconds;
    int verbose;
    int main_thread; /* run Python on the main thread instead of a 16 MiB thread */
    int argc; /* Ren'Py arguments after GAMEDIR */
    char **argv;
    int rc;
};

/* Shared between the interpreter thread and main. */
enum { RUNNING, FINALIZING, FINISHED };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_state = RUNNING;

static double g_t0;
static int g_out_fd = 1; /* the loader's stdout (before the tee) */
static int g_err_fd = 2;
static int g_log_fd = -1;
static pthread_t g_tee;
static int g_tee_on;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* Raw output to the loader and the log file, bypassing stdio and the tee pipe. */
static void raw_out(const char *p, size_t n)
{
    write_all(g_out_fd, p, n);
    if (g_log_fd >= 0)
        write_all(g_log_fd, p, n);
}

/* --- stdout/stderr tee to the log file ------------------------------------------------ */
static void *tee_main(void *arg)
{
    int rfd = (int)(intptr_t)arg;
    char buf[4096];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        raw_out(buf, (size_t)n);
    }
    close(rfd);
    return NULL;
}

static void start_tee(const char *logdir)
{
    char path[MAX_PATH_LEN], prev[MAX_PATH_LEN];
    snprintf(path, sizeof path, "%s/launcher-last.log", logdir);
    snprintf(prev, sizeof prev, "%s/launcher-prev.log", logdir);
    rename(path, prev); /* keep one previous run */
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_log_fd < 0) {
        printf("RENPY WARN cannot open %s: %s (no log file)\n", path, strerror(errno));
        return;
    }
    int p[2];
    fflush(stdout);
    fflush(stderr);
    g_out_fd = dup(1);
    g_err_fd = dup(2);
    if (g_out_fd < 0 || g_err_fd < 0 || pipe(p) != 0) {
        printf("RENPY WARN tee setup failed: %s (log file only gets launcher lines)\n", strerror(errno));
        g_out_fd = g_out_fd < 0 ? 1 : g_out_fd;
        return;
    }
    dup2(p[1], 1);
    dup2(p[1], 2);
    close(p[1]);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&g_tee, &attr, tee_main, (void *)(intptr_t)p[0]) == 0) {
        g_tee_on = 1;
    } else { /* undo: write straight to the loader again */
        dup2(g_out_fd, 1);
        dup2(g_err_fd, 2);
        close(p[0]);
    }
    pthread_attr_destroy(&attr);
    printf("RENPY log %s\n", path);
}

/* Restores fds 1/2 (closing the pipe's write side) and waits for the tee to drain. */
static void stop_tee(void)
{
    fflush(stdout);
    fflush(stderr);
    if (!g_tee_on)
        return;
    dup2(g_out_fd, 1);
    dup2(g_err_fd, 2);
    pthread_join(g_tee, NULL);
    g_tee_on = 0;
}

/* --- fault reporter (async-signal-safe: write() only) --------------------------------- */
static void put_str(char *buf, size_t *n, const char *s)
{
    while (*s && *n < 255)
        buf[(*n)++] = *s++;
}

static void put_hex(char *buf, size_t *n, uintptr_t v)
{
    char tmp[16];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && i < 16);
    put_str(buf, n, "0x");
    while (i > 0 && *n < 255)
        buf[(*n)++] = tmp[--i];
}

static void fault_handler(int sig, siginfo_t *si, void *uctx)
{
    char buf[256];
    size_t n = 0;
    uintptr_t rip = 0, rsp = 0;
#if defined(__FreeBSD__) && defined(__x86_64__)
    const ucontext_t *uc = uctx;
    rip = (uintptr_t)uc->uc_mcontext.mc_rip;
    rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
#else
    (void)uctx;
#endif
    put_str(buf, &n, "\nRENPY FAULT sig=");
    put_hex(buf, &n, (uintptr_t)sig);
    put_str(buf, &n, " addr=");
    put_hex(buf, &n, (uintptr_t)(si ? si->si_addr : 0));
    put_str(buf, &n, " rip=");
    put_hex(buf, &n, rip);
    put_str(buf, &n, " rsp=");
    put_hex(buf, &n, rsp);
    put_str(buf, &n, " main=");
    put_hex(buf, &n, (uintptr_t)&main);
    put_str(buf, &n, "\nKSRE_PS5_RENPY_END rc=134\n");
    raw_out(buf, n);
    _exit(134);
}

static void install_fault_reporter(void)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGSYS, SIGABRT, SIGTRAP};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* a closed loader socket must not kill the game */
}

/* Per-thread alternate stack so a stack overflow is reported too. */
static void install_altstack(void)
{
    static char alt[2][64 * 1024];
    static int used;
    if (used >= 2)
        return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt[used++];
    ss.ss_size = sizeof alt[0];
    sigaltstack(&ss, NULL);
}

/* MXCSR is per thread (R2b): clear flush-to-zero / denormals-are-zero. */
static void clear_ftz_daz(void)
{
#if defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() & ~(0x8000u | 0x0040u));
#endif
}

/* --- helpers -------------------------------------------------------------------------- */
static void mkdir_p(const char *path)
{
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, len = 0, n;
    char *buf = malloc(cap + 1);
    while (buf && (n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            char *bigger = realloc(buf, 2 * cap + 1);
            if (!bigger) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf)
        buf[len] = '\0';
    return buf;
}

static void set_default_env(const char *key, const char *value)
{
    setenv(key, value, 0); /* never override --env or an inherited value */
    const char *v = getenv(key);
    printf("RENPY env %s=%s\n", key, v ? v : "(unset)");
}

/* The SDK's link stubs list functions that the console's libraries do not always export
 * (03b: clock_nanosleep). Every symbol this ELF imports is looked up once at start, so a
 * missing one is named in the log instead of crashing when it is first called. */
static void check_imports(void)
{
    int missing = 0;
    for (int i = 0; i < ksre_import_count; i++) {
        if (dlsym(RTLD_DEFAULT, ksre_imports[i]))
            continue;
        if (missing++ < 64)
            printf("RENPY import-missing %s\n", ksre_imports[i]);
    }
    printf("RENPY imports checked=%d missing=%d\n", ksre_import_count, missing);
}

/* --- Python --------------------------------------------------------------------------- */
static int status_failed(PyStatus st, const char *what)
{
    if (!PyStatus_Exception(st))
        return 0;
    fprintf(stderr, "RENPY FATAL %s: %s%s%s\n", what, st.func ? st.func : "", st.func ? ": " : "",
            st.err_msg ? st.err_msg : "(no message)");
    return 1;
}

static int append_path(PyConfig *cfg, const char *dir, const char *suffix)
{
    char buf[MAX_PATH_LEN];
    snprintf(buf, sizeof buf, "%s%s", dir, suffix);
    wchar_t *w = Py_DecodeLocale(buf, NULL); /* UTF-8 (pre-initialized in UTF-8 mode) */
    if (!w)
        return -1;
    PyStatus st = PyWideStringList_Append(&cfg->module_search_paths, w);
    PyMem_RawFree(w);
    return status_failed(st, "module_search_paths") ? -1 : 0;
}

/* Exit status for the pending exception; SystemExit never reaches PyErr_Print (which
 * would call exit() from this thread). */
static int status_from_exception(void)
{
    if (!PyErr_ExceptionMatches(PyExc_SystemExit)) {
        PyErr_Print();
        return 1;
    }
    PyObject *exc = PyErr_GetRaisedException();
    PyObject *code = PyObject_GetAttrString(exc, "code");
    int rc = 1;
    if (!code) {
        PyErr_Clear();
    } else if (code == Py_None) {
        rc = 0;
    } else if (PyLong_Check(code)) {
        rc = (int)PyLong_AsLong(code);
        if (rc == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            rc = 1;
        }
    } else {
        PyObject_Print(code, stderr, Py_PRINT_RAW);
        fputc('\n', stderr);
    }
    Py_XDECREF(code);
    Py_DECREF(exc);
    return rc;
}

/* Runs `path` as __main__ with __file__ set, like `python path`. */
static int run_main_file(const char *path)
{
    char *src = read_file(path);
    if (!src) {
        fprintf(stderr, "RENPY FATAL cannot read %s: %s\n", path, strerror(errno));
        return 124;
    }
    PyObject *mod = PyImport_AddModule("__main__"); /* borrowed */
    PyObject *globals = mod ? PyModule_GetDict(mod) : NULL;
    PyObject *file = PyUnicode_DecodeFSDefault(path);
    PyObject *code = NULL, *res = NULL;
    int rc = 0;
    if (!globals || !file || PyDict_SetItemString(globals, "__file__", file) < 0 ||
        !(code = Py_CompileString(src, path, Py_file_input)) ||
        !(res = PyEval_EvalCode(code, globals, globals)))
        rc = status_from_exception();
    Py_XDECREF(res);
    Py_XDECREF(code);
    Py_XDECREF(file);
    free(src);
    return rc;
}

static int run_python(struct job *job)
{
    PyStatus st;
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    st = Py_PreInitialize(&pre);
    if (status_failed(st, "Py_PreInitialize"))
        return 120;
    if (init_librenpy() != 0) {
        fprintf(stderr, "RENPY FATAL PyImport_ExtendInittab failed\n");
        return 120;
    }
    printf("RENPY step inittab modules=%d\n", ksre_renpy_module_count);

    char script[MAX_PATH_LEN];
    snprintf(script, sizeof script, "%s/renpy.py", job->renpy_base);
    /* sys.argv = [renpy.py, GAMEDIR, Ren'Py arguments...] */
    char **argv = calloc((size_t)job->argc + 3, sizeof *argv);
    if (!argv)
        return 121;
    argv[0] = script;
    argv[1] = job->game;
    for (int i = 0; i < job->argc; i++)
        argv[i + 2] = job->argv[i];

    PyConfig cfg;
    PyConfig_InitIsolatedConfig(&cfg);
    cfg.buffered_stdio = 0;
    cfg.write_bytecode = 0; /* .pyc files are compiled on the build machine */
    cfg.faulthandler = 1;
    cfg.verbose = job->verbose;
    cfg.module_search_paths_set = 1;
    int bad = 0;
    bad |= status_failed(PyConfig_SetBytesString(&cfg, &cfg.home, job->python_home), "home");
    bad |= status_failed(PyConfig_SetBytesString(&cfg, &cfg.program_name, "ksre-ps5-renpy"), "program_name");
    bad |= status_failed(PyConfig_SetBytesArgv(&cfg, job->argc + 2, argv), "argv");
    bad |= append_path(&cfg, job->python_home, "/lib/python312.zip") != 0;
    bad |= append_path(&cfg, job->python_home, "/lib/python3.12") != 0;
    bad |= append_path(&cfg, job->python_home, "/lib/python3.12/lib-dynload") != 0; /* host only */
    bad |= append_path(&cfg, job->renpy_base, "/lib/python") != 0;
    bad |= append_path(&cfg, job->renpy_base, "") != 0;
    free(argv);
    if (bad) {
        PyConfig_Clear(&cfg);
        return 121;
    }

    double t_init = mono_now();
    st = Py_InitializeFromConfig(&cfg);
    PyConfig_Clear(&cfg);
    if (status_failed(st, "Py_InitializeFromConfig"))
        return PyStatus_IsExit(st) ? st.exitcode : 122;
    printf("RENPY step python-ready t=%.3fs init=%.3fs\n", mono_now() - g_t0, mono_now() - t_init);
    printf("RENPY step run %s %s\n", script, job->game);

    int rc = run_main_file(script);
    printf("RENPY step returned rc=%d t=%.3fs\n", rc, mono_now() - g_t0);

    pthread_mutex_lock(&g_mu);
    g_state = FINALIZING;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    if (Py_FinalizeEx() < 0 && rc == 0)
        rc = 120;
    return rc;
}

static void *py_thread(void *arg)
{
    struct job *job = arg;
    install_altstack();
    clear_ftz_daz();
    job->rc = run_python(job);
    pthread_mutex_lock(&g_mu);
    g_state = FINISHED;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* Waits for the interpreter thread. Returns 0 when it finished, or an rc when a limit hit:
 * --max-seconds for the whole run, FINALIZE_LIMIT_S for Py_FinalizeEx (Ren'Py's
 * non-daemon threads must not keep the payload alive). */
static int wait_for_python(const struct job *job)
{
    pthread_mutex_lock(&g_mu);
    double finalize_start = 0;
    int rc = 0;
    while (g_state != FINISHED) {
        double now = mono_now(), deadline = 0;
        if (g_state == FINALIZING) {
            if (finalize_start == 0)
                finalize_start = now;
            deadline = finalize_start + FINALIZE_LIMIT_S;
        }
        if (job->max_seconds > 0 && (deadline == 0 || g_t0 + job->max_seconds < deadline))
            deadline = g_t0 + job->max_seconds;
        if (deadline == 0) {
            pthread_cond_wait(&g_cv, &g_mu);
            continue;
        }
        if (now >= deadline) {
            rc = g_state == FINALIZING ? 125 : 126;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts); /* condvars default to CLOCK_REALTIME */
        double wait = deadline - now;
        if (wait > 1.0)
            wait = 1.0; /* re-check the state at least once a second */
        ts.tv_sec += (time_t)wait;
        ts.tv_nsec += (long)((wait - (double)(time_t)wait) * 1e9);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    if (rc == 125)
        printf("RENPY WARN Py_FinalizeEx did not return within %d s; exiting anyway\n", FINALIZE_LIMIT_S);
    if (rc == 126)
        printf("RENPY WARN --max-seconds %.0f reached; stopping the payload\n", job->max_seconds);
    return rc;
}

/* Last output and exit; also called from the watchdog thread. */
_Noreturn static void finish(int rc)
{
    printf("RENPY step end t=%.3fs\n", mono_now() - g_t0);
    stop_tee();
    char end[64];
    int n = snprintf(end, sizeof end, "KSRE_PS5_RENPY_END rc=%d\n", rc);
    raw_out(end, (size_t)n);
    if (g_log_fd >= 0)
        fsync(g_log_fd);
    _exit(rc);
}

/* With --main-thread, the limits are enforced from this thread. */
static void *watchdog_main(void *arg)
{
    int rc = wait_for_python(arg);
    if (rc != 0)
        finish(rc);
    return NULL;
}

/* --- arguments ------------------------------------------------------------------------ */
static int args_from_file(const char *root, char ***argvp)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof path, "%s/renpy-args.txt", root);
    char *text = read_file(path); /* kept for the lifetime of the process */
    if (!text)
        return 0;
    static char *av[64];
    int ac = 1;
    av[0] = (*argvp)[0];
    for (char *tok = strtok(text, " \t\r\n"); tok && ac < 63; tok = strtok(NULL, " \t\r\n"))
        av[ac++] = tok;
    av[ac] = NULL;
    printf("RENPY args from %s (%d)\n", path, ac - 1);
    *argvp = av;
    return ac;
}

static void usage(void)
{
    printf("usage: ksre-ps5-renpy [--root DIR] [--python-home DIR] [--renpy-base DIR] [--game DIR]\n"
           "                      [--renderer NAME] [--env KEY=VALUE]... [--max-seconds N]\n"
           "                      [--main-thread | --python-thread] [--verbose] [--] [Ren'Py arguments...]\n");
}

static int takes_value(const char *a)
{
    static const char *opts[] = {"--root", "--python-home", "--renpy-base", "--game",
                                 "--renderer", "--env", "--max-seconds"};
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++)
        if (!strcmp(a, opts[i]))
            return 1;
    return 0;
}

/* Returns the index of the first Ren'Py argument, or -1 on a usage error. */
static int parse_args(struct job *job, int argc, char **argv)
{
    const char *home = NULL, *base = NULL, *game = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--"))
            return i + 1;
        if (!strcmp(a, "--help"))
            return -1;
        if (!strcmp(a, "--verbose")) {
            job->verbose = 1;
            continue;
        }
        if (!strcmp(a, "--main-thread") || !strcmp(a, "--python-thread")) {
            job->main_thread = !strcmp(a, "--main-thread");
            continue;
        }
        if (!takes_value(a))
            break; /* first Ren'Py argument */
        if (i + 1 >= argc)
            return -1;
        const char *v = argv[++i];
        if (!strcmp(a, "--root"))
            snprintf(job->root, sizeof job->root, "%s", v);
        else if (!strcmp(a, "--python-home"))
            home = v;
        else if (!strcmp(a, "--renpy-base"))
            base = v;
        else if (!strcmp(a, "--game"))
            game = v;
        else if (!strcmp(a, "--renderer"))
            job->renderer = v;
        else if (!strcmp(a, "--max-seconds"))
            job->max_seconds = atof(v);
        else if (!strchr(v, '=') || job->n_env >= MAX_ENV) /* --env */
            return -1;
        else
            job->env[job->n_env++] = v;
    }
    snprintf(job->python_home, sizeof job->python_home, "%s/python", job->root);
    snprintf(job->renpy_base, sizeof job->renpy_base, "%s/renpy", job->root);
    snprintf(job->game, sizeof job->game, "%s/testgame", job->root);
    if (home)
        snprintf(job->python_home, sizeof job->python_home, "%s", home);
    if (base)
        snprintf(job->renpy_base, sizeof job->renpy_base, "%s", base);
    if (game)
        snprintf(job->game, sizeof job->game, "%s", game);
    return i;
}

int main(int argc, char **argv)
{
    g_t0 = mono_now();
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_fault_reporter();
    install_altstack();
    clear_ftz_daz();

    static struct job job;
    snprintf(job.root, sizeof job.root, "%s", KSRE_ROOT);
    job.renderer = "gl2"; /* 02b decision: OSMesa/llvmpipe, desktop GL */
#if defined(__APPLE__)
    job.main_thread = 1; /* Cocoa: windows only on the main thread */
#endif
    if (argc <= 1) {
        int n = args_from_file(job.root, &argv);
        if (n)
            argc = n;
    }
    int first = parse_args(&job, argc, argv);
    if (first < 0) {
        usage();
        printf("KSRE_PS5_RENPY_END rc=2\n");
        fflush(stdout);
        _exit(2);
    }
    job.argc = argc - first;
    job.argv = argv + first;

    char dir[MAX_PATH_LEN];
    const char *subdirs[] = {"logs", "saves", "tmp"};
    for (size_t i = 0; i < sizeof subdirs / sizeof subdirs[0]; i++) {
        snprintf(dir, sizeof dir, "%s/%s", job.root, subdirs[i]);
        mkdir_p(dir);
    }
    snprintf(dir, sizeof dir, "%s/logs", job.root);
    start_tee(dir);

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    printf("RENPY start unix=%lld root=%s game=%s\n", (long long)wall.tv_sec, job.root, job.game);
    printf("RENPY paths python-home=%s renpy-base=%s\n", job.python_home, job.renpy_base);

    /* --env first, then defaults that leave existing values alone. */
    for (int i = 0; i < job.n_env; i++) {
        char *kv = strdup(job.env[i]);
        char *eq = kv ? strchr(kv, '=') : NULL;
        if (eq) {
            *eq = '\0';
            setenv(kv, eq + 1, 1);
        }
        free(kv);
    }
    char buf[MAX_PATH_LEN];
    snprintf(buf, sizeof buf, "%s/saves", job.root);
    set_default_env("RENPY_PATH_TO_SAVES", buf); /* saves -> <root>/saves/<save_directory> */
    snprintf(buf, sizeof buf, "%s/logs", job.root);
    set_default_env("RENPY_LOG_BASE", buf); /* log.txt, errors.txt, traceback.txt */
    set_default_env("RENPY_RENDERER", job.renderer);
    set_default_env("RENPY_PLATFORM", "ps5-x86_64"); /* renpy.arch = x86_64 */
    set_default_env("HOME", job.root);
    snprintf(buf, sizeof buf, "%s/tmp", job.root);
    set_default_env("TMPDIR", buf);
    /* SDL hints read from the environment (SDL_hints.h names without the HINT_ part). */
    set_default_env("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");

    check_imports();
    if (chdir(job.renpy_base) != 0)
        printf("RENPY WARN chdir(%s): %s\n", job.renpy_base, strerror(errno));

    printf("RENPY python on the %s\n", job.main_thread ? "main thread" : "interpreter thread (16 MiB)");
    pthread_attr_t attr;
    pthread_t tid;
    int err = pthread_attr_init(&attr);
    if (job.main_thread) {
        if (!err)
            err = pthread_attr_setstacksize(&attr, 256 * 1024);
        if (!err)
            err = pthread_create(&tid, &attr, watchdog_main, &job);
        if (err)
            printf("RENPY WARN no watchdog thread (error %d): limits are off\n", err);
        py_thread(&job);
        finish(job.rc);
    }
    if (!err)
        err = pthread_attr_setstacksize(&attr, PY_THREAD_STACK);
    if (!err)
        err = pthread_create(&tid, &attr, py_thread, &job);
    if (err) {
        fprintf(stderr, "RENPY FATAL interpreter thread: error %d\n", err);
        finish(123);
    }
    int rc = wait_for_python(&job);
    if (rc == 0) {
        pthread_join(tid, NULL);
        rc = job.rc;
    }
    finish(rc);
}
