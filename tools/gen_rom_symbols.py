#!/usr/bin/env python3
"""
gen_rom_symbols.py - define the decomp's ROM asset symbols as ROM offsets.

On N64 every game file (models, levels, setups, text, music, demos, ...)
is placed into the ROM by the linker, and the game DMAs a file from ROM
using the *address* of its symbol: romCopy(buf, &PcctvZ, size). The
ROM-only segments are linked with VMA == ROM position, so that address is
the file's ROM offset.

This port loads the user's ROM into a 12 MB buffer, ge_rom_image, which
this file defines as a .bss section of the executable. Each symbol is
defined at its ROM offset inside that buffer:

    .global PcctvZ
    .set    PcctvZ, ge_rom_image + 0x7BCB20

so &PcctvZ is a real address in the program. That matters because .3dsx
files are relocatable and every address constant must point into the
image (plain absolute symbols make 3dsxtool fail with "Relocation to
invalid address"). The PI shim (src/ultra/os_pi.c) subtracts
ge_rom_image to get back the ROM offset.

Offsets come from the decomp's own file list (scripts/filelist.u.csv,
the offsets its extractor reads from) and its asset .s files.

    python tools/gen_rom_symbols.py <decomp dir> <output .s>

Covers:
  * every label in assets/**/*.s (obseg models/levels/setups/text, music
    banks, ramrom demos, romfiles2), including the end_<name> labels
  * the linker-script segment symbols (_<seg>SegmentRomStart/RomEnd/
    Start/End) for segments the game DMAs into RAM itself. The game only
    ever uses Start/End to compute End - Start (a size), so they are
    placed at the ROM bounds too, which keeps that difference exact.
"""

import csv
import re
import sys
from pathlib import Path

VERSION_DEFINES = {"VERSION_US"}
ROM_SIZE = 12 * 1024 * 1024


def load_filelist(decomp):
    """ROM offset and size of every extracted file, keyed by its path with
    no extension (the .s files incbin build products like .rz / .seg
    whose stems match the extracted .bin)."""
    files = {}
    for row in csv.reader(open(decomp / "scripts" / "filelist.u.csv", newline="")):
        if len(row) < 3:
            continue
        off, size, path = int(row[0]), int(row[1]), row[2].strip()
        files[norm(path)] = (off, size)
    # Region builds keep some files in a u/ subfolder (setup/u, text/u)
    # while ob_seg.s names the generic path; allow lookup by unique stem.
    stems = {}
    for k, v in list(files.items()):
        stems.setdefault(k.rsplit("/", 1)[-1], []).append(v)
    for k, v in list(files.items()):
        parent, stem = k.rsplit("/", 1)
        if parent.endswith("/u") and len(stems[stem]) == 1:
            files.setdefault(parent[:-2] + "/" + stem, v)
    return files


def norm(path):
    p = path.replace("\\", "/")
    p = re.sub(r"^build/[a-z]/", "", p)          # build/u/assets/... -> assets/...
    return re.sub(r"\.(bin|rz|seg)$", "", p)   # keep .ctl/.tbl: sfx.ctl != sfx.tbl


def align(x, a):
    return (x + a - 1) // a * a


def expand_obseg(line):
    """ob_seg.s macro calls -> (labels, incbin path, align_after)."""
    m = re.match(r"\s*obseg_file_(?:rz|Z)\s+([\w/]+)\s*,\s*(\w+)", line)
    if m:
        path, name = m.groups()
        return [name], f"assets/obseg/{path}/{name}", 16, f"end_{name}"
    m = re.match(r"\s*bg_file_seg\s+(\w+)\s*,\s*(\w+)", line)
    if m:
        name, sname = m.groups()
        return [name], f"assets/obseg/bg/{sname}", None, f"end_{name}"
    return None


def scan_asm(path, files, out, problems):
    """Walk one assets .s file, assigning ROM offsets to labels."""
    pending = []       # labels waiting for the next incbin
    pos = None         # ROM position just after the last incbin
    skip = []          # .ifdef stack (True = skipping)
    in_macro = False

    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.split("#")[0].rstrip()
        s = line.strip()
        if not s:
            continue
        if s.startswith(".macro"):
            in_macro = True
            continue
        if s.startswith(".endm"):
            in_macro = False
            continue
        if in_macro:
            continue
        if s.startswith(".ifdef"):
            skip.append(s.split()[1] not in VERSION_DEFINES)
            continue
        if s.startswith(".ifndef"):
            skip.append(s.split()[1] in VERSION_DEFINES)
            continue
        if s.startswith(".else"):
            skip[-1] = not skip[-1]
            continue
        if s.startswith(".endif"):
            skip.pop()
            continue
        if any(skip):
            continue

        mac = expand_obseg(s)
        if mac:
            labels, inc, al, endlabel = mac
            key = norm(inc)
            if key not in files:
                problems.append(f"{path.name}: no ROM entry for {inc}")
                continue
            off, size = files[key]
            for l in pending + labels:
                out[l] = off
            pending = []
            pos = off + size
            if al:
                pos = align(pos, al)
            out[endlabel] = pos
            continue

        m = re.match(r"([A-Za-z_.][\w.]*):$", s)
        if m:
            pending.append(m.group(1))
            continue
        m = re.match(r'\.incbin\s+"([^"]+)"', s)
        if m:
            key = norm(m.group(1))
            if key not in files:
                problems.append(f"{path.name}: no ROM entry for {m.group(1)}")
                pending = []
                continue
            off, size = files[key]
            for l in pending:
                out[l] = off
            pending = []
            pos = off + size
            continue
        m = re.match(r"\.balign\s+(\d+)", s)
        if m and pos is not None:
            pos = align(pos, int(m.group(1)))
            continue
        if s.startswith((".global", ".section", ".set", ".include")):
            continue
        # Data directives (.word/.half/...) aren't tracked; labels right
        # after an incbin (the *_end / *RomEnd ones) were resolved below.
        if pending and pos is not None and not s.startswith("."):
            continue

    # Labels left pending at the end follow the last incbin.
    for l in pending:
        if pos is not None:
            out[l] = pos


