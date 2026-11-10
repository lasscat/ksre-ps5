/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experiment 02b: can the PS5 give Ren'Py's gl2 renderer what it needs?
 *
 * For each requested API ("gl" = Ren'Py's `gl2`, desktop GL 2.0 compatibility
 * profile with GLSL 1.20; "gles" = Ren'Py's `gles2`, OpenGL ES 3.0 with GLSL ES
 * 1.00) this program
 *
 *   1. requests the GL attributes Ren'Py requests (renpy/gl2/gl2draw.pyx
 *      select_gl_attributes: RGBA8, 24-bit depth, version/profile above) and
 *      creates the window + context through SDL,
 *   2. logs GL_VENDOR/RENDERER/VERSION/SHADING_LANGUAGE_VERSION, the limits and
 *      the extension list, and resolves every function in Ren'Py's
 *      gl2functions.required_functions through SDL_GL_GetProcAddress,
 *   3. assembles Ren'Py's built-in shader parts (renpy.geometry, renpy.texture,
 *      renpy.matrixcolor, renpy.alpha, renpy.blur; texts copied from
 *      renpy/common/_shaders.rpym at the pinned tag) exactly the way
 *      renpy/gl2/gl2shadercache.py `source()` does, then compiles and links them,
 *   4. draws like Ren'Py: client-side vertex arrays, GL_UNSIGNED_INT client-side
 *      indices, premultiplied blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), a
 *      renderbuffer FBO sized like Ren'Py's, render-to-texture with
 *      glCopyTexImage2D + glGenerateMipmap,
 *   5. reads pixels back and checks them against values computed on the CPU
 *      (a black frame with "no GL errors" must not pass),
 *   6. times 1080p-virtual-size scenes, a fill-rate stress, the FBO +
 *      matrixcolor path, and Ren'Py's mip-bias blur (cached and re-rendered).
 *
 * Builds for the PS5 (prospero-clang + pacbrew SDL2, static; Mesa is dlopen()ed
 * by SDL at run time) and for the host (system SDL2; `--api gl` runs on a
 * macOS/Linux desktop GL 2.1+ driver).
 *
 * Log markers: KSRE_PS5_02B_BEGIN / KSRE_PS5_02B_END ... fail=N
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#if defined(__PROSPERO__)
#include <pthread_np.h>
#endif
#include <signal.h>
#if defined(__PROSPERO__)
#include <ucontext.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION "1"
#define VW 1920 /* Ren'Py virtual size of KSRE */
#define VH 1080

#if defined(__PROSPERO__)
#define TARGET "x86_64-sie-ps5"
#else
#define TARGET "host"
#endif

/* ===========================================================================
 * Logging, watchdog, crash marker
 * ======================================================================== */
static int g_fail;
static FILE *g_logfile; /* PS5: /data/ksre-ps5/02b/last-run.log (backup when stdout is not connected) */
static volatile const char *g_stage = "start";
static volatile unsigned long g_tick; /* heartbeat bumped by the main thread */
static volatile int g_main_done;
static long g_stall_ms = 45000, g_total_ms = 900000;

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

#define OK(what, ...) say("02B PASS " what " :: " __VA_ARGS__)
#define INFO(what, ...) say("02B INFO " what " :: " __VA_ARGS__)
#define FAIL(what, ...)                                                        \
  do {                                                                         \
    g_fail++;                                                                  \
    say("02B FAIL " what " :: " __VA_ARGS__);                                  \
  } while (0)
#define FAIL_SDL(what)                                                         \
  do {                                                                         \
    const char *e_ = SDL_GetError();                                           \
    g_fail++;                                                                  \
    say("02B FAIL " what " :: SDL_GetError: %s", e_ && *e_ ? e_ : "(empty)");  \
  } while (0)

static void stage(const char *s) {
  g_stage = s;
  g_tick++;
}

static void log_output(void *ud, int cat, SDL_LogPriority pri, const char *msg) {
  (void)ud;
  say("02B SDLLOG [cat=%d pri=%d] %s", cat, (int)pri, msg);
}

#if defined(__PROSPERO__)
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceUserServiceGetForegroundUser(unsigned int *user_id);
static void notify(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
static void notify(const char *msg) { (void)msg; }
#endif

/* A GL stall or crash must not leave the process (and the display) hanging.
 * The watchdog ends the process when the main thread stops making progress.
 * Explicit stack size: the default pthread stack is only 64 KiB here (01b). */
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
      say("02B FAIL watchdog :: %s (no progress for %ld ms, total %ld ms) in stage '%s'; _exit(3)",
          total >= g_total_ms ? "total time limit" : "stall", idle, total, (const char *)g_stage);
      say("KSRE_PS5_02B_END fail=%d watchdog=1 stage=%s", g_fail + 1, (const char *)g_stage);
      _exit(3);
    }
  }
  return NULL;
}

#if defined(__PROSPERO__)
static int g_peek_pipe[2] = {-1, -1};

/* Copies n bytes from an arbitrary address without risking a second fault: write() to a pipe
 * fails with EFAULT on an unreadable address, and read() returns the bytes. */
static int peek_bytes(const void *addr, unsigned char *out, size_t n) {
  if (g_peek_pipe[0] < 0 || !addr) return -1;
  ssize_t w = write(g_peek_pipe[1], addr, n);
  if (w != (ssize_t)n) return -1;
  return read(g_peek_pipe[0], out, n) == (ssize_t)n ? 0 : -1;
}

/* Fault details: address, instruction pointer, stack pointer and the module/symbol the
 * instruction pointer is in. fault == rip means execution of memory that is not executable
 * (JIT permissions); a small fault address means a null-ish dereference. */
static void crash_action(int sig, siginfo_t *si, void *ucv) {
  char buf[640];
  const ucontext_t *uc = ucv;
  /* The PS5 signal frame has extra words before mc_rip, so the header's field offsets are wrong
   * (console 02b: word 27 is always 0x43 = user cs, 28 = rflags, 29 = rsp, 26 = rip). Use the raw
   * words when that pattern holds, and fall back to the header fields otherwise. */
  const unsigned long *w = (const unsigned long *)&uc->uc_mcontext;
  int raw = (w[27] & 0xffff) == 0x43;
  void *rip = raw ? (void *)w[26] : (void *)uc->uc_mcontext.mc_rip;
  void *rsp = raw ? (void *)w[29] : (void *)uc->uc_mcontext.mc_rsp;
  Dl_info di;
  memset(&di, 0, sizeof di);
  int have = dladdr(rip, &di);
  int n = snprintf(buf, sizeof buf,
                   "\n02B FAIL crash :: signal %d code %d fault_addr=%p rip=%p rsp=%p rbp=%p (context=%s) in %s+0x%lx : %s\n"
                   "02B FAIL crash :: stage '%s'\n",
                   sig, si ? si->si_code : -1, si ? si->si_addr : NULL, rip, rsp, (void *)w[9], raw ? "raw" : "header",
                   have && di.dli_fname ? di.dli_fname : "(unknown module)", have ? (unsigned long)((char *)rip - (char *)di.dli_fbase) : 0UL,
                   have && di.dli_sname ? di.dli_sname : "(no symbol)", (const char *)g_stage);
  if (n > 0) {
    (void)!write(1, buf, (size_t)n);
    if (g_logfile) (void)!write(fileno(g_logfile), buf, (size_t)n);
  }
  /* Raw saved-context words, the code bytes at the instruction pointer, and the top of the stack
   * (return addresses) with the module each value falls in, for mapping offline. */
  {
    char big[4096];
    int m = 0;
    m += snprintf(big + m, sizeof big - (size_t)m, "02B FAIL crash :: mcontext words:");
    for (int i = 0; i < 32; i++) m += snprintf(big + m, sizeof big - (size_t)m, " %lx", w[i]);
    unsigned char code[48];
    m += snprintf(big + m, sizeof big - (size_t)m, "\n02B FAIL crash :: code at rip-16 (48 bytes):");
    if (peek_bytes((const unsigned char *)rip - 16, code, sizeof code) == 0)
      for (int i = 0; i < 48; i++) m += snprintf(big + m, sizeof big - (size_t)m, " %02x", code[i]);
    else
      m += snprintf(big + m, sizeof big - (size_t)m, " (unreadable)");
    m += snprintf(big + m, sizeof big - (size_t)m, "\n");
    unsigned long st[48];
    /* Stay inside the page that holds rsp: a read that crosses into an unmapped page fails whole. */
    size_t room = 4096 - ((size_t)(uintptr_t)rsp & 4095);
    size_t want = room < sizeof st ? room : sizeof st;
    int nst = (int)(want / sizeof st[0]);
    if (nst > 0 && peek_bytes(rsp, (unsigned char *)st, (size_t)nst * sizeof st[0]) == 0) {
      for (int i = 0; i < nst && m < (int)sizeof big - 200; i++) {
        Dl_info d2;
        memset(&d2, 0, sizeof d2);
        if (st[i] > 0x10000 && dladdr((void *)st[i], &d2) && d2.dli_fname) {
          const char *b = strrchr(d2.dli_fname, '/');
          m += snprintf(big + m, sizeof big - (size_t)m, "02B FAIL crash :: stack[%d] %lx %s+0x%lx\n", i, st[i], b ? b + 1 : d2.dli_fname,
                        (unsigned long)(st[i] - (unsigned long)d2.dli_fbase));
        } else if (i < 28) {
          m += snprintf(big + m, sizeof big - (size_t)m, "02B FAIL crash :: stack[%d] %lx\n", i, st[i]);
        }
      }
    } else {
      m += snprintf(big + m, sizeof big - (size_t)m, "02B FAIL crash :: stack unreadable\n");
    }
    m += snprintf(big + m, sizeof big - (size_t)m, "KSRE_PS5_02B_END fail=%d crash=%d stage=%s\n", g_fail + 1, sig, (const char *)g_stage);
    (void)!write(1, big, (size_t)m);
    if (g_logfile) (void)!write(fileno(g_logfile), big, (size_t)m);
  }
  _exit(4);
}
#endif

