# SPDX-License-Identifier: GPL-3.0-or-later
#
# ksre-ps5 platform overlay for Katawa Shoujo: Re-Engineered v2.0.5.
# Copied into KSRE's game/ directory next to KSRE's own (unmodified) files by
# ports/ksre/stage.sh. It changes platform behaviour only; no story content.
#
# On the PS5 Ren'Py's platform detection ends with renpy.linux = True, and any
# child process (fork/exec) takes the whole console down. The launcher makes
# subprocess/os spawn functions raise OSError(ENOSYS); this file keeps KSRE
# away from those paths.

init -10 python:

    class _KsrePs5NoPresence(object):
        """Stands in for KSRE's Discord Rich Presence thread (no Discord on the PS5)."""

        def __init__(self):
            self.data = {}

        def start(self):
            pass

    def _ksre_ps5_tts(s):
        # Ren'Py's Linux TTS runs espeak as a child process. Keep the modes that
        # spawn nothing; spoken self-voicing has no engine on the PS5.
        if renpy.game.preferences.self_voicing in ("clipboard", "debug"):
            renpy.display.tts.default_tts_function(s)

# KSRE creates `rpc` in `python early` and starts it in `init 999 python`
# (definitions.rpy); replace it before that.
init 998 python:
    if not renpy.emscripten:
        rpc = _KsrePs5NoPresence()

define config.tts_function = _ksre_ps5_tts

# The console has one display; start in full screen (players can still change it).
define config.default_fullscreen = True

# 06a: the console has no mouse. KSRE draws its own cursor (config.mouse_displayable) at the
# last mouse position, i.e. stuck in the top-left corner; draw none. Also keeps the "Hardware
# cursor" toggle in Options from bringing it back.
init 998 python:
    mouse = Null()
    config.mouse_displayable = None
