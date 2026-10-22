"""
geanim.py - reads GoldenEye character animations (the decomp's
assets/animationtable_data.c and animationtable_entries.c) and gives the
guard skeleton's joint rotations for any frame.

Layout (initanitable.c, model.c):
  animation table: per animation a 0x14-byte header at PTR_ANIM_<name>
    s32 frame data offset (into the entries segment), u16 frames,
    u8 bits per angle, u8 flags, u32 descriptors, u16 root-motion bits
    per frame, u16 bits per frame, u32 root-motion stream
  entries segment: per frame, (bits per frame / 8) bytes; joint j's three
    angles start at bit skeleton_guard[j].mtxA * width, big-endian, each
    the top `width` bits of a 16-bit turn (sub_GAME_7F06DEC0)
A joint's matrix is parent * T(origin) * Rz * Ry * Rx
(matrix_4x4_set_rotation_around_xyz).
"""
import math
import os
import re

# the decomp's assets/ (tools/romtocia points this at files cut from a ROM)
ASSETS = os.environ.get("GE_ASSETS") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "external", "goldeneye", "assets")
ASSETS = ASSETS.rstrip("/\\") + "/"

# JOINTLIST(guard): mtxA per joint id (assets/embedded/skeletons/guard.inc.c)
GUARD_MTXA = [0x0, 0x0, 0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E,
              0x21, 0x24, 0x27, 0x2A]


def _defines(*hfiles):
    defs = {}
    for h in hfiles:
        for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)", open(h).read()):
            defs[m.group(1)] = int(m.group(2), 0)
    return defs


def _segment(cfile, defs):
    """The segment's bytes: each array placed at its PTR_ define."""
    text = open(cfile).read()
    buf = bytearray()
    for m in re.finditer(r"u32\s+(\w+)\[\]\s*=\s*\{(.*?)\}", text, re.S):
        name, body = m.group(1), m.group(2)
        words = []
        for tok in re.split(r"[,\s]+", body.strip()):
            if not tok:
                continue
            words.append(defs[tok] if tok in defs else int(tok, 0))
        key = "PTR_" + name.replace("ANIM_DATA_", "ANIM_").replace("ANIM_ENTRY_", "ANIM_ENTRY_")
        start = defs.get(key, len(buf))
        if start > len(buf):
            buf += bytes(start - len(buf))
        data = b"".join((w & 0xFFFFFFFF).to_bytes(4, "big") for w in words)
        buf[start:start + len(data)] = data
    return bytes(buf)


class Anims:
    def __init__(s):
        s.defs = _defines(ASSETS + "animationtable_data.h", ASSETS + "animationtable_entries.h")
        s.table = _segment(ASSETS + "animationtable_data.c", s.defs)
        s.entries = _segment(ASSETS + "animationtable_entries.c", s.defs)

    def header(s, name):
        o = s.defs["PTR_ANIM_" + name]
        t = s.table
        return dict(address=int.from_bytes(t[o:o + 4], "big"),
                    frames=int.from_bytes(t[o + 4:o + 6], "big"),
                    width=t[o + 6], flags=t[o + 7],
                    frame_bits=int.from_bytes(t[o + 0xE:o + 0x10], "big"))

    def rotations(s, name, frame):
        """{joint id: (x, y, z) radians} for joints 1..15 at a frame."""
        h = s.header(name)
        frame = max(0, min(int(frame), h["frames"] - 1))
        base = h["address"] + frame * (h["frame_bits"] >> 3)
        w = h["width"]

        def bits(off):
            v = 0
            for i in range(w):
                b = off + i
                v = (v << 1) | ((s.entries[base + (b >> 3)] >> (7 - (b & 7))) & 1)
            return (v << (16 - w)) & 0xFFFF

        out = {}
        for j in range(1, 16):
            off = GUARD_MTXA[j] * w
            out[j] = tuple(bits(off + k * w) * 2 * math.pi / 65535.0 for k in range(3))
        return out


if __name__ == "__main__":
    a = Anims()
    for nm in ("idle", "extending_left_hand", "aim_one_handed_weapon_left_right"):
        h = a.header(nm)
        print(nm, h, hex(a.defs["PTR_ANIM_ENTRY_" + nm]))
    print(a.rotations("idle", 0))