static void crash_handler(int sig) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "\n02B FAIL crash :: signal %d in stage '%s'\nKSRE_PS5_02B_END fail=%d crash=%d stage=%s\n",
                   sig, (const char *)g_stage, g_fail + 1, sig, (const char *)g_stage);
  if (n > 0) {
    (void)!write(1, buf, (size_t)n);
    if (g_logfile) (void)!write(fileno(g_logfile), buf, (size_t)n);
  }
  _exit(4);
}

/* ===========================================================================
 * Minimal GL declarations. Everything is resolved at run time through
 * SDL_GL_GetProcAddress (as Ren'Py's uguu does), so the same binary logic
 * serves desktop GL and GLES and nothing links against libGL.
 * ======================================================================== */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_DEPTH_COMPONENT 0x1902
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE0 0x84C0
#define GL_GENERATE_MIPMAP_HINT 0x8192
#define GL_NICEST 0x1102
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_LOD_BIAS 0x84FD
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

/* Functions Ren'Py requires (renpy/gl2/gl2functions.py required_functions). */
#define GL_REQUIRED_FUNCS(X)                                                                                     \
  X(void, glActiveTexture, (GLenum))                                                                             \
  X(void, glAttachShader, (GLuint, GLuint))                                                                      \
  X(void, glBindFramebuffer, (GLenum, GLuint))                                                                   \
  X(void, glBindRenderbuffer, (GLenum, GLuint))                                                                  \
  X(void, glBindTexture, (GLenum, GLuint))                                                                       \
  X(void, glBlendEquation, (GLenum))                                                                             \
  X(void, glBlendEquationSeparate, (GLenum, GLenum))                                                             \
  X(void, glBlendFunc, (GLenum, GLenum))                                                                         \
  X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                 \
  X(void, glClear, (GLbitfield))                                                                                 \
  X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                    \
  X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                             \
  X(void, glCompileShader, (GLuint))                                                                             \
  X(void, glCopyTexImage2D, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint))                      \
  X(GLuint, glCreateProgram, (void))                                                                             \
  X(GLuint, glCreateShader, (GLenum))                                                                            \
  X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                                                       \
  X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *))                                                      \
  X(void, glDeleteShader, (GLuint))                                                                              \
  X(void, glDeleteTextures, (GLsizei, const GLuint *))                                                           \
  X(void, glDisableVertexAttribArray, (GLuint))                                                                  \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))                                               \
  X(void, glEnable, (GLenum))                                                                                    \
  X(void, glEnableVertexAttribArray, (GLuint))                                                                   \
  X(void, glFinish, (void))                                                                                      \
  X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                           \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                       \
  X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                                \
  X(void, glGenRenderbuffers, (GLsizei, GLuint *))                                                               \
  X(void, glGenTextures, (GLsizei, GLuint *))                                                                    \
  X(void, glGenerateMipmap, (GLenum))                                                                            \
  X(GLint, glGetAttribLocation, (GLuint, const GLchar *))                                                        \
  X(void, glGetFloatv, (GLenum, GLfloat *))                                                                      \
  X(void, glGetIntegerv, (GLenum, GLint *))                                                                      \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                           \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                             \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                            \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                              \
  X(const GLubyte *, glGetString, (GLenum))                                                                      \
  X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                                       \
  X(void, glHint, (GLenum, GLenum))                                                                              \
  X(void, glLinkProgram, (GLuint))                                                                               \
  X(void, glPixelStorei, (GLenum, GLint))                                                                        \
  X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                                \
  X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                             \
  X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                               \
  X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))           \
  X(void, glTexParameterf, (GLenum, GLenum, GLfloat))                                                            \
  X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                              \
  X(void, glUniform1f, (GLint, GLfloat))                                                                         \
  X(void, glUniform1i, (GLint, GLint))                                                                           \
  X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                                \
  X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                                       \
  X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                              \
  X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                                      \
  X(void, glUseProgram, (GLuint))                                                                                \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))                      \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

/* Extra functions this experiment uses (not in Ren'Py's required list). */
#define GL_EXTRA_FUNCS(X)                                                                                        \
  X(GLenum, glGetError, (void))                                                                                  \
  X(void, glDisable, (GLenum))                                                                                   \
  X(GLenum, glCheckFramebufferStatus, (GLenum))                                                                  \
  X(void, glDeleteProgram, (GLuint))

#define X(ret, name, args) static ret(*name) args;
GL_REQUIRED_FUNCS(X)
GL_EXTRA_FUNCS(X)
#undef X

static int g_dump_procs;
/* _exit() after the end marker instead of returning from main. Returning crashes on the PS5 when the
 * GL library has been unloaded by SDL_Quit: a thread-exit destructor then points into unmapped
 * library code (console 02b). Default on for the PS5 build; --normal-exit restores the return. */
#if defined(__PROSPERO__)
static int g_fast_exit = 1;
#else
static int g_fast_exit = 0;
#endif
#if defined(__PROSPERO__)
static void *g_osmesa_handle;
#endif

/* --dump-procs: where each resolved entry point lives. An address outside any loaded module
 * (module "?") is a stub the GL library generated at run time. */
static void dump_proc(const char *fname, void *p) {
  Dl_info di;
  if (p && dladdr(p, &di) && di.dli_fname) {
    const char *base = strrchr(di.dli_fname, '/');
    INFO("gl.proc", "%-26s %p  %s+0x%lx", fname, p, base ? base + 1 : di.dli_fname, (unsigned long)((char *)p - (char *)di.dli_fbase));
  } else {
    INFO("gl.proc", "%-26s %p  ? (no module)", fname, p);
  }
}

