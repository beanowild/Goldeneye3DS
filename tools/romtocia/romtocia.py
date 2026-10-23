"""
romtocia.py - turns your own GoldenEye 007 ROM into an installable 3DS CIA.

    py -3 romtocia.py "GoldenEye 007 (USA).z64"   (or drop the ROM on "ROM to CIA.bat")

Shipped as a folder by build_py.py: this script, payload/ (the compiled
port, whose only game data is what the public n64decomp/007 repo already
has; makerom, bannertool, the banner sound) and lib/ (the banner code from
tools/banner and tools/cia); nothing from the ROM. On first use it installs
Pillow and gltflib with pip and downloads pycgfx (at a fixed commit) into
lib/. On the player's PC it:
  1. checks the ROM is the NTSC-U release (.z64, .v64 or .n64)
  2. cuts Bond, the PP7, the logo and their textures out of it and builds
     the 3D HOME Menu banner (tools/banner: export.py, banner_glb.py, pycgfx)
  3. packs the port, the banner and the ROM (in the RomFS, romfs:/rom.z64)
     into "GoldenEye 007 (3DS).cia" next to the ROM
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
PYCGFX_COMMIT = "1f78850086f3a77c41e07162e842f97a5bf3c18a"   # github.com/skyfloogle/pycgfx

# bundled by build_exe.py; the shipped folder (build_py.py); else a source
# checkout, with the repo's own files
if getattr(sys, "frozen", False):
    PAYLOAD = Path(sys._MEIPASS) / "payload"
elif (HERE / "lib").is_dir():
    PAYLOAD = HERE / "payload"
    sys.path[:0] = [str(HERE / "lib"), str(HERE / "lib" / "pycgfx")]
else:
    ROOT = Path(__file__).resolve().parents[2]
    PAYLOAD = Path(__file__).resolve().parent / "payload"
    sys.path[:0] = [str(ROOT / "tools" / "banner"), str(ROOT / "tools" / "cia"),
                    str(ROOT / ".tools" / "pycgfx")]

GE_US_SHA1 = "abe01e4aeb033b6c0836819f549c791b26cfde83"
OUT_NAME = "GoldenEye 007 (3DS).cia"

# Banner models: Rare 1172 files (2-byte header, then raw deflate), as
# (ROM offset, size) from the decomp's scripts/filelist.u.csv.
MODELS = {
    "obseg/chr/CdjbondZ.bin": (7382992, 11920),
    "obseg/chr/CheadbrosnanZ.bin": (7445856, 3408),
    "obseg/prop/PchrwppksilZ.bin": (8190288, 736),
    "obseg/prop/PgoldeneyelogoZ.bin": (8262192, 3760),
}
# Their textures, stored as is: texture number -> (ROM offset, size)
TEXTURES = {
    873: (0x9D7FCB, 400), 878: (0x9D8441, 57), 879: (0x9D847A, 81), 887: (0x9D90DE, 364),
    1514: (0xA7EA42, 12), 1595: (0xA95D33, 411), 1604: (0xA7EA42, 12), 1607: (0xA97AD7, 632),
    1608: (0xA97D4F, 12), 1609: (0xA97D5B, 983), 1613: (0xA98A2F, 1436),
    1616: (0xA994B5, 1268), 1619: (0xA99EAD, 1174), 1622: (0xA9A87C, 1255),
    1624: (0xA9AF15, 2144), 1625: (0xA9B775, 2170), 1626: (0xA9BFEF, 1788),
    1627: (0xA9C6EB, 335), 1628: (0xA9C83A, 762), 1630: (0xA9CE33, 308),
    1631: (0xA9CF67, 724), 2064: (0xB2799A, 581),
}


class Fail(Exception):
    """A message for the player; no traceback."""


def to_z64(data):
    """The ROM in .z64 (big-endian) order (as builder.py)."""
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data
    if magic == b"\x37\x80\x40\x12":            # .v64: 16-bit halves swapped
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if magic == b"\x40\x12\x37\x80":            # .n64: 32-bit words reversed
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(out)
    raise Fail("This file is not an N64 ROM.")


def cut_assets(rom, assets):
    """The files tools/banner reads from the decomp's assets/, from the ROM."""
    for name, (off, size) in MODELS.items():
        d = rom[off:off + size]
        if d[:2] != b"\x11\x72":
            raise Fail(f"Unexpected ROM data at {off:#x}.")
        (assets / name).parent.mkdir(parents=True, exist_ok=True)
        (assets / name).write_bytes(zlib.decompressobj(-15).decompress(d[2:]))
    split = assets / "images" / "split"
    split.mkdir(parents=True, exist_ok=True)
    for num, (off, size) in TEXTURES.items():
        (split / f"{num}.bin").write_bytes(rom[off:off + size])
    # the animation tables: public decomp source, bundled
    for f in (PAYLOAD / "decomp").iterdir():
        (assets / f.name).write_bytes(f.read_bytes())


