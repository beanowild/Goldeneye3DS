#!/usr/bin/env python3
"""
texdump_to_png.py - turn the renderer's texture dump into a contact sheet.

    python tools/texdump_to_png.py [--out build/texdump.png] [--scale 3]

Build with F3D_TEX_DUMP defined in src/gfx/f3d_tex.c and run the game
once; it writes decoded textures to Azahar's SD card
(.tools/azahar/user/sdmc/3ds/ge007/texdump/NNN_WxH_....rgba, RGBA8 as
little-endian 0xRRGGBBAA words). This lays them out in one PNG, on a
checkerboard so alpha shows, numbered left to right, top to bottom, and
prints each file's parameters.
"""

import argparse
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DUMP = ROOT / ".tools/azahar/user/sdmc/3ds/ge007/texdump"


def write_png(path, width, height, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--out", default=str(ROOT / "build" / "texdump.png"))
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--dir", default=str(DUMP),
                    help="dump folder (scripted runs: .tools/azahar-auto/user/sdmc/3ds/ge007/texdump)")
    ap.add_argument("--cols", type=int, default=10)
    ap.add_argument("--first", type=int, default=0, help="first dump number to include")
    ap.add_argument("--count", type=int, default=1000, help="how many to include")
    args = ap.parse_args()

    dump = Path(args.dir)
    files = sorted(dump.glob("*.rgba"))
    if not files:
        raise SystemExit(f"no dump in {dump}")
    texs = []
    for f in files:
        m = re.match(r"(\d+)_(\d+)x(\d+)_", f.name)
        n, w, h = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if not args.first <= n < args.first + args.count:
            continue
        data = f.read_bytes()
        if len(data) < w * h * 4:
            continue
        px = [struct.unpack_from("<I", data, 4 * i)[0] for i in range(w * h)]
        texs.append((n, w, h, px))
        print(f.name)

    sc, cols = args.scale, args.cols
    cell = max(max(w, h) for _, w, h, _ in texs) * sc + 4
    rows_n = (len(texs) + cols - 1) // cols
    W, H = cols * cell, rows_n * cell
    img = [bytearray(W * 3) for _ in range(H)]
    for y in range(H):
        for x in range(W):
            v = 90 if ((x // 6) + (y // 6)) % 2 else 60
            img[y][3 * x:3 * x + 3] = bytes((v, v, v))
    for i, (n, w, h, px) in enumerate(texs):
        ox, oy = (i % cols) * cell + 2, (i // cols) * cell + 2
        for y in range(h * sc):
            for x in range(w * sc):
                c = px[(y // sc) * w + x // sc]
                r, g, b, a = c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255
                row = img[oy + y]
                o = 3 * (ox + x)
                bg = row[o]
                row[o:o + 3] = bytes(((r * a + bg * (255 - a)) // 255, (g * a + bg * (255 - a)) // 255,
                                      (b * a + bg * (255 - a)) // 255))
    write_png(args.out, W, H, img)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
