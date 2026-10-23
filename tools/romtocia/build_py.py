"""
build_py.py - builds dist/romtocia/ (and dist/romtocia.zip): the ROM-to-CIA
maker as a plain Python script, instead of build_exe.py's PyInstaller exe
(which antivirus engines flag: its self-unpacking loader looks like malware).

    py -3 tools/romtocia/build_py.py [--sound file.wav]

Needs a finished build (builder.py) and .tools/ as for build_exe.py. The
folder holds romtocia.py, "ROM to CIA.bat" (drop the ROM on it), payload/
(see build_exe.gather_payload) and lib/ (the banner code). Nothing from the
ROM goes in. pycgfx is not shipped: romtocia.py downloads it on first use.
"""
import argparse
import shutil
import zipfile
from pathlib import Path

import build_exe
from build_exe import ROOT, PAYLOAD

LIB = ["tools/banner/export.py", "tools/banner/banner_glb.py", "tools/banner/gemodel.py",
       "tools/banner/geanim.py", "tools/banner/getex.py", "tools/banner/pose.py",
       "tools/cia/make_cia.py"]

BAT = r"""@echo off
rem Drop your GoldenEye 007 (USA) ROM on this file.
setlocal
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if defined PY %PY% -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul || set "PY="
if not defined PY (
    echo Python 3.8 or newer is needed: https://www.python.org/downloads/
    echo When installing, tick "Add python.exe to PATH".
    pause
    exit /b 1
)
%PY% "%~dp0romtocia.py" %*
pause
"""

README = """GoldenEye 007 3DS: ROM to CIA
=============================

Turns your own GoldenEye 007 (USA) N64 ROM into an installable 3DS CIA.

1. Install Python 3 from https://www.python.org/downloads/ if you don't have
   it (tick "Add python.exe to PATH").
2. Drag your ROM (.z64, .v64 or .n64) onto "ROM to CIA.bat".
   (Or put the ROM in this folder and double-click the .bat.)
3. "GoldenEye 007 (3DS).cia" appears next to your ROM. Install it with FBI.

The first run needs the internet: it installs Pillow and gltflib with pip and
downloads pycgfx (github.com/skyfloogle/pycgfx) into lib/.

Only the unmodified NTSC-U ROM works. The CIA contains your ROM: don't share
it. Nothing from the ROM is in this folder.

Included tools: makerom (github.com/3DSGuy/Project_CTR) and bannertool
(github.com/Steveice10/bannertool), both MIT licensed.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sound", type=Path, default=ROOT / "banner" / "Banner new.wav",
                    help="the HOME Menu banner sound (any WAV)")
    args = ap.parse_args()
    build_exe.gather_payload(args.sound)

    out = ROOT / "dist" / "romtocia"
    shutil.rmtree(out, ignore_errors=True)
    (out / "lib").mkdir(parents=True)
    shutil.copytree(PAYLOAD, out / "payload")
    shutil.copy(ROOT / "tools" / "romtocia" / "romtocia.py", out)
    for f in LIB:
        shutil.copy(ROOT / f, out / "lib")
    (out / "ROM to CIA.bat").write_bytes(BAT.replace("\n", "\r\n").encode())
    (out / "README.txt").write_bytes(README.replace("\n", "\r\n").encode())

    z = ROOT / "dist" / "romtocia.zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(out.rglob("*")):
            if p.is_file():
                zf.write(p, Path("romtocia") / p.relative_to(out))
    print(f"wrote {out} and {z} ({z.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
