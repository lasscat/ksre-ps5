# SPDX-License-Identifier: GPL-3.0-or-later
#
# Debug only (not part of the staged game): with KSRE_PS5_SHOTS=DIR, saves a screenshot of
# the screen every KSRE_PS5_SHOT_EVERY seconds (default 10) as DIR/shot-NNN.png, and one
# DIR/main_menu.png 8 s after the main menu appears. Used to compare console colours with
# the host twin (milestone 05).

init python:
    import os as _ksre_os
    import time as _ksre_time

    _ksre_shot_dir = _ksre_os.environ.get("KSRE_PS5_SHOTS")

    if _ksre_shot_dir:
        _ksre_shot_state = {"next": _ksre_time.time() + 5, "n": 0, "menu_since": None, "menu_done": False}
        _ksre_shot_every = float(_ksre_os.environ.get("KSRE_PS5_SHOT_EVERY", "10"))

        def _ksre_shot(name):
            path = _ksre_os.path.join(_ksre_shot_dir, name)
            ok = renpy.screenshot(path)
            renpy.write_log("ksre-ps5 screenshot %s ok=%r" % (path, ok))

        def _ksre_shot_tick():
            now = _ksre_time.time()
            st = _ksre_shot_state
            if now >= st["next"]:
                st["next"] = now + _ksre_shot_every
                _ksre_shot("shot-%03d.png" % st["n"])
                st["n"] += 1
            if not st["menu_done"]:
                if renpy.get_screen("main_menu"):
                    if st["menu_since"] is None:
                        st["menu_since"] = now
                    elif now - st["menu_since"] > 8:
                        st["menu_done"] = True
                        _ksre_shot("main_menu.png")
                else:
                    st["menu_since"] = None

        config.periodic_callbacks.append(_ksre_shot_tick)
