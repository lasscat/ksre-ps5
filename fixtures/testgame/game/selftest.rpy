# SPDX-License-Identifier: GPL-3.0-or-later
# Unattended self-test of the ksre-ps5 test game. Runs instead of the main menu when the
# environment variable KSRE_TESTGAME_SELFTEST is set (label start jumps here; the main menu
# is skipped so the test runs in the normal game context, where saves and rollback are
# recorded), prints one line per check
#   TESTGAME PASS|FAIL|INFO <name> <detail>
# then `TESTGAME END pass=N fail=M` and quits with status 0 (all passed) or 1.
# Results live in renpy.session, which survives the load and rollback checks.

init -10 python:
    import os
    import sys
    import time
    import io

    ksre_selftest = "KSRE_TESTGAME_SELFTEST" in os.environ
    if ksre_selftest:
        os.environ["RENPY_SKIP_MAIN_MENU"] = "1"

    # Colours of the two halves of images/ksre_bg.png (make_assets.py).
    KSRE_LEFT = (32, 96, 160)
    KSRE_RIGHT = (176, 64, 48)

    def ksre_say(kind, name, detail=""):
        print("TESTGAME %s %s %s" % (kind, name, detail))
        sys.stdout.flush()

    def ksre_result(name, ok, detail=""):
        renpy.session.setdefault("ksre_results", {})[name] = bool(ok)
        ksre_say("PASS" if ok else "FAIL", name, detail)

    def ksre_atl_probe(trans, st, at):
        # Called every frame by the ksre_bounce transform (script.rpy) while it is shown.
        s = renpy.session.setdefault("ksre_atl", {"calls": 0, "st": 0.0, "lo": 9.0, "hi": -9.0})
        s["calls"] += 1
        s["st"] = max(s["st"], st)
        y = trans.yalign
        if y is not None:
            s["lo"] = min(s["lo"], y)
            s["hi"] = max(s["hi"], y)
        return 0

    def ksre_pixel_check():
        data = renpy.screenshot_to_bytes((1920, 1080))
        surf = renpy.pygame.image.load(io.BytesIO(data), "screenshot.png")
        got = {}
        for name, pos, want in (("left", (100, 100), KSRE_LEFT), ("right", (1850, 100), KSRE_RIGHT)):
            c = tuple(surf.get_at(pos))[:3]
            got[name] = c
            if max(abs(a - b) for a, b in zip(c, want)) > 3:
                return False, "png=%d bytes %s=%r want %r" % (len(data), name, c, want)
        return True, "png=%d bytes left=%r right=%r" % (len(data), got["left"], got["right"])

label ksre_selftest:
    python:
        renpy.session["ksre_t0"] = time.time()
        renpy.session["ksre_results"] = {}
        persistent.ksre_st_phase = None
        persistent.ksre_st_rb_passes = 0
        ksre_say("INFO", "version", "%s python=%s platform=%s" % (renpy.version_only, sys.version.split()[0], sys.platform))
        info = renpy.get_renderer_info()
        ksre_say("INFO", "renderer", repr(dict(info)))
        ksre_result("renderer", info.get("renderer") in ("gl2", "gles2") and info.get("models", info.get("model", False)),
                    "renderer=%r" % info.get("renderer"))

        env_saves = os.environ.get("RENPY_PATH_TO_SAVES")
        ksre_say("INFO", "savedir", config.savedir)
        ksre_result("savedir", env_saves is None or config.savedir.startswith(env_saves), "env=%r" % env_saves)

        persistent.ksre_selftest_runs = (persistent.ksre_selftest_runs or 0) + 1
        renpy.save_persistent()
        pfile = os.path.join(config.savedir, "persistent")
        ksre_result("persistent", os.path.exists(pfile), "runs=%d file=%s" % (persistent.ksre_selftest_runs, pfile))

    scene ksre_bg
    show ksre_mark at ksre_bounce
    "Self-test: dialogue and text rendering.{w=0.3}{nw}"
    $ renpy.pause(1.0, hard=True)

    python:
        s = renpy.session.get("ksre_atl", {"calls": 0, "st": 0.0, "lo": 9.0, "hi": -9.0})
        ksre_result("atl", s["calls"] >= 10 and s["st"] > 0.5 and s["hi"] - s["lo"] > 0.1,
                    "calls=%d st=%.2f yalign=%.2f..%.2f" % (s["calls"], s["st"], s["lo"], s["hi"]))
        ok, detail = ksre_pixel_check()
        ksre_result("pixels", ok, detail)

    play sound "audio/ksre_tone.wav"
    $ renpy.pause(0.6, hard=True)
    python:
        playing = renpy.music.get_playing("sound")
        pos = renpy.music.get_pos("sound")
        ksre_result("audio", playing == "audio/ksre_tone.wav" and pos is not None and pos > 0.1,
                    "playing=%r pos=%r" % (playing, pos))

    # Two outcomes of a choice, without the menu UI (that is in the interactive path).
    call ksre_st_choice("blue")
    call ksre_st_choice("orange")
    python:
        c = renpy.session.get("ksre_choice", [])
        ksre_result("choice", c == ["blue:spin", "orange:slide"], repr(c))

    jump ksre_st_saveload