static int load_gl_functions(void) {
  int missing = 0, n = 0;
#define X(ret, name, args)                                                                                       \
  name = (ret(*) args)SDL_GL_GetProcAddress(#name);                                                              \
  n++;                                                                                                           \
  if (g_dump_procs) dump_proc(#name, (void *)name);                                                              \
  if (!name) {                                                                                                   \
    missing++;                                                                                                   \
    FAIL("gl.function", "required function %s did not resolve", #name);                                          \
  }
  GL_REQUIRED_FUNCS(X)
  GL_EXTRA_FUNCS(X)
#undef X
  INFO("gl.functions", "%d of %d resolved (Ren'Py's required list plus the extras this test uses)", n - missing, n);
#if defined(__PROSPERO__)
  if (g_osmesa_handle && glGenerateMipmap) {
    void *d = dlsym(g_osmesa_handle, "glGenerateMipmap");
    INFO("gl.alias", "SDL_GL_GetProcAddress(glGenerateMipmap)=%p dlsym=%p delta=0x%lx", (void *)glGenerateMipmap, d,
         (unsigned long)((char *)glGenerateMipmap - (char *)d));
  }
#endif
  return missing;
}

static int g_no_mipmap; /* --no-mipmap: skip glGenerateMipmap (diagnostic; the blur test is then not meaningful) */

static void gen_mipmap(void) {
  if (g_no_mipmap) return;
  glGenerateMipmap(GL_TEXTURE_2D);
}

/* --probe-calls: one call to each GL entry point class Ren'Py uses, in order of increasing dispatch
 * slot, each preceded by a stage marker, so a crash names the first call that fails. */
static void probe_calls(void) {
  static const unsigned char px[4 * 4 * 4] = {0};
  GLuint id = 0, tex = 0;
  stage("probe.glActiveTexture");
  glActiveTexture(GL_TEXTURE0);
  INFO("probe", "glActiveTexture ok");
  stage("probe.glBlendFuncSeparate");
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  INFO("probe", "glBlendFuncSeparate ok");
  stage("probe.glCreateShader");
  id = glCreateShader(GL_VERTEX_SHADER);
  glDeleteShader(id);
  INFO("probe", "glCreateShader/glDeleteShader ok (id=%u)", (unsigned)id);
  stage("probe.glGenRenderbuffers");
  glGenRenderbuffers(1, &id);
  glDeleteRenderbuffers(1, &id);
  INFO("probe", "glGenRenderbuffers/glDeleteRenderbuffers ok");
  stage("probe.glGenFramebuffers");
  glGenFramebuffers(1, &id);
  glDeleteFramebuffers(1, &id);
  INFO("probe", "glGenFramebuffers/glDeleteFramebuffers ok");
  stage("probe.glTexImage2D4x4");
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  INFO("probe", "4x4 texture uploaded");
  {
    unsigned char out[4] = {0, 0, 0, 0};
    stage("probe.glClear");
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000 | 0x100); /* GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT */
    glFinish();
    INFO("probe", "glClear + glFinish ok");
    stage("probe.glReadPixels");
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    INFO("probe", "glReadPixels ok: %u %u %u %u (expect ~64 128 191 255)", out[0], out[1], out[2], out[3]);
  }
  stage("probe.glGenerateMipmap");
  if (g_no_mipmap) {
    INFO("probe", "glGenerateMipmap skipped (--no-mipmap)");
  } else {
    glGenerateMipmap(GL_TEXTURE_2D);
    INFO("probe", "glGenerateMipmap on a 4x4 texture ok");
  }
  glDeleteTextures(1, &tex);
}

static int gl_errors(const char *where) {
  int n = 0;
  GLenum e;
  while ((e = glGetError()) != GL_NO_ERROR && n < 8) {
    n++;
    FAIL("gl.error", "glGetError 0x%x after %s", e, where);
  }
  return n;
}

/* ===========================================================================
 * Ren'Py shader parts, assembled the way gl2shadercache.source() does.
 * Part texts are copied verbatim from renpy/common/_shaders.rpym
 * (Ren'Py 8.5.3.26051504); indentation included.
 * ======================================================================== */
typedef struct {
  int prio;
  const char *text;
} PartText;

typedef struct {
  const char *name;
  const char *variables; /* newline-separated declarations, no trailing ';' needed */
  const PartText *vertex;
  const PartText *fragment; /* both arrays end with {0, NULL} */
} ShaderPart;

static const PartText geometry_v[] = {{0, "\n        gl_Position = a_position;\n    "},
                                      {100, "\n        gl_Position = u_transform * gl_Position;\n    "},
                                      {0, NULL}};
static const PartText none_p[] = {{0, NULL}};
static const PartText texture_v[] = {{200, "\n        v_tex_coord = a_tex_coord;\n    "}, {0, NULL}};
static const PartText texture_f[] = {{200, "\n        gl_FragColor = texture2D(tex0, v_tex_coord.xy, u_lod_bias);\n    "}, {0, NULL}};
static const PartText blur_f[] = {
    {200, "\n"
          "        gl_FragColor = vec4(0.);\n"
          "        float renpy_blur_norm = 0.;\n"
          "\n"
          "        for (float i = -5.; i < 1.; i += 1.) {\n"
          "            float renpy_blur_weight = exp(-0.5 * pow(u_renpy_blur_log2 - i, 2.));\n"
          "            renpy_blur_norm += renpy_blur_weight;\n"
          "        }\n"
          "\n"
          "        gl_FragColor += renpy_blur_norm * texture2D(tex0, v_tex_coord.xy, 0.);\n"
          "\n"
          "        for (float i = 1.; i < 14.; i += 1.) {\n"
          "\n"
          "            if (i >= u_renpy_blur_log2 + 5.) {\n"
          "                break;\n"
          "            }\n"
          "\n"
          "            float renpy_blur_weight = exp(-0.5 * pow(u_renpy_blur_log2 - i, 2.));\n"
          "            gl_FragColor += renpy_blur_weight * texture2D(tex0, v_tex_coord.xy, i);\n"
          "            renpy_blur_norm += renpy_blur_weight;\n"
          "        }\n"
          "\n"
          "        if (renpy_blur_norm > 0.0) {\n"
          "            gl_FragColor /= renpy_blur_norm;\n"
          "        } else {\n"
          "            gl_FragColor = texture2D(tex0, v_tex_coord.xy, 0.0);\n"
          "        }\n"
          "   "},
    {0, NULL}};
static const PartText matrixcolor_f[] = {{400, "\n        gl_FragColor = u_renpy_matrixcolor * gl_FragColor;\n    "}, {0, NULL}};
static const PartText alpha_f[] = {
    {500, "\n        gl_FragColor = gl_FragColor * vec4(u_renpy_alpha, u_renpy_alpha, u_renpy_alpha, u_renpy_alpha * u_renpy_over);\n    "},
    {0, NULL}};

static const ShaderPart P_GEOMETRY = {"renpy.geometry", "uniform mat4 u_transform;\nattribute vec4 a_position;", geometry_v, none_p};
static const ShaderPart P_TEXTURE = {"renpy.texture",
                                     "uniform float u_lod_bias;\nuniform sampler2D tex0;\nattribute vec2 a_tex_coord;\nvarying vec2 v_tex_coord;",
                                     texture_v, texture_f};
static const ShaderPart P_BLUR = {"renpy.blur",
                                  "uniform sampler2D tex0;\nattribute vec2 a_tex_coord;\nvarying vec2 v_tex_coord;\nuniform float u_renpy_blur_log2;",
                                  texture_v, blur_f};
static const ShaderPart P_MATRIXCOLOR = {"renpy.matrixcolor", "uniform mat4 u_renpy_matrixcolor;", none_p, matrixcolor_f};
static const ShaderPart P_ALPHA = {"renpy.alpha", "uniform float u_renpy_alpha;\nuniform float u_renpy_over;", none_p, alpha_f};

/* word-boundary containment test (the \b\w+\b scan Ren'Py does) */
static int has_word(const char *hay, const char *word) {
  size_t wl = strlen(word);
  for (const char *p = hay; (p = strstr(p, word)) != NULL; p++) {
    int before = (p == hay) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
    int after = !(isalnum((unsigned char)p[wl]) || p[wl] == '_');
    if (before && after) return 1;
  }
  return 0;
}

typedef struct {
  char line[96];
  char name[64];
} VarDecl;

typedef struct {
  int prio;
  const char *part;
  const char *text;
} Emitted;

static int cmp_var(const void *a, const void *b) { return strcmp(((const VarDecl *)a)->name, ((const VarDecl *)b)->name); }
static int cmp_emit(const void *a, const void *b) {
  const Emitted *x = a, *y = b;
  if (x->prio != y->prio) return x->prio < y->prio ? -1 : 1;
  int c = strcmp(x->part, y->part);
  return c ? c : strcmp(x->text, y->text);
}

/* Builds the source for one stage. Parts must be given in sorted-by-name order
 * (ShaderCache.get sorts the part names before assembling). */
static char *assemble(const ShaderPart *const *parts, int nparts, int gles, int fragment) {
  VarDecl vars[24];
  int nvars = 0;
  Emitted em[16];
  int nem = 0;
  char *out = malloc(16384);
  size_t len = 0;
  if (!out) return NULL;
#define APPEND(...) len += (size_t)snprintf(out + len, 16384 - len, __VA_ARGS__)

  for (int i = 0; i < nparts; i++) {
    const ShaderPart *sp = parts[i];
    const PartText *pt = fragment ? sp->fragment : sp->vertex;
    for (; pt->text; pt++) em[nem++] = (Emitted){pt->prio, sp->name, pt->text};
    /* variables of this part used by this part's own code in this stage */
    const char *v = sp->variables;
    while (*v) {
      const char *nl = strchr(v, '\n');
      size_t ll = nl ? (size_t)(nl - v) : strlen(v);
      char line[96];
      snprintf(line, sizeof line, "%.*s", (int)ll, v);
      v += ll + (nl ? 1 : 0);
      /* variable name = last identifier of the declaration (all our parts are simple) */
      char *semi = strchr(line, ';');
      if (semi) *semi = 0;
      char *last = strrchr(line, ' ');
      const char *name = last ? last + 1 : line;
      int used = 0;
      for (const PartText *q = fragment ? sp->fragment : sp->vertex; q->text; q++)
        if (has_word(q->text, name)) used = 1;
      if (!used) continue;
      int dup = 0;
      for (int k = 0; k < nvars; k++)
        if (!strcmp(vars[k].name, name)) dup = 1;
      if (dup) continue;
      snprintf(vars[nvars].line, sizeof vars[nvars].line, "%s", line);
      snprintf(vars[nvars].name, sizeof vars[nvars].name, "%s", name);
      nvars++;
    }
  }
  qsort(vars, (size_t)nvars, sizeof vars[0], cmp_var);
  qsort(em, (size_t)nem, sizeof em[0], cmp_emit);

  if (gles) {
    APPEND("#version 100\n");
    if (fragment)
      APPEND("#ifdef GL_FRAGMENT_PRECISION_HIGH\n    precision highp float;\n    precision highp int;\n#else\n    precision mediump float;\n    precision mediump int;\n#endif\n");
  } else {
    APPEND("#version 120\n");
  }
  for (int i = 0; i < nvars; i++) APPEND("%s;\n", vars[i].line);
  APPEND("\nvoid main() {\n");
  for (int i = 0; i < nem; i++) APPEND("%s", em[i].text);
  APPEND("}\n");
#undef APPEND
  return out;
}

typedef struct {
  GLuint prog;
  GLint a_position, a_tex_coord;
  GLint u_transform, u_lod_bias, tex0, u_matrixcolor, u_alpha, u_over, u_blur_log2;
  const char *name;
} Prog;

static void print_numbered(const char *src) {
  int line = 1;
  const char *p = src;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    say("02B SRC %3d| %.*s", line++, (int)n, p);
    if (!nl) break;
    p = nl + 1;
  }
}

static GLuint compile_stage(GLenum kind, const char *src, const char *pname, int dump) {
  const char *kn = kind == GL_VERTEX_SHADER ? "vertex" : "fragment";
  GLuint sh = glCreateShader(kind);
  if (!sh) {
    FAIL("shader.create", "%s %s: glCreateShader returned 0", pname, kn);
    return 0;
  }
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0, loglen = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &loglen);
  if (loglen > 1) {
    char *log = malloc((size_t)loglen + 1);
    if (log) {
      glGetShaderInfoLog(sh, loglen, NULL, log);
      log[loglen] = 0;
      INFO("shader.log", "%s %s compile log: %s", pname, kn, log);
      free(log);
    }
  }
  if (!ok) {
    FAIL("shader.compile", "%s %s shader did not compile", pname, kn);
    print_numbered(src);
    glDeleteShader(sh);
    return 0;
  }
  if (dump) print_numbered(src);
  return sh;
}

static int build_program(Prog *p, const char *name, const ShaderPart *const *parts, int nparts, int gles, int dump) {
  memset(p, 0, sizeof *p);
  p->name = name;
  char *vs = assemble(parts, nparts, gles, 0), *fs = assemble(parts, nparts, gles, 1);
  if (!vs || !fs) {
    free(vs);
    free(fs);
    FAIL("shader.assemble", "%s: out of memory", name);
    return -1;
  }
  GLuint v = compile_stage(GL_VERTEX_SHADER, vs, name, dump);
  GLuint f = compile_stage(GL_FRAGMENT_SHADER, fs, name, dump);
  free(vs);
  free(fs);
  if (!v || !f) {
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return -1;
  }
  p->prog = glCreateProgram();
  glAttachShader(p->prog, v);
  glAttachShader(p->prog, f);
  glLinkProgram(p->prog);
  GLint ok = 0, loglen = 0;
  glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
  glGetProgramiv(p->prog, GL_INFO_LOG_LENGTH, &loglen);
  if (loglen > 1) {
    char *log = malloc((size_t)loglen + 1);
    if (log) {
      glGetProgramInfoLog(p->prog, loglen, NULL, log);
      log[loglen] = 0;
      INFO("shader.log", "%s link log: %s", name, log);
      free(log);
    }
  }
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok) {
    FAIL("shader.link", "%s did not link", name);
    glDeleteProgram(p->prog);
    p->prog = 0;
    return -1;
  }
  p->a_position = glGetAttribLocation(p->prog, "a_position");
  p->a_tex_coord = glGetAttribLocation(p->prog, "a_tex_coord");
  p->u_transform = glGetUniformLocation(p->prog, "u_transform");
  p->u_lod_bias = glGetUniformLocation(p->prog, "u_lod_bias");
  p->tex0 = glGetUniformLocation(p->prog, "tex0");
  p->u_matrixcolor = glGetUniformLocation(p->prog, "u_renpy_matrixcolor");
  p->u_alpha = glGetUniformLocation(p->prog, "u_renpy_alpha");
  p->u_over = glGetUniformLocation(p->prog, "u_renpy_over");
  p->u_blur_log2 = glGetUniformLocation(p->prog, "u_renpy_blur_log2");
  OK("shader.link", "%s (%s): attrib position=%d tex_coord=%d; uniforms transform=%d lod_bias=%d tex0=%d", name,
     gles ? "GLSL ES 1.00" : "GLSL 1.20", p->a_position, p->a_tex_coord, p->u_transform, p->u_lod_bias, p->tex0);
  if (gl_errors(name)) return -1;
  return 0;
}

