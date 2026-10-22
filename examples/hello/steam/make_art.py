#!/usr/bin/env python3
"""Draws examples/hello's Steam artwork (game.json launcher.steam_art) beside this script, from scratch:

    python3 examples/hello/steam/make_art.py [--check]

At Steam's sizes (GAME_CONTRACT.md "1. game.json"): capsule 600x900, wide_capsule 920x430, hero 3840x1240, logo
1280x720 (transparent) and icon 256x256. Flat panels in the launcher's colours and hello's two triangles; they exist
so the launcher's build embeds real pictures and its self-test writes them (CI's launcher job builds hello's
launcher). --check: the files on disk are what this script draws.
"""
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BG, PANEL, ACCENT, PALE = (0x15, 0x17, 0x1C, 255), (0x1C, 0x1F, 0x26, 255), (0xE8, 0x87, 0x3A, 255), (0xE8, 0xE6, 0xE3, 255)
CLEAR = (0, 0, 0, 0)


def png(w, h, pixel):
    """A w x h RGBA PNG; pixel(x, y) gives each pixel (called per row segment for speed: see draw())."""
    rows = b"".join(b"\0" + pixel(y) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def draw(w, h, background, triangles):
    """`background` everywhere, then each triangle (x0, y0, size, colour): pointing up, its base `size` wide."""
    def row(y):
        line = [background] * w
        for x0, y0, size, colour in triangles:
            if y0 <= y < y0 + size:
                half = (y - y0 + 1) * size // (2 * size)  # widens by one pixel every two lines
                for x in range(max(0, x0 + size // 2 - half), min(w, x0 + size // 2 + half)):
                    line[x] = colour
        return b"".join(bytes(c) for c in line)
    return png(w, h, row)


def art():
    return {
        "capsule.png": draw(600, 900, BG, [(100, 250, 400, ACCENT), (200, 500, 200, PALE)]),
        "wide_capsule.png": draw(920, 430, BG, [(260, 65, 300, ACCENT), (460, 165, 200, PALE)]),
        "hero.png": draw(3840, 1240, PANEL, [(1320, 220, 800, ACCENT), (1920, 520, 600, PALE)]),
        "logo.png": draw(1280, 720, CLEAR, [(340, 110, 500, ACCENT), (640, 310, 300, PALE)]),
        "icon.png": draw(256, 256, BG, [(28, 28, 200, ACCENT)]),
    }


def main():
    files = art()
    if "--check" in sys.argv[1:]:
        bad = [n for n, data in files.items() if not (HERE / n).exists() or (HERE / n).read_bytes() != data]
        print("hello's Steam art: " + ("differs: " + ", ".join(bad) if bad else "up to date"))
        return 1 if bad else 0
    for n, data in files.items():
        (HERE / n).write_bytes(data)
        print(f"wrote {HERE / n} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
