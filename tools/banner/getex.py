"""
getex.py - decodes GoldenEye image-bank textures
(external/goldeneye/assets/images/split/<n>.bin) to RGBA8.

Only the zlib (paletted) kind is handled, which is what character, head and
weapon models use; texLoad / texInflateZlib in src/game/image.c is the
reference. Layout, as a big-endian bit string:
  1 bit  explicit lods, 1 bit zlib, 6 bits lod count
  8 bits format (pdtex numbering: 9 RGBA16_CI8, 10 RGBA16_CI4,
         11 IA16_CI8, 12 IA4_CI4), 8 bits colours - 1, 16 bits per colour
  per image: 8 bits width, 8 bits height, then a rarezip stream (0x1172
  + raw deflate, or 0x1173 + 3 size bytes + raw deflate) of indices.
Rows are first row first (v = 0 at the top).
"""
import struct
import zlib

import geanim

SPLIT = geanim.ASSETS + "images/split/"


def rgba5551(c):
    r, g, b = (c >> 11) & 31, (c >> 6) & 31, (c >> 1) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if c & 1 else 0)


def ia16(c):
    i, a = c >> 8, c & 0xFF
    return (i, i, i, a)


def decode(texnum):
    """(width, height, bytes RGBA8) of the first image of a texture."""
    d = open(f"{SPLIT}{texnum}.bin", "rb").read()
    head = d[0]
    if not head & 0x40:
        raise ValueError(f"texture {texnum}: not zlib (header {head:02x})")
    fmt, ncol = d[1], d[2] + 1
    pal = [struct.unpack_from(">H", d, 3 + 2 * i)[0] for i in range(ncol)]
    o = 3 + 2 * ncol
    w, h = d[o], d[o + 1]
    o += 2
    if d[o:o + 2] == b"\x11\x72":
        o += 2
    elif d[o:o + 2] == b"\x11\x73":
        o += 5
    else:
        raise ValueError(f"texture {texnum}: no rarezip header")
    raw = zlib.decompressobj(-15).decompress(d[o:])
    if fmt in (9, 11):
        idx = raw[:w * h]
    elif fmt in (10, 12):
        rowb = (w + 1) // 2
        idx = bytearray()
        for y in range(h):
            row = raw[y * rowb:(y + 1) * rowb]
            for x in range(w):
                b = row[x // 2]
                idx.append(b >> 4 if x % 2 == 0 else b & 0xF)
    else:
        raise ValueError(f"texture {texnum}: format {fmt}")
    conv = rgba5551 if fmt in (9, 10) else ia16
    cols = [conv(c) for c in pal]
    out = bytearray()
    for i in idx:
        out += bytes(cols[i] if i < len(cols) else (255, 0, 255, 255))
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