/* ===========================================================================
 * Textures, FBO, drawing (Ren'Py style)
 * ======================================================================== */
typedef struct {
  /* surface */
  int win_w, win_h;   /* drawable size: what Ren'Py renders the screen into */
  GLint default_fbo;
  int gles;
  /* programs */
  Prog tex, tex_alpha, tex_mc, blur;
  /* textures */
  GLuint t_bg, t_sprite_a, t_sprite_b, t_box, t_flat_f, t_flat_g, t_copy;
  /* FBO (renderbuffer-backed, sized like Ren'Py's init_fbo) */
  GLuint fbo, rb_color, rb_depth;
  int fbo_w, fbo_h;
  GLint cur_fbo;
} Ctx;

static void change_fbo(Ctx *c, GLuint fbo) {
  if ((GLuint)c->cur_fbo != fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    c->cur_fbo = (GLint)fbo;
  }
}

/* Column-major transform from virtual space (0..VW, 0..VH) to clip space.
 * flip=1: screen projection (image y down); flip=0: render-to-texture
 * projection, so FBO row 0 is the image top and texture v=0 stays the top. */
static void make_transform(float m[16], int flip) {
  memset(m, 0, 16 * sizeof(float));
  m[0] = 2.0f / VW;
  m[5] = flip ? -2.0f / VH : 2.0f / VH;
  m[10] = 1.0f;
  m[12] = -1.0f;
  m[13] = flip ? 1.0f : -1.0f;
  m[15] = 1.0f;
}

static void blend_default(void) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

typedef struct {
  float a_position[4];
  float a_tex_coord[2];
} Vtx;

static void draw_quad(const Prog *p, GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
  static const GLuint idx[6] = {0, 1, 2, 2, 1, 3};
  Vtx v[4] = {{{x, y, 0, 1}, {u0, v0}}, {{x + w, y, 0, 1}, {u1, v0}}, {{x, y + h, 0, 1}, {u0, v1}}, {{x + w, y + h, 0, 1}, {u1, v1}}};
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  /* Client-side arrays and client-side 32-bit indices, as gl2shader.pyx does. */
  if (p->a_position >= 0) {
    glVertexAttribPointer((GLuint)p->a_position, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), v[0].a_position);
    glEnableVertexAttribArray((GLuint)p->a_position);
  }
  if (p->a_tex_coord >= 0) {
    glVertexAttribPointer((GLuint)p->a_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), v[0].a_tex_coord);
    glEnableVertexAttribArray((GLuint)p->a_tex_coord);
  }
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, idx);
  if (p->a_position >= 0) glDisableVertexAttribArray((GLuint)p->a_position);
  if (p->a_tex_coord >= 0) glDisableVertexAttribArray((GLuint)p->a_tex_coord);
}

static void use_prog(const Prog *p, const float *transform) {
  glUseProgram(p->prog);
  if (p->u_transform >= 0) glUniformMatrix4fv(p->u_transform, 1, GL_FALSE, transform);
  if (p->u_lod_bias >= 0) glUniform1f(p->u_lod_bias, 0.0f);
  if (p->tex0 >= 0) glUniform1i(p->tex0, 0);
}

/* ---- procedural content (original, deterministic) ----------------------- */
static uint8_t *pix_alloc(int w, int h) { return malloc((size_t)w * (size_t)h * 4); }

static void put(uint8_t *px, int i, int r, int g, int b, int a) {
  px[i * 4 + 0] = (uint8_t)r;
  px[i * 4 + 1] = (uint8_t)g;
  px[i * 4 + 2] = (uint8_t)b;
  px[i * 4 + 3] = (uint8_t)a;
}

static uint8_t *make_bg(int w, int h) {
  uint8_t *px = pix_alloc(w, h);
  if (!px) return NULL;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int r = 30 + 120 * x / w, g = 50 + 110 * y / h, b = 150 - 70 * x / w;
      if (x % 120 < 2 || y % 120 < 2) { r += 60; g += 60; b += 60; } /* 16:9 grid */
      double dx = x - w / 2.0, dy = y - h / 2.0, d = sqrt(dx * dx + dy * dy);
      if (fabs(d - 400.0) < 3.0) { r = g = b = 250; }                   /* circle: round on a 16:9 screen */
      if (x < 48 && y < 48) { r = 255; g = 40; b = 40; }                  /* corner markers */
      if (x >= w - 48 && y < 48) { r = 40; g = 255; b = 40; }
      if (x < 48 && y >= h - 48) { r = 60; g = 60; b = 255; }
      if (x >= w - 48 && y >= h - 48) { r = 255; g = 230; b = 40; }
      put(px, y * w + x, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255);
    }
  return px;
}

/* premultiplied soft ellipse */
static uint8_t *make_sprite(int w, int h, int cr, int cg, int cb) {
  uint8_t *px = pix_alloc(w, h);
  if (!px) return NULL;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double dx = (x - w / 2.0) / (w / 2.0), dy = (y - h / 2.0) / (h / 2.0), d = sqrt(dx * dx + dy * dy);
      double a = (1.0 - d) * 10.0;
      a = a < 0 ? 0 : a > 1 ? 1 : a;
      double shade = 0.55 + 0.45 * (1.0 - fabs(dy));
      put(px, y * w + x, (int)(cr * shade * a), (int)(cg * shade * a), (int)(cb * shade * a), (int)(255 * a));
    }
  return px;
}

/* premultiplied translucent dialogue-box style rectangle with an opaque edge */
static uint8_t *make_box(int w, int h) {
  uint8_t *px = pix_alloc(w, h);
  if (!px) return NULL;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int edge = (x < 4 || y < 4 || x >= w - 4 || y >= h - 4);
      if (edge) put(px, y * w + x, 240, 240, 240, 255);
      else put(px, y * w + x, 6, 12, 30, 153); /* 60% alpha, premultiplied */
    }
  return px;
}

static void upload(GLuint tex, int w, int h, const void *pixels, int mipmaps) {
  stage("upload.params");
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipmaps ? 1000 : 0);
  stage("upload.glTexImage2D");
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  if (mipmaps) {
    stage("upload.glGenerateMipmap");
    glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
    gen_mipmap();
  }
  stage("upload.done");
}

static GLuint make_flat_texture(int r, int g, int b, int a) {
  uint8_t px[4 * 4 * 4];
  for (int i = 0; i < 16; i++) put(px, i, r, g, b, a);
  GLuint t;
  glGenTextures(1, &t);
  upload(t, 4, 4, px, 0);
  return t;
}

/* gl2texture.pyx allocate_texture(): full mip chain of NULL levels, MAX_LEVEL 1000 */
static void allocate_texture(GLuint tex, int tw, int th) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLuint level = 0;
  for (;;) {
    glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (tw == 1 && th == 1) break;
    tw = tw >> 1 ? tw >> 1 : 1;
    th = th >> 1 ? th >> 1 : 1;
    level++;
    if (level > 1000) break;
  }
}

static void mipmap_texture(GLuint tex) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
  glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
  gen_mipmap();
}

/* gl2draw.pyx init_fbo(): colour (+depth) renderbuffers of
 * min(max(virtual+64, drawable+64, config.max_texture_size 4096), limits). */
static int init_fbo(Ctx *c) {
  GLint max_rb = 0, max_tex = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &c->default_fbo);
  c->cur_fbo = c->default_fbo;
  glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
  if (max_rb < 1024) max_rb = 1024;
  if (max_tex < 1024) max_tex = 1024;
  int w = VW + 64, h = VH + 64;
  if (c->win_w + 64 > w) w = c->win_w + 64;
  if (c->win_h + 64 > h) h = c->win_h + 64;
  if (w < 4096) w = 4096;
  if (h < 4096) h = 4096;
  if (w > max_tex) w = max_tex;
  if (h > max_tex) h = max_tex;
  if (w > max_rb) w = max_rb;
  if (h > max_rb) h = max_rb;
  c->fbo_w = w;
  c->fbo_h = h;
  glGenFramebuffers(1, &c->fbo);
  glGenRenderbuffers(1, &c->rb_color);
  glGenRenderbuffers(1, &c->rb_depth);
  change_fbo(c, c->fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, c->rb_color);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c->rb_color);
  glBindRenderbuffer(GL_RENDERBUFFER, c->rb_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, c->gles ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c->rb_depth);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  change_fbo(c, (GLuint)c->default_fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  if (gl_errors("init_fbo")) return -1;
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    FAIL("fbo.status", "framebuffer status 0x%x for %dx%d RGBA8 + depth24 renderbuffers", st, w, h);
    return -1;
  }
  OK("fbo.status", "complete: %dx%d RGBA8 + depth (default fbo %d, max renderbuffer %d, max texture %d)", w, h, c->default_fbo,
     max_rb, max_tex);
  return 0;
}

/* ---- scenes ------------------------------------------------------------- */
/* A typical visual-novel frame: background (mipmapped, opaque), two sprites
 * with soft alpha edges (one through renpy.alpha), a translucent text box. */
static void draw_scene(Ctx *c, const float *tf, double t) {
  use_prog(&c->tex, tf);
  draw_quad(&c->tex, c->t_bg, 0, 0, VW, VH, 0, 0, 1, 1);
  float sx = (float)(20.0 * sin(t * 1.3));
  draw_quad(&c->tex, c->t_sprite_a, 560 + sx, 90, 640, 900, 0, 0, 1, 1);
  use_prog(&c->tex_alpha, tf);
  glUniform1f(c->tex_alpha.u_alpha, (float)(0.75 + 0.25 * sin(t * 2.1)));
  glUniform1f(c->tex_alpha.u_over, 1.0f);
  draw_quad(&c->tex_alpha, c->t_sprite_b, 1040 - sx, 90, 640, 900, 0, 0, 1, 1);
  use_prog(&c->tex, tf);
  draw_quad(&c->tex, c->t_box, 80, 740, 1760, 300, 0, 0, 1, 1);
}