label ksre_st_choice(which):
    if which == "blue":
        show ksre_mark at ksre_spin
        $ renpy.session.setdefault("ksre_choice", []).append("blue:spin")
    else:
        show ksre_mark at ksre_slide
        $ renpy.session.setdefault("ksre_choice", []).append("orange:slide")
    "Self-test: outcome [which].{w=0.2}{nw}"
    return

# Auto-dismissed lines ({nw}) are not rollback checkpoints, so the save/load and rollback
# checks set their own with renpy.checkpoint(). Retry counters in renpy.session (which
# survives load and rollback) turn a wrong resume point into a FAIL instead of a loop.
label ksre_st_saveload:
    $ ksre_st_value = "before-save"
    "Self-test: save and load.{w=0.2}{nw}"
    $ renpy.checkpoint()
    python:
        tries = renpy.session.get("ksre_load_tries", 0)
        if persistent.ksre_st_phase != "loading" and tries < 2:
            renpy.session["ksre_load_tries"] = tries + 1
            renpy.save("ksre-selftest", "ksre-ps5 self-test")
            ksre_result("save", renpy.can_load("ksre-selftest"), "slot=ksre-selftest")
            persistent.ksre_st_phase = "loading"
            ksre_st_value = "after-save"
            renpy.load("ksre-selftest")
            persistent.ksre_st_phase = None
            ksre_result("load", False, "renpy.load returned")
        else:
            ok = persistent.ksre_st_phase == "loading" and ksre_st_value == "before-save"
            ksre_result("load", ok, "value after load=%r tries=%d" % (ksre_st_value, tries))
            persistent.ksre_st_phase = None
            renpy.unlink_save("ksre-selftest")

label ksre_st_rollback:
    $ ksre_st_rb = "first"
    "Self-test: rollback, first line.{w=0.2}{nw}"
    $ renpy.checkpoint()
    $ renpy.session.setdefault("ksre_rb_seen", []).append(ksre_st_rb)
    $ persistent.ksre_st_rb_passes += 1
    $ ksre_st_rb = "second"
    "Self-test: rollback, second line.{w=0.2}{nw}"
    $ renpy.checkpoint()
    python:
        tries = renpy.session.get("ksre_rb_tries", 0)
        seen = renpy.session.get("ksre_rb_seen")
        if persistent.ksre_st_rb_passes == 1 and tries < 2:
            renpy.session["ksre_rb_tries"] = tries + 1
            # Greedy rollback (the default) ends just after the previous checkpoint: the
            # first renpy.checkpoint() above, where ksre_st_rb is "first" again.
            renpy.rollback(force=True, checkpoints=1)
            ksre_result("rollback", False, "renpy.rollback returned")
        else:
            loads = renpy.session.get("ksre_load_tries")
            ksre_result("rollback", persistent.ksre_st_rb_passes == 2 and seen == ["first", "first"] and loads == 1,
                        "passes=%d seen=%r tries=%d save-section-runs=%r" % (persistent.ksre_st_rb_passes, seen, tries, loads))

    python:
        res = renpy.session.get("ksre_results", {})
        fails = sorted(k for k, v in res.items() if not v)
        ksre_say("END", "pass=%d fail=%d" % (len(res) - len(fails), len(fails)),
                 "t=%.1fs failed=%s" % (time.time() - renpy.session["ksre_t0"], ",".join(fails) or "-"))
        renpy.quit(status=1 if fails else 0)
