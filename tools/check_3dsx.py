#!/usr/bin/env python3
"""
check_3dsx.py - validate a .3dsx's relocations the way loaders do.

    python tools/check_3dsx.py build/ge007.3dsx [build/ge007.elf]

.3dsx executables are relocatable. Each relocated word holds a target in
the file's own segment layout, and the top 4 bits must be a valid
sub-type (0 for absolute relocations, 0 or 1 for relative ones).
3dsxtool doesn't check this, but loaders do (Azahar fails with a bare
"Failed to load ROM (Error 1)", and the Homebrew Launcher fails too).

The usual cause is N64 address arithmetic that GCC folds into a
relocation addend, e.g. `&sym - 0x80000000` from a KSEG0 conversion,
which yields an "address" far outside the program. Given the .elf, each
bad word is mapped back to its function and source line.

Exit status: 0 if clean, 1 if any relocation is invalid.
"""

import shutil
import struct
import subprocess
import sys
from pathlib import Path

CODE_BASE = 0x00100000   # where 3dsx code is linked/loaded


def check(path):
    d = Path(path).read_bytes()
    magic, hsz, rsz = struct.unpack_from("<4sHH", d, 0)
    if magic != b"3DSX":
        raise SystemExit(f"{path}: not a 3DSX file")
    code, ro, data, bss = struct.unpack_from("<IIII", d, 16)  # after version, flags
    ntab = rsz // 4

    off = hsz
    relocs = []
    for _ in range(3):
        relocs.append(struct.unpack_from(f"<{ntab}I", d, off))
        off += rsz

    segs = [(n + 0xFFF) & ~0xFFF for n in (code, ro, data)]
    img = bytearray(sum(segs))
    p = off
    img[0:code] = d[p:p + code]; p += code
    img[segs[0]:segs[0] + ro] = d[p:p + ro]; p += ro
    img[segs[0] + segs[1]:segs[0] + segs[1] + data - bss] = d[p:p + data - bss]
    p += data - bss

    bad = []
    starts = [0, segs[0], segs[0] + segs[1]]
    for seg in range(3):
        pos, end = starts[seg], starts[seg] + segs[seg]
        for table in range(ntab):
            for _ in range(relocs[seg][table]):
                skip, patch = struct.unpack_from("<HH", d, p)
                p += 4
                if table >= 2:
                    continue
                pos += skip * 4
                for _ in range(patch):
                    if pos >= end:
                        break
                    word = struct.unpack_from("<I", img, pos)[0]
                    sub = word >> 28
                    if (table == 0 and sub != 0) or (table == 1 and sub > 1):
                        bad.append((pos, word, "absolute" if table == 0 else "relative"))
                    pos += 4
    return bad


def locate(elf, addrs):
    """addr -> 'function at file:line', using addr2line if it's around."""
    tool = shutil.which("arm-none-eabi-addr2line")
    if not tool:
        guess = Path(__file__).resolve().parent.parent / ".tools/devkitpro/devkitARM/bin/arm-none-eabi-addr2line.exe"
        tool = str(guess) if guess.is_file() else None
    if not tool or not elf:
        return {}
    out = subprocess.run([tool, "-f", "-e", elf] + [hex(a) for a in addrs],
                         capture_output=True, text=True).stdout.split("\n")
    return {a: f"{out[2 * i]} at {out[2 * i + 1]}" for i, a in enumerate(addrs) if 2 * i + 1 < len(out)}


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    bad = check(sys.argv[1])
    if not bad:
        print(f"check_3dsx: {sys.argv[1]}: relocations OK")
        return
    where = locate(sys.argv[2] if len(sys.argv) == 3 else None,
                   [CODE_BASE + pos for pos, _, _ in bad])
    print(f"check_3dsx: {len(bad)} invalid relocation(s); loaders will reject this file:")
    for pos, word, kind in bad:
        loc = where.get(CODE_BASE + pos, "")
        print(f"  {kind} reloc at {CODE_BASE + pos:#010x} = {word:#010x}  {loc}")
    sys.exit(1)


if __name__ == "__main__":
    main()