static void draw_fill(Ctx *c, const float *tf, double t, int layers) {
  use_prog(&c->tex, tf);
  draw_quad(&c->tex, c->t_bg, 0, 0, VW, VH, 0, 0, 1, 1);
  use_prog(&c->tex_alpha, tf);
  for (int i = 1; i < layers; i++) {
    glUniform1f(c->tex_alpha.u_alpha, 0.35f);
    glUniform1f(c->tex_alpha.u_over, 1.0f);
    draw_quad(&c->tex_alpha, (i & 1) ? c->t_sprite_a : c->t_sprite_b, (float)(i * 10) + (float)(10 * sin(t)), (float)(i * 6), VW - 20.0f * (float)i, VH - 12.0f * (float)i, 0, 0, 1, 1);
  }
}

/* Render the scene into the FBO (render-to-texture projection) and copy it into
 * c->t_copy exactly like gl2texture.pyx render-to-texture does. */
static void render_to_texture(Ctx *c, double t, int regenerate_alloc) {
  float tf[16];
  make_transform(tf, 0);
  change_fbo(c, c->fbo);
  if (regenerate_alloc) allocate_texture(c->t_copy, VW, VH);
  glViewport(0, 0, VW, VH);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  blend_default();
  draw_scene(c, tf, t);
  glBindTexture(GL_TEXTURE_2D, c->t_copy);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, VW, VH, 0);
  change_fbo(c, (GLuint)c->default_fbo);
}

static void begin_screen(Ctx *c, int vp_w, int vp_h) {
  change_fbo(c, (GLuint)c->default_fbo);
  glViewport(0, 0, vp_w, vp_h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  blend_default();
}

/* ===========================================================================
 * Read-back correctness checks
 * ======================================================================== */
static const int FLAT_F[4] = {200, 100, 50, 255};
static const int FLAT_G_PRE[4] = {50, 50, 50, 128}; /* premultiplied: colour 100 at alpha 128/255 */

static void probe(const char *what, int x, int y, int er, int eg, int eb, int tol) {
  uint8_t px[4] = {0, 0, 0, 0};
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gl_errors("glReadPixels");
  int ok = abs(px[0] - er) <= tol && abs(px[1] - eg) <= tol && abs(px[2] - eb) <= tol;
  if (ok) OK("probe", "%s at (%d,%d): got %d,%d,%d expected %d,%d,%d (tol %d)", what, x, y, px[0], px[1], px[2], er, eg, eb, tol);
  else FAIL("probe", "%s at (%d,%d): got %d,%d,%d,%d expected %d,%d,%d (tol %d)", what, x, y, px[0], px[1], px[2], px[3], er, eg, eb, tol);
}

/* G (premultiplied, alpha 128) composited over F with GL_ONE, GL_ONE_MINUS_SRC_ALPHA */
static void expected_g_over_f(int out[3]) {
  double inv = 1.0 - FLAT_G_PRE[3] / 255.0;
  for (int i = 0; i < 3; i++) out[i] = (int)lround(FLAT_G_PRE[i] + inv * FLAT_F[i]);
}

/* Draw: F full screen, G over the left half, a green 200x200 marker top-left. */
static void draw_selftest_scene(Ctx *c, const float *tf) {
  use_prog(&c->tex, tf);
  draw_quad(&c->tex, c->t_flat_f, 0, 0, VW, VH, 0, 0, 1, 1);
  draw_quad(&c->tex, c->t_flat_g, 0, 0, VW / 2, VH, 0, 0, 1, 1);
}

/* gl-origin pixel for a point given in fractions of the screen, image-top-down */
static void screen_px(const Ctx *c, double fx, double fy, int *px, int *py) {
  *px = (int)(fx * c->win_w);
  *py = c->win_h - 1 - (int)(fy * c->win_h);
}

static int selftest(Ctx *c) {
  int before = g_fail;
  float tf[16];
  int gx, gy, rx, ry, mx, my;
  int e_gf[3];
  expected_g_over_f(e_gf);
  screen_px(c, 0.25, 0.5, &gx, &gy);
  screen_px(c, 0.75, 0.5, &rx, &ry);
  screen_px(c, 0.02, 0.03, &mx, &my);

  /* 1. texture shader + premultiplied blending, straight to the window */
  stage("selftest.texture");
  make_transform(tf, 1);
  begin_screen(c, c->win_w, c->win_h);
  draw_selftest_scene(c, tf);
  /* orientation marker: opaque green top-left, drawn last */
  {
    GLuint green = make_flat_texture(0, 255, 0, 255);
    draw_quad(&c->tex, green, 0, 0, 200, 200, 0, 0, 1, 1);
    glDeleteTextures(1, &green);
  }
  glFinish();
  gl_errors("selftest.texture");
  probe("renpy.texture: F only (right half)", rx, ry, FLAT_F[0], FLAT_F[1], FLAT_F[2], 2);
  probe("renpy.texture: G over F (left half, premultiplied blend)", gx, gy, e_gf[0], e_gf[1], e_gf[2], 3);
  probe("orientation: green marker is at the TOP-left of the screen", mx, my, 0, 255, 0, 2);

  /* 2. render to FBO, glCopyTexImage2D, then renpy.matrixcolor (swap R and B) */
  stage("selftest.fbo_matrixcolor");
  make_transform(tf, 0);
  change_fbo(c, c->fbo);
  allocate_texture(c->t_copy, VW, VH);
  glViewport(0, 0, VW, VH);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  blend_default();
  draw_selftest_scene(c, tf);
  glBindTexture(GL_TEXTURE_2D, c->t_copy);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, VW, VH, 0);
  mipmap_texture(c->t_copy);
  change_fbo(c, (GLuint)c->default_fbo);
  make_transform(tf, 1);
  begin_screen(c, c->win_w, c->win_h);
  use_prog(&c->tex_mc, tf);
  {
    const float swap_rb[16] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1}; /* column-major */
    glUniformMatrix4fv(c->tex_mc.u_matrixcolor, 1, GL_FALSE, swap_rb);
  }
  draw_quad(&c->tex_mc, c->t_copy, 0, 0, VW, VH, 0, 0, 1, 1);
  glFinish();
  gl_errors("selftest.fbo_matrixcolor");
  probe("FBO->copy->matrixcolor(swap R/B): F", rx, ry, FLAT_F[2], FLAT_F[1], FLAT_F[0], 3);
  probe("FBO->copy->matrixcolor(swap R/B): G over F", gx, gy, e_gf[2], e_gf[1], e_gf[0], 4);

  /* 3. mip-bias blur on the copied, mipmapped texture: flat areas must stay put */
  stage("selftest.blur");
  begin_screen(c, c->win_w, c->win_h);
  use_prog(&c->blur, tf);
  glUniform1f(c->blur.u_blur_log2, 3.0f);
  draw_quad(&c->blur, c->t_copy, 0, 0, VW, VH, 0, 0, 1, 1);
  glFinish();
  gl_errors("selftest.blur");
  probe("renpy.blur(log2=3) of mipmapped copy: F region", rx, ry, FLAT_F[0], FLAT_F[1], FLAT_F[2], 6);
  probe("renpy.blur(log2=3) of mipmapped copy: G over F region", gx, gy, e_gf[0], e_gf[1], e_gf[2], 6);

  return g_fail - before;
}

/* ===========================================================================
 * Benchmark
 * ======================================================================== */
static double now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

typedef struct {
  double avg, mn, p50, p95, p99, mx;
} Stats;

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static Stats stats_of(const double *v, int n) {
  Stats s = {0, 0, 0, 0, 0, 0};
  if (n <= 0) return s;
  double *c = malloc((size_t)n * sizeof *c);
  if (!c) return s;
  memcpy(c, v, (size_t)n * sizeof *c);
  qsort(c, (size_t)n, sizeof *c, cmp_d);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += c[i];
  s.avg = sum / n;
  s.mn = c[0];
  s.p50 = c[n / 2];
  s.p95 = c[(int)((n - 1) * 0.95)];
  s.p99 = c[(int)((n - 1) * 0.99)];
  s.mx = c[n - 1];
  free(c);
  return s;
}

typedef enum {
  T_SCENE_WIN,   /* scene drawn straight to the window at drawable size */
  T_SCENE_1080,  /* same, viewport limited to 1920x1080 (cost comparison when the drawable is larger) */
  T_FILL8,       /* 8 full-screen layers, fill-rate stress */
  T_FBO_MC,      /* scene -> FBO -> glCopyTexImage2D -> matrixcolor draw to the window, every frame */
  T_BLUR_CACHED, /* mipmapped texture prepared once; per frame only the renpy.blur full-screen draw */
  T_BLUR_RERENDER, /* every frame: scene -> FBO -> copy -> glGenerateMipmap -> renpy.blur draw */
  T_COUNT
} TestId;

static const char *test_name(int t) {
  static const char *n[] = {"scene_window", "scene_1080_viewport", "fill8_window", "fbo_matrixcolor", "blur_cached", "blur_rerender"};
  return n[t];
}

typedef struct {
  int quit;     /* user asked to stop (SDL_QUIT, Escape, pad button) */
  int frames;
  int hold_ms;
  const char *api;
  const char *screenshot;
  int vsync_mode; /* 0, 1, 2 = both */
  int fullscreen;
  int preflight;
  int dump_shaders;
  int selftest_only;
  int probe_calls;
  int skip_flip_probe;
} Opts;

static void pump_events(Opts *o) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) { INFO("event", "SDL_QUIT"); o->quit = 1; }
    else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { INFO("event", "Escape"); o->quit = 1; }
    else if (e.type == SDL_JOYBUTTONDOWN) { INFO("event", "joystick button %d: stopping early", (int)e.jbutton.button); o->quit = 1; }
  }
}

typedef struct {
  int vsync;
  Stats render, period, swap;
  int frames;
} Result;

static Result g_results[2][3][T_COUNT]; /* [api][vsync idx][test] */
static int g_result_valid[2][3][T_COUNT];

