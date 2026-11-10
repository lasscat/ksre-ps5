# SPDX-License-Identifier: GPL-3.0-or-later
# Interactive path of the ksre-ps5 test game (the checklist for a person with a
# controller or keyboard). The unattended self-test is in selftest.rpy.

define guide = Character("Guide", color="#ffd040")

default ksre_path = "none"
default persistent.ksre_starts = 0

transform ksre_bounce:
    xalign 0.75 yalign 0.3
    parallel:
        function ksre_atl_probe
    parallel:
        linear 0.6 yalign 0.7
        linear 0.6 yalign 0.3
        repeat

transform ksre_spin:
    xalign 0.75 yalign 0.5
    rotate 0
    linear 1.5 rotate 360
    repeat

transform ksre_slide:
    xalign 0.55 yalign 0.5
    linear 1.0 xalign 0.95
    linear 1.0 xalign 0.55
    repeat

label start:
    if ksre_selftest:
        jump ksre_selftest
    $ persistent.ksre_starts += 1
    scene ksre_bg
    show ksre_mark at ksre_bounce

    guide "Welcome to the ksre-ps5 test game. It checks dialogue, choices, motion, sound, saving and rollback."
    guide "You have started this game [persistent.ksre_starts] time(s) on this device. After a restart this number should be one higher."
    guide "Advance with the confirm button. To go back, use the rollback button (L1) or scroll up."

    menu:
        guide "Which path should the test take?"

        "The blue path":
            $ ksre_path = "blue"
            show ksre_mark at ksre_spin
            guide "You took the blue path. The marker now spins in place."

        "The orange path":
            $ ksre_path = "orange"
            show ksre_mark at ksre_slide
            guide "You took the orange path. The marker now slides left and right."

    play sound "audio/ksre_chime.wav"
    guide "A short two-note chime should have played just now."

    guide "Open the game menu, save in the first slot, then return here."
    guide "Now open the game menu again and load that slot. You should come back to this part of the test on the [ksre_path] path."
    guide "Roll back to the question about the path and choose the other answer. The marker should move differently."
    guide "That is the whole test. Quit from the game menu, start the game again, and check that the start count went up."
    return
