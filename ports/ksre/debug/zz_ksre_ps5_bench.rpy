# SPDX-License-Identifier: GPL-3.0-or-later
#
# Debug only (not part of the staged game; milestone 06b). With KSRE_PS5_BENCH=1 the game
# skips its splash screen and runs an unattended media/performance benchmark, then quits:
#
#   video   each KSRE video type through renpy.movie_cutscene (4LS splash, opening, act title
#           cards, an act 2 card at the highest bit rate), plus a credits Movie displayable
#   scene   static scene idle, dissolves, ImageDissolve transitions, a sprite, text at KSRE's
#           default 40 cps; each with and without KSRE's camera transform (labels.rpy)
#   audio   music + ambient + sound channels together; positions and mixer volumes
#
# One line per segment on stdout:  KSRE_PS5_BENCH seg=NAME wall=S frames=N fps=F
#   iv_med/iv_p95/iv_max (ms between drawn frames), draw_ms (avg time inside draw_screen),
#   render_ms (avg Python render-tree time per frame), vframes / vfps (video frames delivered by ffmedia), first_ms (first frame after start).
# KSRE_PS5_BENCH_SHORT=1 cuts every video to 8 s. Text shown here is neutral test text, not
# KSRE's script.

init python:
    import os as _kb_os
    import time as _kb_time

    _kb_on = _kb_os.environ.get("KSRE_PS5_BENCH") == "1"
    _kb_short = _kb_os.environ.get("KSRE_PS5_BENCH_SHORT") == "1"

    if _kb_on:
        config.label_overrides["splashscreen"] = "ksre_ps5_bench"

    _kb = {"seg": None, "frames": [], "draw": 0.0, "render": 0.0, "vframes": 0, "t0": 0.0,
           "installed": False}

    def _kb_out(s):
        print("KSRE_PS5_BENCH " + s)
        try:
            import sys
            sys.stdout.flush()
        except Exception:
            pass

    def _kb_install():
        if _kb["installed"]:
            return
        _kb["installed"] = True
        iface = renpy.display.interface
        orig_draw = iface.draw_screen

        def draw_screen(root_widget, fullscreen_video, draw):
            t = _kb_time.time()
            rv = orig_draw(root_widget, fullscreen_video, draw)
            if draw and _kb["seg"]:
                e = _kb_time.time()
                _kb["frames"].append(e)
                _kb["draw"] += e - t
            return rv

        iface.draw_screen = draw_screen

        # Python side of a frame (render tree), the rest of draw_ms is GL drawing + flip.
        rmod = renpy.display.render
        orig_render = rmod.render_screen

        def render_screen(*args, **kwargs):
            t = _kb_time.time()
            rv = orig_render(*args, **kwargs)
            if _kb["seg"]:
                _kb["render"] += _kb_time.time() - t
            return rv

        rmod.render_screen = render_screen

        chan = renpy.audio.audio.Channel
        orig_read = chan.read_video

        def read_video(self):
            rv = orig_read(self)
            if rv is not None and _kb["seg"]:
                _kb["vframes"] += 1
            return rv

        chan.read_video = read_video

        import sys
        _kb_out("start cpu_count=%r LP_NUM_THREADS=%r renderer=%r vsync=%r gl_framerate=%r "
                "gl_powersave=%r short=%r" % (
            _kb_os.cpu_count(), _kb_os.environ.get("LP_NUM_THREADS"),
            renpy.display.draw.info.get("renderer"), _kb_os.environ.get("RENPY_GL_VSYNC"),
            _preferences.gl_framerate, _preferences.gl_powersave, _kb_short))

    def _kb_begin(name):
        _kb["seg"] = name
        _kb["frames"] = []
        _kb["draw"] = 0.0
        _kb["render"] = 0.0
        _kb["vframes"] = 0
        _kb["t0"] = _kb_time.time()

    def _kb_end():
        name = _kb["seg"]
        _kb["seg"] = None
        wall = _kb_time.time() - _kb["t0"]
        fr = _kb["frames"]
        n = len(fr)
        iv = sorted((b - a) * 1000.0 for a, b in zip(fr, fr[1:]))

        def pct(p):
            if not iv:
                return 0.0
            return iv[min(len(iv) - 1, int(p * len(iv)))]

        first = (fr[0] - _kb["t0"]) * 1000.0 if fr else -1.0
        _kb_out("seg=%s wall=%.2f frames=%d fps=%.1f iv_med=%.1f iv_p95=%.1f iv_max=%.1f "
                "draw_ms=%.1f render_ms=%.1f vframes=%d vfps=%.1f first_ms=%.0f" % (
            name, wall, n, n / wall if wall else 0.0, pct(0.5), pct(0.95), iv[-1] if iv else 0.0,
            (_kb["draw"] / n * 1000.0) if n else 0.0, (_kb["render"] / n * 1000.0) if n else 0.0,
            _kb["vframes"],
            _kb["vframes"] / wall if wall else 0.0, first))

    def _kb_movie(name, seconds):
        if _kb_short:
            seconds = min(seconds, 8.0)
        _kb_begin("video:" + name)
        renpy.movie_cutscene("video/" + name + ".mkv", delay=seconds)
        _kb_end()

    def _kb_audio_report(tag):
        parts = []
        for c in ("music", "sound", "ambient"):
            try:
                parts.append("%s=%s@%s" % (c, renpy.music.get_playing(c), renpy.music.get_pos(c)))
            except Exception as e:
                parts.append("%s=err:%r" % (c, e))
        try:
            vol = dict(_preferences.volumes)
        except Exception:
            vol = {}
        _kb_out("audio %s %s volumes=%r mute=%r" % (tag, " ".join(parts), vol,
                                                   dict(getattr(_preferences, "mute", {}))))

