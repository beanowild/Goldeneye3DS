#!/usr/bin/env python3
"""
dwarf_layout.py - print how GCC laid out a struct in the port's ELF.

    py -3 tools/dwarf_layout.py TYPE [TYPE ...] [--elf build/ge007.elf]

Each type is flattened to its leaf fields (nested structs and arrays are
expanded, union members listed with the union's offset) as
"offset size name", bitfields with their bit range. Used to write the
byte-swap tables for ROM data (src/port/*_swap.c): only fields narrower
than 4 bytes, and bitfields, need more than a 32-bit swap.

Needs pyelftools: py -3 -m pip install --target .tools/pylib pyelftools
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / ".tools" / "pylib"))
from elftools.elf.elffile import ELFFile  # noqa: E402


def strip(die):
    """Follow typedefs and const/volatile to the underlying type."""
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                                           "DW_TAG_volatile_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    return die


def size_of(die):
    die = strip(die)
    if die is None:
        return 0
    if "DW_AT_byte_size" in die.attributes:
        return die.attributes["DW_AT_byte_size"].value
    if die.tag == "DW_TAG_array_type":
        n = 1
        for sub in die.iter_children():
            if sub.tag == "DW_TAG_subrange_type":
                if "DW_AT_count" in sub.attributes:
                    n *= sub.attributes["DW_AT_count"].value
                elif "DW_AT_upper_bound" in sub.attributes:
                    n *= sub.attributes["DW_AT_upper_bound"].value + 1
                else:
                    n = 0
        return n * size_of(die.get_DIE_from_attribute("DW_AT_type"))
    return 0


def flatten(die, base, prefix, out, in_union=False):
    die = strip(die)
    if die is None:
        return
    if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        union = die.tag == "DW_TAG_union_type"
        for m in die.iter_children():
            if m.tag != "DW_TAG_member":
                continue
            name = m.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in m.attributes else "?"
            off = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in m.attributes else 0
            mt = m.get_DIE_from_attribute("DW_AT_type")
            if "DW_AT_bit_size" in m.attributes:
                bits = m.attributes["DW_AT_bit_size"].value
                if "DW_AT_data_bit_offset" in m.attributes:
                    dbo = m.attributes["DW_AT_data_bit_offset"].value
                    off, bit = dbo // 8 // size_of(mt) * size_of(mt), dbo % (8 * size_of(mt))
                else:
                    bit = m.attributes.get("DW_AT_bit_offset").value if "DW_AT_bit_offset" in m.attributes else 0
                out.append((base + off, size_of(mt), f"{prefix}{name}:{bits}@{bit}", True, in_union or union))
                continue
            flatten(mt, base + off, f"{prefix}{name}.", out, in_union or union)
            if union:
                pass
        return
    if die.tag == "DW_TAG_array_type":
        elem = die.get_DIE_from_attribute("DW_AT_type")
        es = size_of(elem)
        total = size_of(die)
        se = strip(elem)
        if es and se is not None and se.tag in ("DW_TAG_base_type", "DW_TAG_pointer_type",
                                                "DW_TAG_enumeration_type"):
            out.append((base, es, prefix.rstrip(".") + f"[{total // es}]", False, in_union))
            return
        for i in range(total // es if es else 0):
            flatten(elem, base + i * es, prefix.rstrip(".") + f"[{i}].", out, in_union)
        return
    out.append((base, size_of(die), prefix.rstrip("."), False, in_union))


def find_types(elf, names):
    found = {}
    dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag not in ("DW_TAG_typedef", "DW_TAG_structure_type"):
                continue
            if "DW_AT_name" not in die.attributes:
                continue
            n = die.attributes["DW_AT_name"].value.decode()
            if n in names and n not in found:
                d = strip(die)
                if d is not None and "DW_AT_byte_size" in d.attributes:
                    found[n] = d
        if len(found) == len(names):
            break
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("types", nargs="+")
    ap.add_argument("--elf", default=str(ROOT / "build" / "ge007.elf"))
    ap.add_argument("--narrow", action="store_true", help="only fields that aren't plain 4-byte words")
    args = ap.parse_args()
    with open(args.elf, "rb") as f:
        elf = ELFFile(f)
        found = find_types(elf, set(args.types))
        for t in args.types:
            if t not in found:
                print(f"{t}: not found")
                continue
            out = []
            flatten(found[t], 0, "", out)
            print(f"{t}: {size_of(found[t])} bytes")
            for off, size, name, bitf, uni in out:
                if args.narrow and size == 4 and not bitf:
                    continue
                print(f"  0x{off:03X} {size} {name}{'  (bitfield)' if bitf else ''}{'  (union)' if uni else ''}")


if __name__ == "__main__":
    main()