static void render_frame(Ctx *c, int test, double t, double *stage_ms) {
  float tf[16];
  switch (test) {
  case T_SCENE_WIN:
    make_transform(tf, 1);
    begin_screen(c, c->win_w, c->win_h);
    draw_scene(c, tf, t);
    break;
  case T_SCENE_1080:
    make_transform(tf, 1);
    begin_screen(c, VW, VH);
    draw_scene(c, tf, t);
    break;
  case T_FILL8:
    make_transform(tf, 1);
    begin_screen(c, c->win_w, c->win_h);
    draw_fill(c, tf, t, 8);
    break;
  case T_FBO_MC: {
    render_to_texture(c, t, 0);
    make_transform(tf, 1);
    begin_screen(c, c->win_w, c->win_h);
    use_prog(&c->tex_mc, tf);
    const float m[16] = {1.0f, 0.0f, 0.0f, 0, 0.05f, 0.9f, 0.05f, 0, 0, 0.1f, 0.9f, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(c->tex_mc.u_matrixcolor, 1, GL_FALSE, m);
    draw_quad(&c->tex_mc, c->t_copy, 0, 0, VW, VH, 0, 0, 1, 1);
    break;
  }
  case T_BLUR_CACHED:
    make_transform(tf, 1);
    begin_screen(c, c->win_w, c->win_h);
    use_prog(&c->blur, tf);
    glUniform1f(c->blur.u_blur_log2, (float)(2.5 + 1.5 * sin(t)));
    draw_quad(&c->blur, c->t_copy, 0, 0, VW, VH, 0, 0, 1, 1);
    break;
  case T_BLUR_RERENDER: {
    double t0 = now_ms();
    render_to_texture(c, t, 1); /* Ren'Py re-specifies the whole mip chain for every new render-to-texture */
    if (stage_ms) { glFinish(); stage_ms[0] += now_ms() - t0; t0 = now_ms(); }
    mipmap_texture(c->t_copy);
    if (stage_ms) { glFinish(); stage_ms[1] += now_ms() - t0; t0 = now_ms(); }
    make_transform(tf, 1);
    begin_screen(c, c->win_w, c->win_h);
    use_prog(&c->blur, tf);
    glUniform1f(c->blur.u_blur_log2, (float)(2.5 + 1.5 * sin(t)));
    draw_quad(&c->blur, c->t_copy, 0, 0, VW, VH, 0, 0, 1, 1);
    if (stage_ms) { glFinish(); stage_ms[2] += now_ms() - t0; }
    break;
  }
  }
}

static void run_test(SDL_Window *win, Ctx *c, Opts *o, int api_idx, int vs_idx, int vsync, int test) {
  int n = o->frames;
  double *rt = malloc((size_t)n * sizeof(double)), *pt = malloc((size_t)n * sizeof(double)), *st = malloc((size_t)n * sizeof(double));
  if (!rt || !pt || !st) {
    FAIL("bench.alloc", "out of memory");
    free(rt); free(pt); free(st);
    return;
  }
  stage(test_name(test));
  if (test == T_BLUR_CACHED) {
    /* prepare the mipmapped source once */
    render_to_texture(c, 0.5, 1);
    mipmap_texture(c->t_copy);
  } else if (test == T_FBO_MC) {
    allocate_texture(c->t_copy, VW, VH);
  } else if (test == T_BLUR_RERENDER) {
    allocate_texture(c->t_copy, VW, VH);
  }
  /* warm-up: first draws compile/specialise shaders in the driver */
  double w0 = now_ms();
  for (int i = 0; i < 3; i++) {
    render_frame(c, test, 0.1 * i, NULL);
    glFinish();
    SDL_GL_SwapWindow(win);
    g_tick++;
  }
  double warm = now_ms() - w0;
  int errs = gl_errors(test_name(test));
  double prev = now_ms(), start = prev;
  int done = 0;
  for (int i = 0; i < n; i++) {
    pump_events(o);
    if (o->quit) break;
    double t0 = now_ms();
    render_frame(c, test, (now_ms() - start) / 1000.0, NULL);
    glFinish();
    double t1 = now_ms();
    SDL_GL_SwapWindow(win);
    double t2 = now_ms();
    rt[i] = t1 - t0;
    st[i] = t2 - t1;
    pt[i] = t2 - prev;
    prev = t2;
    g_tick++;
    done++;
  }
  errs += gl_errors(test_name(test));
  Result *r = &g_results[api_idx][vs_idx][test];
  r->vsync = vsync;
  r->frames = done;
  r->render = stats_of(rt, done);
  r->period = stats_of(pt, done);
  r->swap = stats_of(st, done);
  g_result_valid[api_idx][vs_idx][test] = done > 0;
  INFO("bench", "%-20s api=%s vsync=%d frames=%d warmup3=%.1fms | render(to glFinish) avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f ms | swap avg=%.2f max=%.2f ms | frame period avg=%.2f p95=%.2f max=%.2f ms (%.1f fps) | glerrors=%d",
       test_name(test), o->api, vsync, done, warm, r->render.avg, r->render.p50, r->render.p95, r->render.p99, r->render.mx,
       r->swap.avg, r->swap.mx, r->period.avg, r->period.p95, r->period.mx, r->period.avg > 0 ? 1000.0 / r->period.avg : 0.0, errs);
  if (errs) FAIL("bench.glerrors", "%s produced GL errors", test_name(test));
  free(rt); free(pt); free(st);
}

/* per-stage breakdown of the worst case, with glFinish between stages */
static void run_blur_stages(SDL_Window *win, Ctx *c, Opts *o) {
  int n = o->frames < 60 ? o->frames : 60;
  double ms[3] = {0, 0, 0};
  stage("blur_stages");
  allocate_texture(c->t_copy, VW, VH);
  int done = 0;
  for (int i = 0; i < n; i++) {
    pump_events(o);
    if (o->quit) break;
    render_frame(c, T_BLUR_RERENDER, 0.05 * i, ms);
    SDL_GL_SwapWindow(win);
    g_tick++;
    done++;
  }
  if (done)
    INFO("bench.stages", "blur_rerender per-stage wall time over %d frames (glFinish after each stage): render scene+glCopyTexImage2D=%.2f ms, glGenerateMipmap=%.2f ms, blur draw=%.2f ms",
         done, ms[0] / done, ms[1] / done, ms[2] / done);
  gl_errors("blur_stages");
}

static void print_summary(int api_idx, const char *api, int vs_idx, int vsync) {
  for (int t = 0; t < T_COUNT; t++) {
    if (!g_result_valid[api_idx][vs_idx][t]) continue;
    const Result *r = &g_results[api_idx][vs_idx][t];
    say("02B RESULT api=%s vsync=%d test=%s frames=%d render_avg_ms=%.2f render_p95_ms=%.2f render_max_ms=%.2f period_avg_ms=%.2f period_p95_ms=%.2f period_max_ms=%.2f fps=%.1f",
        api, vsync, test_name(t), r->frames, r->render.avg, r->render.p95, r->render.mx, r->period.avg, r->period.p95, r->period.mx,
        r->period.avg > 0 ? 1000.0 / r->period.avg : 0.0);
  }
}

/* ===========================================================================
 * Per-API driver
 * ======================================================================== */
typedef struct {
  const char *name;
  int es;           /* 1: GLES profile */
  int major, minor; /* 0 = leave to SDL */
  int profile;      /* SDL_GL_CONTEXT_PROFILE_* or 0 */
} Variant;

static const Variant GL_VARIANTS[] = {
    {"renpy gl2: 2.0 compatibility", 0, 2, 0, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY},
    {"sdl default (no version/profile)", 0, 0, 0, 0},
    {"3.3 core", 0, 3, 3, SDL_GL_CONTEXT_PROFILE_CORE},
    {NULL, 0, 0, 0, 0}};
static const Variant GLES_VARIANTS[] = {
    {"renpy gles2: ES 3.0", 1, 3, 0, SDL_GL_CONTEXT_PROFILE_ES},
    {"ES 2.0", 1, 2, 0, SDL_GL_CONTEXT_PROFILE_ES},
    {"ES 3.2", 1, 3, 2, SDL_GL_CONTEXT_PROFILE_ES},
    {NULL, 0, 0, 0, 0}};

static void select_attributes(const Variant *v, int vsync_hint) {
  (void)vsync_hint;
  SDL_GL_ResetAttributes();
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); /* renpy.config.depth_size = 24 */
  SDL_SetHint("SDL_OPENGL_ES_DRIVER", v->es ? "1" : "0");
  if (v->major) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v->major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v->minor);
  }
  if (v->profile) SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, v->profile);
}

static void log_gl_attributes(SDL_Window *win) {
  int r, g, b, a, d, s, maj, min, prof, db;
  SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
  SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
  SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
  SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d);
  SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &s);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &maj);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &min);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &prof);
  SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &db);
  INFO("gl.attributes", "obtained: rgba=%d/%d/%d/%d depth=%d stencil=%d version=%d.%d profile_mask=0x%x doublebuffer=%d", r, g, b, a, d, s,
       maj, min, prof, db);
  (void)win;
}

static void log_gl_info(Ctx *c) {
  const char *vendor = (const char *)glGetString(GL_VENDOR), *renderer = (const char *)glGetString(GL_RENDERER),
             *version = (const char *)glGetString(GL_VERSION), *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION),
             *ext = (const char *)glGetString(GL_EXTENSIONS);
  INFO("gl.vendor", "%s", vendor ? vendor : "(null)");
  INFO("gl.renderer", "%s", renderer ? renderer : "(null)");
  INFO("gl.version", "%s", version ? version : "(null)");
  INFO("gl.sl_version", "%s", sl ? sl : "(null)");
  GLint i1 = 0, units = 0, attribs = 0;
  GLfloat bias = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &i1);
  INFO("gl.limit", "GL_MAX_TEXTURE_SIZE=%d", i1);
  glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &i1);
  INFO("gl.limit", "GL_MAX_RENDERBUFFER_SIZE=%d", i1);
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
  glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribs);
  glGetFloatv(GL_MAX_TEXTURE_LOD_BIAS, &bias);
  INFO("gl.limit", "texture image units=%d, vertex attribs=%d, max lod bias=%.1f", units, attribs, bias);
  gl_errors("limits");
  if (ext) {
    int count = 0;
    const char *p = ext;
    while (*p) {
      const char *sp = strchr(p, ' ');
      size_t n = sp ? (size_t)(sp - p) : strlen(p);
      if (n) { say("02B EXT %.*s", (int)n, p); count++; }
      if (!sp) break;
      p = sp + 1;
    }
    INFO("gl.extensions", "%d extensions listed (glGetString(GL_EXTENSIONS), as Ren'Py reads them)", count);
  } else {
    INFO("gl.extensions", "glGetString(GL_EXTENSIONS) returned NULL");
  }
  (void)c;
}

