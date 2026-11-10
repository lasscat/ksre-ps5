// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Experiment 03: embed the static CPython 3.12.8 build (ports/cpython) in a PS5 payload
 * and run a small test script (hello.py, compiled in as hello_py.h).
 *
 * Python setup follows the plan in docs/ARCHITECTURE.md: isolated PyConfig, UTF-8 mode
 * (the console has no UTF-8 locale), home = /data/ksre-ps5/python with the stdlib in
 * lib/python312.zip, unbuffered stdio (stdout streams back through the loader socket),
 * faulthandler on. Project rules: clear MXCSR FTZ/DAZ (R2b), link with -rdynamic (R13),
 * end with flush + _exit() (R11).
 *
 * The interpreter runs on its own thread with a 16 MiB stack: payload threads default
 * to 64 KiB and the main thread has RLIMIT_STACK 2 MiB (01b probe), while CPython's
 * recursion limits assume a typical desktop stack.
 *
 * Usage: payload [--home DIR] [--script FILE] [--verbose] [args...]   (args go to sys.argv)
 * elfldr passes no arguments: without any, the payload reads them (whitespace-separated)
 * from <home>/../03/args.txt, i.e. /data/ksre-ps5/03/args.txt, if that file exists.
 * A fault (SIGSEGV/BUS/ILL/FPE/SYS/ABRT/TRAP) prints the signal, rip, fault address and
 * the load slide (rip - slide = address in the ELF for llvm-symbolizer) before _exit.
 * The same source builds for the host against build/host-python (make run-host).
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif
#if defined(__FreeBSD__)
#include <ucontext.h>
#endif

#include "hello_py.h" /* static const unsigned char hello_py[] (NUL-terminated) */

#ifndef KSRE_PY_HOME
#define KSRE_PY_HOME "/data/ksre-ps5/python"
#endif
#define PY_THREAD_STACK (16u * 1024u * 1024u)

struct py_job {
    const char *home;
    const char *script; /* NULL = embedded hello.py */
    int verbose;        /* PyConfig.verbose: trace imports on stderr */
    int argc;
    char **argv;
    int rc;
};

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double g_t0;
int main(int argc, char **argv);

/* --- fault reporter (async-signal-safe: write() only) --------------------------------- */
static void put_str(char *buf, size_t *n, const char *s)
{
    while (*s && *n < 255)
        buf[(*n)++] = *s++;
}

static void put_hex(char *buf, size_t *n, uintptr_t v)
{
    char tmp[2 + 16];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
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
    put_str(buf, &n, "\n03 FAULT sig=");
    put_hex(buf, &n, (uintptr_t)sig);
    put_str(buf, &n, " addr=");
    put_hex(buf, &n, (uintptr_t)(si ? si->si_addr : 0));
    put_str(buf, &n, " rip=");
    put_hex(buf, &n, rip);
    put_str(buf, &n, " rsp=");
    put_hex(buf, &n, rsp);
    put_str(buf, &n, " main=");
    put_hex(buf, &n, (uintptr_t)&main);
    put_str(buf, &n, "\n");
    (void)!write(1, buf, n);
#if defined(__FreeBSD__) && defined(__x86_64__)
    /* Raw ucontext words (layout check) and the words at rsp (return addresses). */
    const uintptr_t *w = uctx;
    for (int i = 0; i < 48; i += 8) {
        n = 0;
        put_str(buf, &n, "03 FAULT uc");
        for (int j = i; j < i + 8; j++) {
            put_str(buf, &n, " ");
            put_hex(buf, &n, w[j]);
        }
        put_str(buf, &n, "\n");
        (void)!write(1, buf, n);
    }
#endif
    (void)!write(1, "KSRE_PS5_03_END rc=134\n", 23);
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

/* MXCSR is per thread; call this on every thread that runs Python code we start. */
static void clear_ftz_daz(const char *who)
{
#if defined(__x86_64__)
    unsigned int before = _mm_getcsr();
    _mm_setcsr(before & ~(0x8000u /* FTZ */ | 0x0040u /* DAZ */));
    printf("03 mxcsr %s before=0x%04x after=0x%04x\n", who, before, _mm_getcsr());
#else
    (void)who;
#endif
}

static int status_failed(PyStatus st, const char *what)
{
    if (!PyStatus_Exception(st))
        return 0;
    fprintf(stderr, "03 FATAL %s: %s%s%s\n", what, st.func ? st.func : "",
            st.func ? ": " : "", st.err_msg ? st.err_msg : "(no message)");
    return 1;
}

static int append_path(PyConfig *cfg, const char *home, const char *suffix)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s%s", home, suffix);
    wchar_t *w = Py_DecodeLocale(buf, NULL); /* UTF-8 after pre-init in UTF-8 mode */
    if (!w)
        return -1;
    PyStatus st = PyWideStringList_Append(&cfg->module_search_paths, w);
    PyMem_RawFree(w);
    return status_failed(st, "module_search_paths") ? -1 : 0;
}

/* Exit status for the pending exception. SystemExit is handled here on purpose: letting
 * PyErr_Print() see it would call exit() from the interpreter thread and skip the end
 * marker and _exit() (R11). */
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

