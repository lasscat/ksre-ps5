/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experiment 02a: SDL2 on the PS5 - an original generated test image plus
 * SDL2_ttf text, animated for a fixed time, then a clean exit.
 *
 * Builds for the PS5 (prospero-clang + pacbrew SDL2/SDL2_image/SDL2_ttf,
 * static) and for the host (Homebrew/apt SDL2; run it with
 * SDL_VIDEODRIVER=dummy for a headless logic test).
 *
 * Options:  --duration-ms N   animation length (default 10000)
 *           --screenshot F    host aid: save a BMP of the frame at 60% time
 *           --renderer NAME   SDL render driver (default "software"; the PS5
 *                             framebuffer path only supports software, and
 *                             hardware renderers would pull in Mesa = 02b)
 *
 * Log markers: KSRE_PS5_02A_BEGIN / KSRE_PS5_02A_END ... fail=N
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "1"
#define BASE_W 1920
#define BASE_H 1080

#if defined(__PROSPERO__)
#define TARGET "x86_64-sie-ps5"
#else
#define TARGET "host"
#endif

extern const unsigned char ksre_asset_image[];
extern const unsigned long ksre_asset_image_len;
extern const unsigned char ksre_asset_font[];
extern const unsigned long ksre_asset_font_len;

static int g_fail;
static FILE *g_logfile; /* PS5: /data/ksre-ps5/02a/last-run.log (backup when stdout is not connected) */

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

#define OK(what, ...) say("02A PASS " what " :: " __VA_ARGS__)
#define INFO(what, ...) say("02A INFO " what " :: " __VA_ARGS__)
#define FAIL(what, ...)                                                        \
  do {                                                                         \
    g_fail++;                                                                  \
    say("02A FAIL " what " :: " __VA_ARGS__);                                  \
  } while (0)
/* SDL_GetError() is read once, right after the failing call. */
#define FAIL_SDL(what)                                                         \
  do {                                                                         \
    const char *e_ = SDL_GetError();                                           \
    g_fail++;                                                                  \
    say("02A FAIL " what " :: SDL_GetError: %s", e_ && *e_ ? e_ : "(empty)");  \
  } while (0)

/* ---- notifications (PS5 only, same call the probe uses) ------------------ */
#if defined(__PROSPERO__)
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceUserServiceGetForegroundUser(unsigned int *user_id);
#endif

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

static void log_output(void *ud, int cat, SDL_LogPriority pri, const char *msg) {
  (void)ud;
  say("02A SDLLOG [cat=%d pri=%d] %s", cat, (int)pri, msg);
}


/* ---- watchdog ------------------------------------------------------------
 * Presenting on the PS5 blocks on a kernel flip event. If anything stalls
 * there, this thread ends the process so the loader and the display are not
 * left hanging. Explicit stack size: the default pthread stack is only 64 KiB
 * on this platform (console fact from 01b). */
static volatile int g_main_done;
static long g_watchdog_ms;

static void *watchdog_main(void *arg) {
  (void)arg;
  for (long waited = 0; waited < g_watchdog_ms; waited += 100) {
    if (g_main_done) return NULL;
    usleep(100 * 1000);
  }
  if (!g_main_done) {
    say("02A FAIL watchdog :: main thread still running after %ld ms; _exit(3)", g_watchdog_ms);
    say("KSRE_PS5_02A_END frames=-1 fail=%d watchdog=1", g_fail + 1);
    _exit(3);
  }
  return NULL;
}

/* ---- text helper --------------------------------------------------------- */
typedef struct {
  SDL_Texture *tex;
  int w, h;
} Text;

static Text make_text(SDL_Renderer *r, TTF_Font *font, const char *utf8, SDL_Color c) {
  Text t = {NULL, 0, 0};
  SDL_Surface *s = TTF_RenderUTF8_Blended(font, utf8, c);
  if (!s) {
    FAIL_SDL("ttf.render");
    return t;
  }
  t.tex = SDL_CreateTextureFromSurface(r, s);
  if (!t.tex) {
    FAIL_SDL("ttf.texture");
  }
  t.w = s->w;
  t.h = s->h;
  SDL_FreeSurface(s);
  return t;
}

static void draw_text(SDL_Renderer *r, const Text *t, int x, int y) {
  if (!t->tex) return;
  SDL_Rect d = {x, y, t->w, t->h};
  SDL_RenderCopy(r, t->tex, NULL, &d);
}

static void panel(SDL_Renderer *r, int x, int y, int w, int h) {
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
  SDL_Rect rc = {x, y, w, h};
  SDL_RenderFillRect(r, &rc);
}