# Segments the game DMAs into RAM itself (C data in the N64 build), in
# ge007.ld order: (segment, file that starts it, N64 RAM/VMA start). The
# VMA is kept for reference only (see the docstring for Start/End).
# RomEnd is the next segment's RomStart.
DMA_SEGMENTS = [
    ("fontdl",            "assets/ge007.u.117880.jfont_dl",          0x01000000),
    ("jfontchardata",     "assets/ge007.u.117940.jfont_chardata",    0x00000000),
    ("efontchardata",     "assets/ge007.u.123040.efont_chardata",    0x00000000),
    ("animation_entries", "assets/animationtable_entries",           0x00000000),
    ("animation_data",    "assets/animationtable_data",              0x00000000),
    ("Globalimagetable",  "assets/ge007.u.29D160.Globalimagetable",   0x02000000),
    ("rarewarelogo",      "assets/rarewarelogo",                     0x02000000),
    ("romfiles2",         "assets/ge007.u.2A4D50.usedby7F008DE4",    None),
    ("ramromfiles",       "assets/ramrom/ramrom_Dam_1",              None),
    ("fontbankgothic",    "assets/font/fontBankGothic_kerning",      0x00000000),
    ("fontzurichbold",    "assets/font/fontZurichBold_kerning",      0x00000000),
    ("musicfiles",        "assets/music/sfx.ctl",                    None),
    ("obseg",             None,                                      None),  # = first ob_seg label
    ("images",            None,                                      None),  # = first imagelist entry
]


# Labels the .s files put in front of data directives rather than an
# incbin, so the scan above can't place them: label -> extracted file.
DATA_LABELS = {
    "_musicsampletblSegmentRomStart": "assets/music/music.sbk",
}


def segment_symbols(decomp, files, labels, out, problems):
    for label, path in DATA_LABELS.items():
        if path in files:
            out[label] = files[path][0]
        else:
            problems.append(f"{label}: {path} not in file list")
    starts = []
    for name, first, vma in DMA_SEGMENTS:
        if name == "obseg":
            off = labels.get("bg_sev_all_p_seg")  # the first file in ob_seg.s
        elif name == "images":
            row = next(csv.reader(open(decomp / "imagelist.u.csv", newline="")))
            off = int(row[0])
        else:
            off = files.get(first, (None,))[0]
        if off is None:
            problems.append(f"segment {name}: start not found")
        starts.append((name, off, vma))

    for i, (name, off, vma) in enumerate(starts):
        if off is None:
            continue
        nxt = starts[i + 1][1] if i + 1 < len(starts) else None
        out[f"_{name}SegmentRomStart"] = off
        if nxt is not None:
            out[f"_{name}SegmentRomEnd"] = nxt
        out[f"_{name}SegmentStart"] = off
        if nxt is not None:
            out[f"_{name}SegmentEnd"] = nxt


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    decomp, outpath = Path(sys.argv[1]), Path(sys.argv[2])
    files = load_filelist(decomp)
    out, problems = {}, []

    for s in sorted((decomp / "assets").rglob("*.s")):
        scan_asm(s, files, out, problems)
    segment_symbols(decomp, files, out, out, problems)

    if problems:
        for p in problems:
            print("gen_rom_symbols:", p, file=sys.stderr)
        sys.exit(1)

    # GoldenEye's ROM is 12 MB; leave room for a symbol at the very end.
    size = max(ROM_SIZE, align(max(out.values()) + 16, 16))
    lines = ["/* Generated by tools/gen_rom_symbols.py from the decomp's file list.",
             " * ge_rom_image holds the ROM at runtime; each symbol sits at its ROM",
             " * offset inside it (see the script for why). */",
             '.section .bss.ge_rom_image, "aw", %nobits',
             ".balign 64",
             ".global ge_rom_image",
             "ge_rom_image:",
             f".space 0x{size:X}",
             ".global ge_rom_image_end",
             "ge_rom_image_end:",
             ""]
    for name in sorted(out):
        lines.append(f".global {name}")
        lines.append(f".set {name}, ge_rom_image + 0x{out[name]:X}")
    outpath.parent.mkdir(parents=True, exist_ok=True)
    outpath.write_text("\n".join(lines) + "\n", newline="\n")
    print(f"gen_rom_symbols: {len(out)} symbols -> {outpath}")


if __name__ == "__main__":
    main()