static int run_source(const char *src, const char *filename)
{
    PyObject *globals = PyModule_GetDict(PyImport_AddModule("__main__")); /* borrowed */
    PyObject *code = globals ? Py_CompileString(src, filename, Py_file_input) : NULL;
    if (!code)
        return status_from_exception();
    PyObject *res = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!res)
        return status_from_exception();
    Py_DECREF(res);
    return 0;
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

static int run_python(struct py_job *job)
{
    PyStatus st;
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    printf("03 step pre-init\n");
    st = Py_PreInitialize(&pre);
    if (status_failed(st, "Py_PreInitialize"))
        return 120;

    PyConfig cfg;
    PyConfig_InitIsolatedConfig(&cfg);
    cfg.buffered_stdio = 0;
    cfg.write_bytecode = 0;
    cfg.faulthandler = 1;
    cfg.verbose = job->verbose;

    int bad = 0;
    bad |= status_failed(PyConfig_SetBytesString(&cfg, &cfg.home, job->home), "home");
    bad |= status_failed(PyConfig_SetBytesString(&cfg, &cfg.program_name, "ksre-ps5-03-python-hello"),
                         "program_name");
    bad |= status_failed(PyConfig_SetBytesArgv(&cfg, job->argc, job->argv), "argv");
    cfg.module_search_paths_set = 1;
    bad |= append_path(&cfg, job->home, "/lib/python312.zip") != 0;
    bad |= append_path(&cfg, job->home, "/lib/python3.12") != 0;
    bad |= append_path(&cfg, job->home, "/lib/python3.12/lib-dynload") != 0;
    if (bad) {
        PyConfig_Clear(&cfg);
        return 121;
    }

    double t_init = mono_now();
    printf("03 step init t=%.3fs\n", t_init - g_t0);
    st = Py_InitializeFromConfig(&cfg);
    PyConfig_Clear(&cfg);
    if (status_failed(st, "Py_InitializeFromConfig"))
        return PyStatus_IsExit(st) ? st.exitcode : 122;
    printf("03 step ready t=%.3fs init=%.3fs\n", mono_now() - g_t0, mono_now() - t_init);

    int rc;
    if (job->script) {
        char *src = read_file(job->script);
        if (src) {
            rc = run_source(src, job->script);
            free(src);
        } else {
            fprintf(stderr, "03 FATAL cannot read %s\n", job->script);
            rc = 124;
        }
    } else {
        rc = run_source((const char *)hello_py, "<hello.py>");
    }
    if (Py_FinalizeEx() < 0 && rc == 0)
        rc = 120;
    return rc;
}

static void *py_thread(void *arg)
{
    struct py_job *job = arg;
    install_altstack();
    clear_ftz_daz("python-thread");
    job->rc = run_python(job);
    return NULL;
}

/* Arguments from <home>/../03/args.txt when the loader passed none (elfldr never does). */
static int args_from_file(const char *home, char ***argvp)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/../03/args.txt", home);
    char *text = read_file(path);
    if (!text)
        return 0;
    static char *av[64];
    int ac = 1;
    av[0] = (*argvp)[0];
    for (char *tok = strtok(text, " \t\r\n"); tok && ac < 63; tok = strtok(NULL, " \t\r\n"))
        av[ac++] = tok;
    av[ac] = NULL;
    printf("03 args from %s (%d)\n", path, ac - 1);
    *argvp = av;
    return ac;
}

int main(int argc, char **argv)
{
    g_t0 = mono_now();
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_fault_reporter();
    install_altstack();
    clear_ftz_daz("main");

    struct py_job job = {KSRE_PY_HOME, NULL, 0, 0, NULL, 0};
    if (argc <= 1) {
        int n = args_from_file(job.home, &argv);
        if (n)
            argc = n;
    }
    /* Payload start time for the script (time.monotonic() uses the same clock). */
    char t0buf[64];
    snprintf(t0buf, sizeof t0buf, "%.6f", g_t0);
    setenv("KSRE_PS5_T0", t0buf, 1);

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--home") && i + 1 < argc)
            job.home = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            job.script = argv[++i];
        else if (!strcmp(argv[i], "--verbose"))
            job.verbose = 1;
        else
            break;
    }
    /* sys.argv = [program, remaining args...] */
    job.argv = argv + i - 1;
    job.argc = argc - i + 1;
    job.argv[0] = argv[0] ? argv[0] : "payload";
    printf("03 start home=%s script=%s\n", job.home, job.script ? job.script : "(embedded hello.py)");

    pthread_attr_t attr;
    pthread_t tid;
    int err = pthread_attr_init(&attr);
    if (!err)
        err = pthread_attr_setstacksize(&attr, PY_THREAD_STACK);
    if (!err)
        err = pthread_create(&tid, &attr, py_thread, &job);
    if (!err)
        err = pthread_join(tid, NULL);
    if (err) {
        fprintf(stderr, "03 FATAL interpreter thread: error %d\n", err);
        job.rc = 123;
    }

    fflush(stderr);
    printf("KSRE_PS5_03_END rc=%d\n", job.rc);
    fflush(stdout);
    _exit(job.rc);
}
