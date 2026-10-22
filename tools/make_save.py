"""Build a GoldenEye EEPROM image (eeprom.bin, the raw 512-byte .eep layout
os_eeprom.c reads) whose first folder is 100% complete: every mission
done on Agent, Secret Agent and 00 Agent within its cheat target time,
all cheats unlocked, 007 mode unlocked. Folders 2-4 are empty.

    py -3 tools/make_save.py [-o build/eeprom_100.bin]

Layout and checksum follow the decomp's file2.c and crc.c: a 32-byte
header block, then five 0x60-byte save slots from byte 0x20. The port
writes the structs as they are in memory, so the checksums and the
options word are little-endian (an N64 .eep has them big-endian).
"""
import argparse
import struct

MASK64 = (1 << 64) - 1

# front.c solo_target_time_array: seconds per level for Agent, Secret
# Agent, 00 Agent; the non-zero entry is the difficulty its cheat needs.
TARGETS = [
    (0, 160, 0), (0, 0, 125), (300, 0, 0), (0, 210, 0), (0, 0, 240),
    (180, 0, 0), (0, 270, 0), (0, 0, 255), (90, 0, 0), (0, 195, 0),
    (0, 0, 80), (105, 0, 0), (0, 100, 0), (0, 0, 325), (225, 0, 0),
    (0, 600, 0), (0, 0, 570), (135, 0, 0), (0, 540, 0), (0, 0, 360),
]
LEVELS = 20                       # SP_LEVEL_MAX
DEFAULT_OPTIONS = 0x0002 | 0x0008 | 0x0010 | 0x0020


def rng_step(seed):
    """random.c step(): the game's 64-bit shift/xor generator."""
    s = seed
    s = ((((s << 63) & MASK64) >> 31) | (((s << 31) & MASK64) >> 32)) ^ (((s << 44) & MASK64) >> 32)
    s ^= (s >> 20) & 0xFFF
    return s, s & 0xFFFFFFFF


def crc(data):
    """crc.c fileGenerateCRC over data; returns (chksum1, chksum2)."""
    poly = 0x8F809F473108B3C1
    shift = 0
    c1 = c2 = 0
    for b in data:
        poly = (poly + (b << (shift & 0xF))) & MASK64
        poly, r = rng_step(poly)
        c1 ^= r
        shift += 7
    for b in reversed(data):
        poly = (poly + (b << (shift & 0xF))) & MASK64
        poly, r = rng_step(poly)
        c2 ^= r
        shift += 3
    return c1, c2


def set_time(times, level, difficulty, secs):
    """file2.c fileSetDifficultyStageTime: 10-bit times packed MSB first,
    bit offset ((difficulty * 20) + level) * 10."""
    secs = max(1, min(secs, 0x3FF))
    bit = (difficulty * LEVELS + level) * 10
    for i in range(10):
        if secs & (1 << (9 - i)):
            pos = bit + i
            times[pos >> 3] |= 0x80 >> (pos & 7)


def save_slot(folder, slot=0, bond=0, reset=False, complete=False):
    flags = (folder & 7) | ((slot << 3) & 0x18) | ((bond << 5) & 0x60) | (0x80 if reset else 0)
    times = bytearray(76)
    cheats = [0, 0, 0]
    flag_007 = 0
    if complete:
        for level, row in enumerate(TARGETS):
            target = max(row)
            for difficulty in range(3):
                set_time(times, level, difficulty, target - 1)
            cheats[level >> 3] |= 1 << (level & 7)
        flag_007 = 1
    body = struct.pack("<BBBBHBBBB", flags, flag_007, 0xFF, 0xFF, DEFAULT_OPTIONS,
                       cheats[0], cheats[1], cheats[2], 0) + bytes(times) + bytes(2)  # tail padding to 0x60
    assert len(body) == 0x58
    return struct.pack("<II", *crc(body)) + body


def header():
    # file2.c fileValidateSaves expects unk[0] = SAVEFLAGS_SET(FOLDER3,
    # SAVESLOT1, BOND_CONNERY, FALSE) = 0x42.
    body = bytes([0x42]) + bytes(23)
    return struct.pack("<II", *crc(body)) + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="build/eeprom_100.bin")
    args = ap.parse_args()
    image = header()
    image += save_slot(0, complete=True)
    image += save_slot(1) + save_slot(2) + save_slot(3)
    image += save_slot(0, reset=True)            # the spare slot
    assert len(image) == 512
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