static int run_api(Opts *o, int api_idx, const char *api) {
  const int gles = !strcmp(api, "gles");
  const Variant *variants = gles ? GLES_VARIANTS : GL_VARIANTS;
  SDL_Window *win = NULL;
  SDL_GLContext ctx = NULL;
  Ctx *c = calloc(1, sizeof *c);
  int fails_before = g_fail;
  uint8_t *bg = NULL, *sa = NULL, *sb = NULL, *box = NULL;
  o->api = api;
  c->gles = gles;
  stage("api.begin");
  say("02B ==== API %s (%s) ====", api, gles ? "Ren'Py renderer gles2: OpenGL ES 3.0 + GLSL ES 1.00 shaders" : "Ren'Py renderer gl2: OpenGL 2.0 compatibility + GLSL 1.20 shaders");

  const Variant *used = NULL;
  for (const Variant *v = variants; v->name; v++) {
    stage("window.create");
    select_attributes(v, 1);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | (o->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    INFO("gl.request", "trying variant '%s' (fullscreen_desktop=%d)", v->name, o->fullscreen);
    win = SDL_CreateWindow("ksre-ps5 02b", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VW, VH, flags);
    if (!win && o->fullscreen) {
      INFO("window.create", "fullscreen-desktop window failed (%s); retrying windowed like Ren'Py does", SDL_GetError());
      win = SDL_CreateWindow("ksre-ps5 02b", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VW, VH, flags & ~SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    if (!win) {
      FAIL_SDL("window.create");
      continue;
    }
    stage("context.create");
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
      FAIL_SDL("context.create");
      SDL_DestroyWindow(win);
      win = NULL;
      continue;
    }
    used = v;
    OK("context.create", "variant '%s'", v->name);
    break;
  }
  if (!used) {
    FAIL("api", "%s: no GL context could be created with any request variant", api);
    free(c);
    return -1;
  }
  if (used != variants) INFO("gl.request", "NOTE: Ren'Py's own request did not work; fell back to variant '%s'", used->name);

  {
    int w, h, dw, dh;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    c->win_w = dw;
    c->win_h = dh;
    INFO("window", "size=%dx%d drawable=%dx%d flags=0x%x", w, h, dw, dh, SDL_GetWindowFlags(win));
  }
  log_gl_attributes(win);
  stage("context.current");
  if (SDL_GL_MakeCurrent(win, ctx) != 0) FAIL_SDL("context.make_current");

  stage("gl.load");
  if (load_gl_functions() != 0) {
    FAIL("gl.load", "missing required functions; cannot continue with %s", api);
    goto cleanup;
  }
  OK("gl.load", "all of Ren'Py's required GL functions resolved");
  log_gl_info(c);
  if (o->probe_calls) probe_calls();

  /* content */
  stage("content.generate");
  bg = make_bg(VW, VH);
  sa = make_sprite(640, 900, 230, 90, 110);
  sb = make_sprite(640, 900, 90, 200, 230);
  box = make_box(1760, 300);
  INFO("content", "textures generated on the CPU");
  stage("content.upload");
  if (!bg || !sa || !sb || !box) {
    FAIL("content", "out of memory generating textures");
    goto cleanup;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glGenTextures(1, &c->t_bg);
  upload(c->t_bg, VW, VH, bg, 1);
  glGenTextures(1, &c->t_sprite_a);
  upload(c->t_sprite_a, 640, 900, sa, 1);
  glGenTextures(1, &c->t_sprite_b);
  upload(c->t_sprite_b, 640, 900, sb, 1);
  glGenTextures(1, &c->t_box);
  upload(c->t_box, 1760, 300, box, 0);
  c->t_flat_f = make_flat_texture(FLAT_F[0], FLAT_F[1], FLAT_F[2], FLAT_F[3]);
  c->t_flat_g = make_flat_texture(FLAT_G_PRE[0], FLAT_G_PRE[1], FLAT_G_PRE[2], FLAT_G_PRE[3]);
  glGenTextures(1, &c->t_copy);
  if (gl_errors("texture upload")) goto cleanup;
  OK("content", "uploaded procedural textures (mipmapped background %dx%d, sprites, box)", VW, VH);

  /* shaders: the program combinations Ren'Py builds for these draws */
  stage("shaders");
  {
    const ShaderPart *tex_parts[] = {&P_GEOMETRY, &P_TEXTURE};
    const ShaderPart *alpha_parts[] = {&P_ALPHA, &P_GEOMETRY, &P_TEXTURE}; /* sorted: renpy.alpha < renpy.geometry < renpy.texture */
    const ShaderPart *mc_parts[] = {&P_GEOMETRY, &P_MATRIXCOLOR, &P_TEXTURE};
    const ShaderPart *blur_parts[] = {&P_BLUR, &P_GEOMETRY};
    int bad = 0;
    bad |= build_program(&c->tex, "geometry+texture", tex_parts, 2, gles, o->dump_shaders);
    bad |= build_program(&c->tex_alpha, "alpha+geometry+texture", alpha_parts, 3, gles, o->dump_shaders);
    bad |= build_program(&c->tex_mc, "geometry+matrixcolor+texture", mc_parts, 3, gles, o->dump_shaders);
    bad |= build_program(&c->blur, "blur+geometry", blur_parts, 2, gles, o->dump_shaders);
    if (bad) {
      FAIL("shaders", "%s: shader build failed; skipping drawing", api);
      goto cleanup;
    }
  }

  stage("fbo");
  if (init_fbo(c) != 0) goto cleanup;
  glDisable(GL_DEPTH_TEST);

  /* correctness first */
  if (selftest(c) == 0) OK("selftest", "%s: texture shader, premultiplied blend, FBO copy, matrixcolor and blur read back correctly", api);
  else FAIL("selftest", "%s: read-back values differ from the CPU-computed expectation", api);

  if (!o->selftest_only && !o->quit) {
    int vs_list[2], nvs = 0;
    if (o->vsync_mode == 1 || o->vsync_mode == 2) vs_list[nvs++] = 1;
    if (o->vsync_mode == 0 || o->vsync_mode == 2) vs_list[nvs++] = 0;
    for (int vi = 0; vi < nvs && !o->quit; vi++) {
      int want = vs_list[vi];
      stage("swap_interval");
      if (SDL_GL_SetSwapInterval(want) != 0) {
        INFO("swap_interval", "SDL_GL_SetSwapInterval(%d) failed: %s", want, SDL_GetError());
        if (want == 0) { INFO("swap_interval", "skipping the unpaced pass"); continue; }
      }
      INFO("swap_interval", "requested %d, SDL_GL_GetSwapInterval() = %d", want, SDL_GL_GetSwapInterval());
      for (int t = 0; t < T_COUNT && !o->quit; t++) {
        if (t == T_SCENE_1080 && c->win_w == VW && c->win_h == VH) continue; /* identical to scene_window */
        run_test(win, c, o, api_idx, vi, want, t);
      }
      if (!o->quit) run_blur_stages(win, c, o);
      print_summary(api_idx, api, vi, want);
    }
  }

  /* leave a picture on the TV for the maintainer, and a host screenshot aid */
  if (!o->quit && o->hold_ms > 0) {
    stage("showcase");
    INFO("showcase", "animated scene for %d ms (look at the TV: test card, 2 sprites, text box; circle round, 4 coloured corner squares)", o->hold_ms);
    double start = now_ms();
    int shot_done = 0;
    while (!o->quit && now_ms() - start < o->hold_ms) {
      pump_events(o);
      float tf[16];
      make_transform(tf, 1);
      begin_screen(c, c->win_w, c->win_h);
      draw_scene(c, tf, (now_ms() - start) / 1000.0);
      if (o->screenshot && !shot_done && now_ms() - start > o->hold_ms * 0.5) {
        size_t n = (size_t)c->win_w * (size_t)c->win_h * 4;
        uint8_t *rb = malloc(n);
        SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormat(0, c->win_w, c->win_h, 32, SDL_PIXELFORMAT_ABGR8888);
        if (rb && ss) {
          glPixelStorei(GL_PACK_ALIGNMENT, 1);
          glReadPixels(0, 0, c->win_w, c->win_h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
          for (int y = 0; y < c->win_h; y++)
            memcpy((uint8_t *)ss->pixels + (size_t)y * ss->pitch, rb + (size_t)(c->win_h - 1 - y) * c->win_w * 4, (size_t)c->win_w * 4);
          char path[512];
          snprintf(path, sizeof path, "%s.%s.bmp", o->screenshot, api);
          if (SDL_SaveBMP(ss, path) == 0) INFO("screenshot", "saved %s", path);
          else FAIL_SDL("screenshot");
        }
        SDL_FreeSurface(ss);
        free(rb);
        shot_done = 1;
      }
      SDL_GL_SwapWindow(win);
      g_tick++;
    }
  }

cleanup:
  stage("api.cleanup");
  free(bg); free(sa); free(sb); free(box);
  if (glDeleteTextures && c->t_bg) {
    GLuint ts[] = {c->t_bg, c->t_sprite_a, c->t_sprite_b, c->t_box, c->t_flat_f, c->t_flat_g, c->t_copy};
    glDeleteTextures(7, ts);
    if (c->fbo) glDeleteFramebuffers(1, &c->fbo);
    if (c->rb_color) glDeleteRenderbuffers(1, &c->rb_color);
    if (c->rb_depth) glDeleteRenderbuffers(1, &c->rb_depth);
  }
  if (ctx) SDL_GL_DeleteContext(ctx);
  if (win) SDL_DestroyWindow(win);
  INFO("api.done", "%s finished with %d failure(s)", api, g_fail - fails_before);
  free(c);
  return g_fail - fails_before;
}

/* ===========================================================================
 * main
 * ======================================================================== */
static void preflight_libs(void) {
#if defined(__PROSPERO__)
  static const char *libs[] = {"libEGL.so.1", "libGL.so.1", "libGLESv2.so.2", "libgallium-26.2.0.so", "libvulkan.so.1", "libOSMesa.so.8", NULL};
  char cwd[256];
  if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
  for (int i = 0; libs[i]; i++) {
    char path[320];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", cwd, libs[i]);
    if (stat(path, &st) == 0) INFO("libs.cwd", "%s present (%lld bytes)", libs[i], (long long)st.st_size);
    else INFO("libs.cwd", "%s absent in %s", libs[i], cwd);
  }
#endif
}

#if defined(__PROSPERO__)
static void dlopen_probe(const char *name, int flags) {
  stage("preflight.dlopen");
  say("02B INFO preflight :: dlopen(\"%s\", 0x%x) ...", name, flags);
  void *h = dlopen(name, flags);
  if (h) {
    INFO("preflight", "dlopen(%s) ok handle=%p", name, h);
    /* Where the library landed: lets a crash address be mapped back to a function offline
     * (compare with `llvm-nm` addresses of the same symbols in the unstripped library). */
    g_osmesa_handle = h;
    static const char *syms[] = {"OSMesaCreateContext", "OSMesaMakeCurrent", "glGenerateMipmap", "glDrawElements", NULL};
    for (int i = 0; syms[i]; i++) {
      void *p = dlsym(h, syms[i]);
      if (p) {
        Dl_info pd;
        if (dladdr(p, &pd) && pd.dli_fname)
          INFO("preflight.sym", "%s = %p (module %s base %p)", syms[i], p, pd.dli_fname, pd.dli_fbase);
        else
          INFO("preflight.sym", "%s = %p", syms[i], p);
      }
    }
  }
  else {
    const char *e = dlerror();
    INFO("preflight", "dlopen(%s) FAILED: %s", name, e ? e : "(no dlerror)");
  }
}
#endif

static int app_main(int argc, char **argv) {
  Opts o;
  memset(&o, 0, sizeof o);
  o.frames = 300;
  o.hold_ms = 5000;
  o.vsync_mode = 2;
  o.fullscreen = 1;
  o.preflight = 1;
  o.api = "gl,gles";
  const char *api_list = "gl,gles";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--api") && i + 1 < argc) api_list = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--vsync") && i + 1 < argc) { const char *v = argv[++i]; o.vsync_mode = !strcmp(v, "both") ? 2 : atoi(v); }
    else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) o.hold_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--windowed")) o.fullscreen = 0;
    else if (!strcmp(argv[i], "--no-preflight")) o.preflight = 0;
    else if (!strcmp(argv[i], "--dump-shaders")) o.dump_shaders = 1;
    else if (!strcmp(argv[i], "--selftest-only")) o.selftest_only = 1;
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) o.screenshot = argv[++i];
    else if (!strcmp(argv[i], "--stall-ms") && i + 1 < argc) g_stall_ms = atol(argv[++i]);
    else if (!strcmp(argv[i], "--stack-mb") && i + 1 < argc) ++i; /* handled by main() */
    else if (!strcmp(argv[i], "--dump-procs")) g_dump_procs = 1;
    else if (!strcmp(argv[i], "--probe-calls")) o.probe_calls = 1;
    else if (!strcmp(argv[i], "--no-mipmap")) g_no_mipmap = 1;
    else if (!strcmp(argv[i], "--fast-exit")) g_fast_exit = 1;
    else if (!strcmp(argv[i], "--normal-exit")) g_fast_exit = 0;
  }
  if (o.frames < 10) o.frames = 10;
  if (o.frames > 20000) o.frames = 20000;