/* ---- logging of display / renderer facts -------------------------------- */
static void log_displays(void) {
  int n = SDL_GetNumVideoDisplays();
  INFO("display.count", "%d", n);
  for (int d = 0; d < n; d++) {
    SDL_Rect b;
    SDL_DisplayMode m;
    if (SDL_GetDisplayBounds(d, &b) == 0)
      INFO("display.bounds", "#%d name=%s %d,%d %dx%d", d, SDL_GetDisplayName(d), b.x, b.y, b.w, b.h);
    else
      FAIL_SDL("display.bounds");
    if (SDL_GetDesktopDisplayMode(d, &m) == 0)
      INFO("display.desktop", "#%d %dx%d@%dHz %s", d, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
    else
      FAIL_SDL("display.desktop");
    if (SDL_GetCurrentDisplayMode(d, &m) == 0)
      INFO("display.current", "#%d %dx%d@%dHz %s", d, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
    else
      FAIL_SDL("display.current");
    int nm = SDL_GetNumDisplayModes(d);
    INFO("display.modes", "#%d count=%d", d, nm);
    for (int i = 0; i < nm; i++) {
      if (SDL_GetDisplayMode(d, i, &m) == 0)
        INFO("display.mode", "#%d[%d] %dx%d@%dHz %s", d, i, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
    }
    float ddpi = 0, hdpi = 0, vdpi = 0;
    if (SDL_GetDisplayDPI(d, &ddpi, &hdpi, &vdpi) == 0)
      INFO("display.dpi", "#%d %.1f/%.1f/%.1f", d, ddpi, hdpi, vdpi);
    else
      INFO("display.dpi", "unavailable: %s", SDL_GetError());
  }
}

static int log_renderers_and_find(const char *want) {
  int n = SDL_GetNumRenderDrivers(), found = -1;
  INFO("render.drivers", "%d available", n);
  for (int i = 0; i < n; i++) {
    SDL_RendererInfo ri;
    if (SDL_GetRenderDriverInfo(i, &ri) == 0) {
      INFO("render.driver", "#%d %s flags=0x%x tex_formats=%u max_tex=%dx%d", i, ri.name, ri.flags,
           ri.num_texture_formats, ri.max_texture_width, ri.max_texture_height);
      if (strcmp(ri.name, want) == 0) found = i;
    }
  }
  return found;
}

static void log_joysticks(void) {
  int n = SDL_NumJoysticks();
  INFO("joystick.count", "%d", n);
  for (int i = 0; i < n; i++) {
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
    SDL_Joystick *j = SDL_JoystickOpen(i);
    if (!j) {
      INFO("joystick.open", "#%d failed: %s", i, SDL_GetError());
      continue;
    }
    INFO("joystick.open", "#%d name=%s guid=%s buttons=%d axes=%d hats=%d", i, SDL_JoystickName(j), guid,
         SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j));
  }
}

/* ---- main ---------------------------------------------------------------- */
int main(int argc, char **argv) {
  int duration_ms = 10000;
  const char *shot = NULL, *renderer_name = "software";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--duration-ms") && i + 1 < argc) duration_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) renderer_name = argv[++i];
  }

#if defined(__PROSPERO__)
  {
    /* Best-effort backup log on persistent storage (stdout may not be connected). */
    mkdir("/data/ksre-ps5", 0777);
    mkdir("/data/ksre-ps5/02a", 0777);
    g_logfile = fopen("/data/ksre-ps5/02a/last-run.log", "w");
  }
#endif
  say("KSRE_PS5_02A_BEGIN version=%s target=%s duration_ms=%d renderer=%s", VERSION, TARGET, duration_ms,
      renderer_name);
  notify("ksre-ps5 02a: SDL display test started");

  /* Which kind of process are we? A payload started by elfldr is a background
   * process; one started by a homebrew launcher is a foreground "big app".
   * Evidence for the display-ownership question (see README). */
  INFO("process", "pid=%d ppid=%d uid=%d argv0=%s", (int)getpid(), (int)getppid(), (int)getuid(),
       argc > 0 ? argv[0] : "?");
  {
    char cwd[256];
    INFO("process.cwd", "%s", getcwd(cwd, sizeof cwd) ? cwd : "(unknown)");
  }
#if defined(__PROSPERO__)
  {
    unsigned int uid = 0;
    int fg = sceUserServiceGetForegroundUser(&uid);
    INFO("process.bigapp", "sceSystemServiceGetAppIdOfRunningBigApp=0x%x foreground_user_rc=0x%x user=0x%x",
         (unsigned)sceSystemServiceGetAppIdOfRunningBigApp(), (unsigned)fg, uid);
  }
#endif

  SDL_Window *win = NULL;
  SDL_Renderer *ren = NULL;
  SDL_Texture *image = NULL;
  TTF_Font *font = NULL, *font_big = NULL;
  Text title = {0}, l_ver = {0}, l_disp = {0}, l_utf = {0}, l_dyn = {0};
  int sdl_up = 0, img_up = 0, ttf_up = 0;
  unsigned long frames = 0;
  double worst_ms = 0, total_ms = 0;
  Uint64 t_start = 0;
  int quit_reason = 0; /* 0 timeout, 1 quit, 2 key, 3 joystick */

  pthread_t wd;
  int wd_up = 0;
  {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    g_watchdog_ms = (long)duration_ms + 20000;
    if (pthread_create(&wd, &at, watchdog_main, NULL) == 0) {
      wd_up = 1;
      INFO("watchdog", "armed for %ld ms", g_watchdog_ms);
    } else {
      INFO("watchdog", "could not start (non-fatal)");
    }
    pthread_attr_destroy(&at);
  }

  SDL_SetMainReady();
  SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
  SDL_LogSetOutputFunction(log_output, NULL);
  /* The PS5 window may never get keyboard focus; do not drop pad events. */
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

  SDL_version cv, lv;
  SDL_VERSION(&cv);
  SDL_GetVersion(&lv);
  INFO("sdl.version", "compiled %d.%d.%d linked %d.%d.%d revision=%s", cv.major, cv.minor, cv.patch, lv.major,
       lv.minor, lv.patch, SDL_GetRevision());
  const SDL_version *iv = IMG_Linked_Version(), *tv = TTF_Linked_Version();
  INFO("sdl.image.version", "%d.%d.%d", iv->major, iv->minor, iv->patch);
  INFO("sdl.ttf.version", "%d.%d.%d", tv->major, tv->minor, tv->patch);
  INFO("platform", "%s", SDL_GetPlatform());

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    FAIL_SDL("sdl.init");
    goto done;
  }
  sdl_up = 1;
  OK("sdl.init", "video driver=%s", SDL_GetCurrentVideoDriver());
  for (int i = 0; i < SDL_GetNumVideoDrivers(); i++) INFO("sdl.videodriver", "#%d %s", i, SDL_GetVideoDriver(i));

  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0) {
    log_joysticks();
  } else {
    INFO("joystick.init", "failed (non-fatal): %s", SDL_GetError());
  }

  log_displays();

  win = SDL_CreateWindow("ksre-ps5 02a", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, BASE_W, BASE_H,
                         SDL_WINDOW_SHOWN);
  if (!win) {
    FAIL_SDL("window.create");
    goto done;
  }
  {
    int w, h, pw, ph, x, y;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    SDL_GetWindowPosition(win, &x, &y);
    OK("window.create", "size=%dx%d pixels=%dx%d pos=%d,%d flags=0x%x keyboard_focus=%d", w, h, pw, ph, x, y,
       SDL_GetWindowFlags(win), SDL_GetKeyboardFocus() == win);
  }

  int idx = log_renderers_and_find(renderer_name);
  if (idx < 0) {
    FAIL("render.find", "no render driver named '%s'", renderer_name);
    goto done;
  }
  /* An explicit index never falls back to another driver, so a software
   * request cannot silently load Mesa/EGL. */
  ren = SDL_CreateRenderer(win, idx, strcmp(renderer_name, "software") == 0 ? SDL_RENDERER_SOFTWARE : 0);
  if (!ren) {
    FAIL_SDL("render.create");
    goto done;
  }
  {
    SDL_RendererInfo ri;
    int ow, oh;
    if (SDL_GetRendererInfo(ren, &ri) == 0) INFO("render.info", "%s flags=0x%x", ri.name, ri.flags);
    if (SDL_GetRendererOutputSize(ren, &ow, &oh) == 0) INFO("render.output", "%dx%d", ow, oh);
    OK("render.create", "driver=%s", ri.name);
  }

  /* --- original test image, decoded from embedded PNG with SDL2_image --- */
  if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
    FAIL("image.init", "IMG_Init(PNG): %s", IMG_GetError());
  } else {
    img_up = 1;
    SDL_RWops *rw = SDL_RWFromConstMem(ksre_asset_image, (int)ksre_asset_image_len);
    SDL_Surface *s = IMG_Load_RW(rw, 1);
    if (!s) {
      FAIL("image.load", "IMG_Load_RW: %s", IMG_GetError());
    } else {
      OK("image.load", "%dx%d %s (%lu PNG bytes)", s->w, s->h, SDL_GetPixelFormatName(s->format->format),
         ksre_asset_image_len);
      image = SDL_CreateTextureFromSurface(ren, s);
      if (!image) FAIL_SDL("image.texture");
      SDL_FreeSurface(s);
    }
  }

  /* --- text with SDL2_ttf; strings are UTF-8 regardless of the C locale --- */
  if (TTF_Init() != 0) {
    FAIL("ttf.init", "TTF_Init: %s", TTF_GetError());
  } else {
    ttf_up = 1;
    font = TTF_OpenFontRW(SDL_RWFromConstMem(ksre_asset_font, (int)ksre_asset_font_len), 1, 30);
    font_big = TTF_OpenFontRW(SDL_RWFromConstMem(ksre_asset_font, (int)ksre_asset_font_len), 1, 54);
    if (!font || !font_big) {
      FAIL("ttf.font", "TTF_OpenFontRW: %s", TTF_GetError());
    } else {
      OK("ttf.font", "family=%s style=%s height=%d", TTF_FontFaceFamilyName(font), TTF_FontFaceStyleName(font),
         TTF_FontHeight(font));
      SDL_Color white = {255, 255, 255, 255}, yellow = {255, 230, 120, 255};
      char buf[256];
      title = make_text(ren, font_big, "ksre-ps5 · milestone 02a · SDL2 display test", white);
      snprintf(buf, sizeof buf, "SDL %d.%d.%d · video driver: %s · renderer: %s", lv.major, lv.minor,
               lv.patch, SDL_GetCurrentVideoDriver(), renderer_name);
      l_ver = make_text(ren, font, buf, white);
      {
        SDL_DisplayMode m = {0};
        SDL_GetCurrentDisplayMode(0, &m);
        snprintf(buf, sizeof buf, "display %dx%d @ %d Hz · %s", m.w, m.h, m.refresh_rate,
                 SDL_GetPixelFormatName(m.format));
        l_disp = make_text(ren, font, buf, white);
      }
      l_utf = make_text(ren, font, "UTF-8: café · naïve · Ünïcödé · ΑΒΓ αβγ · Привет · → ≈ …", yellow);
    }
  }

  if (!image || !title.tex) {
    FAIL("setup", "image or text missing; continuing would show nothing useful");
    goto done;
  }

  /* --- animation loop ----------------------------------------------------- */
  t_start = SDL_GetTicks64();
  Uint64 last_frame = t_start, last_dyn = 0;
  int shot_done = 0;
  unsigned long ev_total = 0;
  SDL_Color white = {255, 255, 255, 255};
  while (1) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ev_total++;
      switch (e.type) {
      case SDL_QUIT:
        INFO("event", "SDL_QUIT");
        quit_reason = 1;
        break;
      case SDL_KEYDOWN:
        INFO("event", "key down scancode=%d (%s)", e.key.keysym.scancode, SDL_GetScancodeName(e.key.keysym.scancode));
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) quit_reason = 2;
        break;
      case SDL_JOYBUTTONDOWN:
        INFO("event", "joystick %d button %d down", (int)e.jbutton.which, (int)e.jbutton.button);
        quit_reason = 3; /* raw button, unmapped: controller mapping is 02c */
        break;
      case SDL_JOYDEVICEADDED:
      case SDL_JOYDEVICEREMOVED: INFO("event", "joystick device %s", e.type == SDL_JOYDEVICEADDED ? "added" : "removed"); break;
      case SDL_WINDOWEVENT: INFO("event", "window event %d", e.window.event); break;
      default: break;
      }
    }
    Uint64 now = SDL_GetTicks64();
    double el = (double)(now - t_start);
    if (quit_reason || el >= duration_ms) break;

    double secs = el / 1000.0;
    /* background: the test card, scaled to the whole output */
    SDL_RenderCopy(ren, image, NULL, NULL);

    /* progress bar (time elapsed) inside the border */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_Rect pb = {40, 28, (int)((BASE_W - 80) * (el / duration_ms)), 10};
    SDL_RenderFillRect(ren, &pb);

    /* orbiting markers around the test circle */
    for (int k = 0; k < 6; k++) {
      double a = secs * 1.6 + k * (M_PI / 3.0);
      SDL_Rect m = {BASE_W / 2 + (int)(cos(a) * 420) - 14, BASE_H / 2 + (int)(sin(a) * 420) - 14, 28, 28};
      SDL_SetRenderDrawColor(ren, (Uint8)(120 + 20 * k), (Uint8)(255 - 30 * k), 80, 255);
      SDL_RenderFillRect(ren, &m);
    }
    /* bouncing square (triangle-wave motion) */
    {
      double px = fabs(fmod(secs * 380.0, 2.0 * 1500.0) - 1500.0);
      SDL_Rect sq = {200 + (int)px, 760, 60, 60};
      SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
      SDL_RenderFillRect(ren, &sq);
    }

    /* text panels */
    panel(ren, 60, 60, 1160, 260);
    int by = (int)(6 * sin(secs * 3.0));
    draw_text(ren, &title, 80, 70 + by);
    draw_text(ren, &l_ver, 80, 150);
    draw_text(ren, &l_disp, 80, 195);
    draw_text(ren, &l_utf, 80, 245);

    /* dynamic line, regenerated four times a second */
    if (font && (now - last_dyn >= 250 || !l_dyn.tex)) {
      char buf[160];
      double avg = frames ? 1000.0 / (total_ms / frames) : 0.0;
      snprintf(buf, sizeof buf, "t %4.1f / %4.1f s · frame %lu · %.1f fps avg · worst %.1f ms",
               secs, duration_ms / 1000.0, frames, avg, worst_ms);
      if (l_dyn.tex) SDL_DestroyTexture(l_dyn.tex);
      l_dyn = make_text(ren, font, buf, white);
      last_dyn = now;
    }
    panel(ren, 60, 850, 1260, 56);
    draw_text(ren, &l_dyn, 76, 862);

    if (shot && !shot_done && el >= duration_ms * 0.6) {
      SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormat(0, BASE_W, BASE_H, 32, SDL_PIXELFORMAT_ARGB8888);
      if (ss && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, ss->pixels, ss->pitch) == 0 &&
          SDL_SaveBMP(ss, shot) == 0)
        INFO("screenshot", "saved %s", shot);
      else
        FAIL_SDL("screenshot");
      SDL_FreeSurface(ss);
      shot_done = 1;
    }

    SDL_RenderPresent(ren);

    Uint64 after = SDL_GetTicks64();
    double ft = (double)(after - last_frame);
    last_frame = after;
    if (frames > 0) {
      total_ms += ft;
      if (ft > worst_ms) worst_ms = ft;
    }
    frames++;
    if (frames % 60 == 0)
      INFO("anim.progress", "t=%.1fs frames=%lu avg=%.1f fps worst=%.1f ms", secs, frames,
           total_ms > 0 ? 1000.0 * (frames - 1) / total_ms : 0.0, worst_ms);
    if (ft < 2) SDL_Delay(2); /* never spin at thousands of fps on a headless host */
  }
  {
    double secs = (double)(SDL_GetTicks64() - t_start) / 1000.0;
    INFO("anim.done", "%.2fs frames=%lu avg=%.1f fps worst=%.1f ms events=%lu reason=%s", secs, frames,
         total_ms > 0 ? 1000.0 * (frames - 1) / total_ms : 0.0, worst_ms, ev_total,
         quit_reason == 0 ? "timeout" : quit_reason == 1 ? "SDL_QUIT" : quit_reason == 2 ? "escape" : "joystick-button");
    if (frames < 5) FAIL("anim.frames", "only %lu frames presented", frames);
    else OK("anim.frames", "%lu frames presented", frames);
  }

