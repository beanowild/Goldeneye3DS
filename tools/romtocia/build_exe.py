"""
build_exe.py - builds dist/romtocia.exe, the drop-your-ROM-here CIA maker.

    .tools/venv/Scripts/python.exe tools/romtocia/build_exe.py [--sound file.wav]

Needs a finished build (builder.py: build/ge007.elf and .smdh, and the
decomp checkout for its animation tables) and from .tools/: devkitARM's
strip, makerom, bannertool, pycgfx and the venv (with pyinstaller,
pillow, gltflib). Gathers tools/romtocia/payload/ and freezes romtocia.py
with it. Nothing from the ROM goes in: romtocia.py cuts what it needs from
the player's own ROM.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLS = ROOT / ".tools"
PAYLOAD = HERE / "payload"
ASSETS = ROOT / "external" / "goldeneye" / "assets"
DECOMP_FILES = ["animationtable_data.c", "animationtable_data.h",
                "animationtable_entries.c", "animationtable_entries.h"]

sys.path.insert(0, str(ROOT / "tools" / "cia"))
import make_cia  # noqa: E402


def run(*cmd):
    print(">", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True)


def gather_payload(sound):
    """tools/romtocia/payload: the stripped port, its SMDH and RSF, makerom,
    bannertool, the banner sound and the decomp's animation tables."""
    shutil.rmtree(PAYLOAD, ignore_errors=True)
    (PAYLOAD / "decomp").mkdir(parents=True)

    # the port, without its debug info (12 MB -> ~2 MB)
    run(TOOLS / "devkitpro" / "devkitARM" / "bin" / "arm-none-eabi-strip.exe",
        "--strip-debug", "-o", PAYLOAD / "ge007.elf", ROOT / "build" / "ge007.elf")
    shutil.copy(ROOT / "build" / "ge007.smdh", PAYLOAD)
    shutil.copy(ROOT / "tools" / "cia" / "ge007.rsf", PAYLOAD)
    shutil.copy(TOOLS / "makerom" / "makerom.exe", PAYLOAD)
    shutil.copy(TOOLS / "bannertool" / "windows-x86_64" / "bannertool.exe", PAYLOAD)
    make_cia.banner_sound(sound, PAYLOAD / "banner_sound.wav")
    for f in DECOMP_FILES:
        shutil.copy(ASSETS / f, PAYLOAD / "decomp")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sound", type=Path, default=ROOT / "banner" / "Banner new.wav",
                    help="the HOME Menu banner sound (any WAV)")
    args = ap.parse_args()
    gather_payload(args.sound)

    work = ROOT / "build" / "romtocia"
    work.mkdir(parents=True, exist_ok=True)
    from PIL import Image
    Image.open(ROOT / "res" / "icon.png").save(work / "icon.ico")

    sep = ";" if sys.platform == "win32" else ":"
    run(TOOLS / "venv" / "Scripts" / "python.exe", "-m", "PyInstaller",
        "--noconfirm", "--onefile", "--console", "--name", "romtocia",
        "--icon", work / "icon.ico",
        "--distpath", ROOT / "dist", "--workpath", work, "--specpath", work,
        "--paths", ROOT / "tools" / "banner", "--paths", ROOT / "tools" / "cia",
        "--paths", TOOLS / "pycgfx",
        *[a for m in ("export", "banner_glb", "main", "make_cia", "gemodel", "geanim", "getex")
          for a in ("--hidden-import", m)],
        "--add-data", f"{PAYLOAD}{sep}payload",
        "--add-data", f"{ROOT / 'tools' / 'banner' / 'pose.py'}{sep}.",
        HERE / "romtocia.py")
    exe = ROOT / "dist" / "romtocia.exe"
    print(f"wrote {exe} ({exe.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