def ensure_deps():
    """First run of the shipped folder: Pillow and gltflib from pip, pycgfx
    from GitHub into lib/pycgfx."""
    if getattr(sys, "frozen", False) or not (HERE / "lib").is_dir():
        return
    missing = []
    for mod, pkg in (("PIL", "pillow"), ("gltflib", "gltflib")):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if missing:
        print(f"First run: installing {', '.join(missing)} with pip...")
        user = ["--user"] if sys.prefix == sys.base_prefix else []   # not in a venv
        r = subprocess.run([sys.executable, "-m", "pip", "install", *user, *missing])
        if r.returncode:
            raise Fail("pip couldn't install " + ", ".join(missing) + ". Run:\n  "
                       + f'"{sys.executable}" -m pip install ' + " ".join(missing))
        import site, importlib
        site.addsitedir(site.getusersitepackages())
        importlib.invalidate_caches()
    dest = HERE / "lib" / "pycgfx"
    if not (dest / "main.py").is_file():
        import io, shutil, urllib.request, zipfile
        url = f"https://github.com/skyfloogle/pycgfx/archive/{PYCGFX_COMMIT}.zip"
        print("First run: downloading pycgfx (the banner converter)...")
        try:
            data = urllib.request.urlopen(url, timeout=60).read()
        except Exception as e:
            raise Fail(f"Couldn't download {url}\n({e}). Check the internet connection.")
        tmp = HERE / "lib" / "pycgfx.tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            top = f"pycgfx-{PYCGFX_COMMIT}/"
            for name in z.namelist():
                if name.startswith(top) and (name.endswith(".py") or "/cgfx/" in name) and not name.endswith("/"):
                    out = tmp / name[len(top):]
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(z.read(name))
        if not (tmp / "main.py").is_file():
            raise Fail("The pycgfx download didn't have main.py.")
        shutil.rmtree(dest, ignore_errors=True)
        tmp.rename(dest)
        # on sys.path since start-up, when it didn't exist yet
        import importlib
        sys.path_importer_cache.pop(str(dest), None)
        importlib.invalidate_caches()


def build_banner(rom, work):
    """banner.cgfx built from the cut files (tools/banner/build.ps1 without Blender)."""
    assets = work / "assets"
    cut_assets(rom, assets)
    os.environ["GE_ASSETS"] = str(assets)
    import export, banner_glb, main as pycgfx  # read GE_ASSETS on import
    data, glb, cgfx = work / "export", work / "banner.glb", work / "banner.cgfx"
    export.main(str(data))
    banner_glb.main(str(data), str(glb))
    argv, sys.argv = sys.argv, ["pycgfx", str(glb), str(cgfx)]
    try:
        pycgfx.main()
    finally:
        sys.argv = argv
    return cgfx


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        raise Fail(f"{Path(str(cmd[0])).name} failed:\n{r.stdout}{r.stderr}")


def make_cia(rom_path):
    import make_cia
    print(f"ROM: {rom_path}")
    rom = to_z64(Path(rom_path).read_bytes())
    if hashlib.sha1(rom).hexdigest() != GE_US_SHA1:
        raise Fail("This isn't the GoldenEye 007 (USA) ROM, or it has been modified.\n"
                   "Only the unmodified NTSC-U release is supported.")
    print("ROM OK: GoldenEye 007 (USA)")
    out = Path(rom_path).resolve().with_name(OUT_NAME)
    ensure_deps()
    with tempfile.TemporaryDirectory(prefix="ge007cia_") as tmp:
        work = Path(tmp)
        print("Building the HOME Menu banner from the ROM...")
        cgfx = build_banner(rom, work)
        banner = work / "banner.bin"
        run(PAYLOAD / "bannertool.exe", "makebanner", "-ci", cgfx,
            "-a", PAYLOAD / "banner_sound.wav", "-o", banner)
        print("Packing the CIA...")
        z64 = work / "rom.z64"
        z64.write_bytes(rom)
        make_cia.__file__ = str(PAYLOAD / "make_cia.py")  # ge007.rsf sits beside it
        rsf = make_cia.romfs_rsf(z64, work)
        major, minor, micro = make_cia.VERSION.split(".")
        run(PAYLOAD / "makerom.exe", "-f", "cia", "-target", "t", "-exefslogo",
            "-rsf", rsf, "-elf", PAYLOAD / "ge007.elf", "-icon", PAYLOAD / "ge007.smdh",
            "-banner", banner, f"-DAPP_TITLE={make_cia.TITLE[:8]}",
            f"-DAPP_PRODUCT_CODE={make_cia.PRODUCT_CODE}",
            f"-DAPP_UNIQUE_ID=0x{make_cia.UNIQUE_ID:05X}", f"-DAPP_VERSION_MAJOR={major}",
            "-major", major, "-minor", minor, "-micro", micro, "-o", out)
    print(f"\nDone: {out}")
    print("Install it with FBI (or any CIA installer). Don't share it: it contains your ROM.")


def pick_rom():
    """The ROM: dropped on the exe, else the only N64 ROM beside it, else ask."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    here = Path(sys.executable if getattr(sys, "frozen", False) else __file__).parent
    roms = [p for ext in ("*.z64", "*.v64", "*.n64") for p in here.glob(ext)]
    if len(roms) == 1:
        return roms[0]
    try:
        import tkinter
        from tkinter import filedialog
        tkinter.Tk().withdraw()
        return filedialog.askopenfilename(
            title="Choose your GoldenEye 007 (USA) ROM",
            filetypes=[("N64 ROM", "*.z64 *.v64 *.n64"), ("All files", "*.*")]) or None
    except Exception:
        return None


def main():
    print("GoldenEye 007 3DS: ROM to CIA\n")
    code = 0
    try:
        rom = pick_rom()
        if not rom:
            raise Fail('Drag your GoldenEye 007 (USA) ROM onto "ROM to CIA.bat".')
        make_cia(rom)
    except Fail as e:
        print(f"\nError: {e}")
        code = 1
    except Exception:
        import traceback
        traceback.print_exc()
        code = 1
    if getattr(sys, "frozen", False):
        input("\nPress Enter to close.")
    sys.exit(code)


if __name__ == "__main__":
    main()