#if defined(__PROSPERO__)
  mkdir("/data/ksre-ps5", 0777);
  mkdir("/data/ksre-ps5/02b", 0777);
  g_logfile = fopen("/data/ksre-ps5/02b/last-run.log", "w");
#endif
#if defined(__PROSPERO__)
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

  say("KSRE_PS5_02B_BEGIN version=%s target=%s api=%s frames=%d vsync_mode=%d fullscreen=%d", VERSION, TARGET, api_list, o.frames,
      o.vsync_mode, o.fullscreen);
  {
    pthread_attr_t cur;
    size_t ssz = 0;
    void *saddr = NULL;
#if defined(__PROSPERO__)
    if (pthread_attr_init(&cur) == 0 && pthread_attr_get_np(pthread_self(), &cur) == 0 &&
        pthread_attr_getstack(&cur, &saddr, &ssz) == 0)
      INFO("stack", "this thread: base=%p size=%zu KiB", saddr, ssz / 1024);
    else
      INFO("stack", "could not query this thread's stack");
#else
    (void)cur; (void)ssz; (void)saddr;
#endif
  }
  notify("ksre-ps5 02b: OpenGL test started");
  for (int i = 0; i < argc; i++) INFO("argv", "[%d] %s", i, argv[i]);
  INFO("process", "pid=%d ppid=%d uid=%d", (int)getpid(), (int)getppid(), (int)getuid());
  {
    char cwd[256];
    INFO("process.cwd", "%s", getcwd(cwd, sizeof cwd) ? cwd : "(unknown)");
    const char *names[] = {"LD_LIBRARY_PATH", "EGL_PLATFORM", "EGL_LOG_LEVEL", "MESA_DEBUG", "LP_NUM_THREADS", "SDL_VIDEO_EGL_DRIVER", NULL};
    for (int i = 0; names[i]; i++) {
      const char *v = getenv(names[i]);
      INFO("env", "%s=%s", names[i], v ? v : "(unset)");
    }
  }
#if defined(__PROSPERO__)
  {
    unsigned int uid = 0;
    int fg = sceUserServiceGetForegroundUser(&uid);
    INFO("process.bigapp", "sceSystemServiceGetAppIdOfRunningBigApp=0x%x foreground_user_rc=0x%x", (unsigned)sceSystemServiceGetAppIdOfRunningBigApp(), (unsigned)fg);
  }
#endif
  preflight_libs();

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
  {
    SDL_version lv;
    SDL_GetVersion(&lv);
    INFO("sdl.version", "linked %d.%d.%d revision=%s platform=%s", lv.major, lv.minor, lv.patch, SDL_GetRevision(), SDL_GetPlatform());
  }

  stage("sdl.init");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    FAIL_SDL("sdl.init");
    goto done;
  }
  OK("sdl.init", "video driver=%s", SDL_GetCurrentVideoDriver());
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0) {
    int nj = SDL_NumJoysticks();
    INFO("joystick.count", "%d", nj);
    for (int i = 0; i < nj; i++) {
      SDL_Joystick *j = SDL_JoystickOpen(i);
      if (j) INFO("joystick.open", "#%d name=%s buttons=%d axes=%d", i, SDL_JoystickName(j), SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j));
    }
  }
  {
    int nd = SDL_GetNumVideoDisplays();
    INFO("display.count", "%d", nd);
    for (int d = 0; d < nd; d++) {
      SDL_DisplayMode m;
      if (SDL_GetDesktopDisplayMode(d, &m) == 0) INFO("display.desktop", "#%d %dx%d@%dHz %s", d, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
      int nm = SDL_GetNumDisplayModes(d);
      for (int i = 0; i < nm; i++)
        if (SDL_GetDisplayMode(d, i, &m) == 0) INFO("display.mode", "#%d[%d] %dx%d@%dHz %s", d, i, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
    }
  }

  if (o.preflight) {
#if defined(__PROSPERO__)
    /* Same call SDL makes first (libEGL, RTLD_GLOBAL), but with dlerror() text if the library
     * chain cannot load. The software bundle has no libEGL; it loads libOSMesa.so.8 instead. */
    dlopen_probe("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen_probe("libOSMesa.so.8", RTLD_LAZY);
#endif
  }

  {
    char *list = strdup(api_list), *save = NULL;
    int idx = 0;
    for (char *tok = strtok_r(list, ",", &save); tok && !o.quit; tok = strtok_r(NULL, ",", &save)) {
      if (strcmp(tok, "gl") && strcmp(tok, "gles")) {
        FAIL("args", "unknown --api entry '%s' (use gl, gles or gl,gles)", tok);
        continue;
      }
      run_api(&o, strcmp(tok, "gl") ? 1 : 0, tok);
      idx++;
    }
    (void)idx;
    free(list);
  }

done:
  stage("shutdown");
  SDL_Quit();
  INFO("shutdown", "SDL_Quit returned");
  g_main_done = 1;
  if (wd_up) pthread_join(wd, NULL);
  say("KSRE_PS5_02B_END fail=%d", g_fail);
  {
    char msg[96];
    snprintf(msg, sizeof msg, "ksre-ps5 02b: finished (%s)", g_fail ? "FAILURES" : "ok");
    notify(msg);
  }
  if (g_logfile) fclose(g_logfile);
  g_logfile = NULL;
  if (g_fast_exit) _exit(g_fail ? 1 : 0); /* skip atexit/destructors of the GL library */
  return g_fail ? 1 : 0;
}

#if defined(__PROSPERO__)
/* Mesa assumes a desktop-sized stack (8 MiB). The PS5 gives pthreads 64 KiB by default and the
 * main thread's size is not documented, so the whole test runs on a thread with an explicit big
 * stack. SDL's video code must stay on the main thread on macOS, so the host build does not do this. */
static int g_app_argc, g_app_ret;
static char **g_app_argv;

static void *app_thread(void *arg) {
  (void)arg;
  g_app_ret = app_main(g_app_argc, g_app_argv);
  return NULL;
}

int main(int argc, char **argv) {
  long stack_mb = 64;
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], "--stack-mb")) stack_mb = atol(argv[i + 1]);
  if (stack_mb <= 0) return app_main(argc, argv);
  g_app_argc = argc;
  g_app_argv = argv;
  pthread_attr_t at;
  pthread_t t;
  pthread_attr_init(&at);
  if (pthread_attr_setstacksize(&at, (size_t)stack_mb * 1024 * 1024) != 0) return app_main(argc, argv);
  if (pthread_create(&t, &at, app_thread, NULL) != 0) return app_main(argc, argv);
  pthread_attr_destroy(&at);
  pthread_join(t, NULL);
  return g_app_ret;
}
#else
int main(int argc, char **argv) { return app_main(argc, argv); }
#endif
