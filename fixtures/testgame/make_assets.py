#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Write the generated assets of the ksre-ps5 test game into GAME_DIR (python3 stdlib only).

    python3 fixtures/testgame/make_assets.py build/ports/renpy/testgame/game

images/ksre_bg.png    1920x1080 RGB: left half SELFTEST_LEFT, right half SELFTEST_RIGHT,
                      a light band across the middle (the self-test samples the halves)
images/ksre_mark.png  256x256 RGBA disc with soft edge (moves under ATL)
audio/ksre_tone.wav   1.5 s 440 Hz sine, 44.1 kHz stereo 16-bit (self-test playback check)
audio/ksre_chime.wav  0.8 s two-note chime, 44.1 kHz stereo 16-bit (interactive path)

Output is deterministic (same bytes every run).
"""
import math
import os
import struct
import sys
import wave
import zlib

SELFTEST_LEFT = (32, 96, 160)
SELFTEST_RIGHT = (176, 64, 48)
BAND = (230, 230, 220)


def png(path, width, height, rows, alpha):
    """rows: iterable of bytes, one per scanline, without the filter byte."""
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def background(path):
    w, h = 1920, 1080
    half = w // 2
    plain = bytes(SELFTEST_LEFT) * half + bytes(SELFTEST_RIGHT) * (w - half)
    band = bytes(BAND) * w
    png(path, w, h, (band if 500 <= y < 580 else plain for y in range(h)), alpha=False)


def mark(path):
    size, r = 256, 120.0
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            d = math.hypot(x - 127.5, y - 127.5)
            a = max(0.0, min(1.0, r - d))  # 1 px soft edge
            shade = int(200 + 55 * (1 - d / r)) if d < r else 0
            row += bytes((255, shade, 64, int(a * 255)))
        rows.append(bytes(row))
    png(path, size, size, rows, alpha=True)


def wav(path, seconds, notes, rate=44100):
    frames = bytearray()
    n = int(seconds * rate)
    for i in range(n):
        t = i / rate
        f = notes[min(len(notes) - 1, int(t / seconds * len(notes)))]
        env = min(1.0, t / 0.02, (seconds - t) / 0.05)  # click-free start and end
        s = int(0.3 * env * 32767 * math.sin(2 * math.pi * f * t))
        frames += struct.pack("<hh", s, s)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    os.makedirs(os.path.join(game, "images"), exist_ok=True)
    os.makedirs(os.path.join(game, "audio"), exist_ok=True)
    background(os.path.join(game, "images", "ksre_bg.png"))
    mark(os.path.join(game, "images", "ksre_mark.png"))
    wav(os.path.join(game, "audio", "ksre_tone.wav"), 1.5, [440.0])
    wav(os.path.join(game, "audio", "ksre_chime.wav"), 0.8, [659.25, 880.0])
    print("assets written to", game)


if __name__ == "__main__":
    main()
