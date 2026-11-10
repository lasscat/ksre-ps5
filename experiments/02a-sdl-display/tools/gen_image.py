#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the ORIGINAL 1920x1080 test image for experiment 02a.

Pure standard library (zlib + struct); deterministic, so the output PNG is
byte-for-byte reproducible. The image is a display test card made only of
generated geometry: a diagonal colour gradient, a 16:9 grid, a circle (which
looks like an ellipse if the aspect ratio is wrong), border and corner
markers (which reveal overscan), and a colour-bar strip. It contains no
third-party artwork.

Usage: gen_image.py OUT.png [WIDTH HEIGHT]
"""
import struct
import sys
import zlib


def png_bytes(w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(r) for r in rows)  # filter type 0 per row
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    out = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 1080

    cx, cy = w // 2, h // 2
    radius = h * 3 // 10
    ring = max(4, h // 135)
    border = max(6, h // 90)
    grid = w // 16  # 16 columns; square cells when the aspect ratio is 16:9
    marker = h // 12
    bars = [(235, 235, 235), (235, 235, 0), (0, 235, 235), (0, 235, 0),
            (235, 0, 235), (235, 0, 0), (0, 0, 235), (20, 20, 20)]
    bar_top, bar_bot = h - h // 8, h - h // 20

    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            # Diagonal gradient: deep blue -> teal -> warm orange.
            t = (x / (w - 1) + y / (h - 1)) / 2.0
            if t < 0.5:
                u = t * 2
                r, g, b = 20 + 20 * u, 30 + 110 * u, 90 + 60 * u
            else:
                u = (t - 0.5) * 2
                r, g, b = 40 + 190 * u, 140 + 20 * u, 150 - 90 * u
            r, g, b = int(r), int(g), int(b)

            if x % grid == 0 or y % grid == 0:  # grid lines
                r, g, b = (r + 255) // 2, (g + 255) // 2, (b + 255) // 2

            d2 = (x - cx) ** 2 + (y - cy) ** 2  # circle with ring
            if (radius - ring) ** 2 <= d2 <= radius ** 2:
                r, g, b = 255, 255, 255
            elif d2 < (radius - ring) ** 2:
                r, g, b = r // 2, g // 2, b // 2

            if bar_top <= y < bar_bot:  # colour bars
                r, g, b = bars[min(7, x * 8 // w)]

            on_border = (x < border or y < border or x >= w - border
                         or y >= h - border)
            in_corner = ((x < marker or x >= w - marker)
                         and (y < marker or y >= h - marker))
            if on_border:
                r, g, b = 255, 255, 255
            if in_corner and not on_border:
                r, g, b = 255, 60, 60
            row += bytes((r, g, b))
        rows.append(row)

    data = png_bytes(w, h, rows)
    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s (%dx%d, %d bytes)" % (out, w, h, len(data)))


if __name__ == "__main__":
    main()