done:
  if (l_dyn.tex) SDL_DestroyTexture(l_dyn.tex);
  if (l_utf.tex) SDL_DestroyTexture(l_utf.tex);
  if (l_disp.tex) SDL_DestroyTexture(l_disp.tex);
  if (l_ver.tex) SDL_DestroyTexture(l_ver.tex);
  if (title.tex) SDL_DestroyTexture(title.tex);
  if (image) SDL_DestroyTexture(image);
  if (font_big) TTF_CloseFont(font_big);
  if (font) TTF_CloseFont(font);
  if (ttf_up) TTF_Quit();
  if (img_up) IMG_Quit();
  if (ren) SDL_DestroyRenderer(ren);
  if (win) SDL_DestroyWindow(win);
  if (sdl_up) SDL_Quit();
  INFO("shutdown", "SDL_Quit returned");

  g_main_done = 1;
  if (wd_up) pthread_join(wd, NULL);
  say("KSRE_PS5_02A_END frames=%lu fail=%d", frames, g_fail);
  if (g_logfile) fclose(g_logfile);
  g_logfile = NULL;
  {
    char msg[96];
    snprintf(msg, sizeof msg, "ksre-ps5 02a: finished (%s)", g_fail ? "FAILURES" : "ok");
    notify(msg);
  }
  return g_fail ? 1 : 0;
}