label ksre_ps5_bench:
    $ _kb_install()
    if _kb_os.environ.get("KSRE_PS5_BENCH_SCENES_ONLY") == "1":
        jump ksre_ps5_bench_scenes
    $ _kb_out("phase video")
    $ _kb_movie("4ls", 9.0)
    $ _kb_movie("tc_act1", 15.0)
    $ _kb_movie("tc_act3_emi", 15.0)
    $ _kb_movie("op_1", 30.0)
    $ _kb_movie("tc_act2_hanako", 30.0)

    scene black
    with None
    $ _kb_begin("video:credits_emi_displayable")
    show emi credits
    $ renpy.pause(8.0 if _kb_short else 20.0, hard=True)
    $ _kb_end()
    hide emi credits

label ksre_ps5_bench_scenes:
    $ _kb_out("phase scene camera=ksre")
    camera at defaultColorblind:
        function tf_parallax
    call ksre_ps5_bench_scene("ksre")

    $ _kb_out("phase scene camera=matrix")
    camera at defaultColorblind
    call ksre_ps5_bench_scene("matrix")

    $ _kb_out("phase scene camera=function")
    camera:
        function _ksre_ps5_tf_parallax_ksre
    call ksre_ps5_bench_scene("function_ksre")

    $ _kb_out("phase scene camera=none")
    camera
    call ksre_ps5_bench_scene("none")

    # Same scenes with textures loaded without mipmaps (config.mipmap; KSRE draws at 1:1).
    $ config.mipmap = False
    $ renpy.free_memory()
    $ _kb_out("phase scene camera=ksre mipmap=False")
    camera at defaultColorblind:
        function tf_parallax
    call ksre_ps5_bench_scene("ksre_nomip")
    $ _kb_out("phase scene camera=none mipmap=False")
    camera
    call ksre_ps5_bench_scene("none_nomip")
    $ config.mipmap = True
    $ renpy.free_memory()

    $ _kb_out("phase audio")
    camera at defaultColorblind:
        function tf_parallax
    scene bg school_gate
    with None
    $ _kb_begin("audio")
    play music music_happiness
    play ambient sfx_traffic
    play sound sfx_normalbell
    $ renpy.pause(4.0, hard=True)
    $ _kb_audio_report("t4")
    $ renpy.pause(4.0, hard=True)
    $ _kb_audio_report("t8")
    $ _kb_end()
    stop music
    stop ambient
    stop sound

    $ _kb_out("done")
    $ renpy.quit()

label ksre_ps5_bench_scene(tag):
    scene bg school_gate
    with None
    $ renpy.pause(1.0, hard=True)

    $ _kb_begin("idle:" + tag)
    $ renpy.pause(3.0, hard=True)
    $ _kb_end()

    $ _kb_begin("dissolve:" + tag)
    scene bg school_courtyard
    with locationchange
    $ _kb_end()

    $ _kb_begin("show_sprite:" + tag)
    scene bg school_lobby
    show muto normal at center
    with charachangealways
    $ _kb_end()

    $ _kb_begin("text40cps:" + tag)
    "ksre-ps5 text speed check: the quick brown fox jumps over the lazy dog. 0123456789. The second sentence makes this line wrap onto the next row of the box.{w=1.0}{nw}"
    $ _kb_end()

    $ _kb_begin("dotwipe:" + tag)
    scene bg school_staircase2
    with dotwipe_up
    $ _kb_end()

    $ _kb_begin("shorttimeskip:" + tag)
    scene bg school_hallway3
    with shorttimeskip
    $ _kb_end()

    $ _kb_begin("heartattack:" + tag)
    scene bg op_snowywoods
    show heartattack alpha
    with dissolve
    $ _kb_end()
    return
