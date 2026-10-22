"""
make_cia.py - packs the built port into an installable CIA, with the 3D
HOME Menu banner (tools/banner) and its sound.

    py -3 tools/cia/make_cia.py [--sound "banner/Banner new.wav"] [--out dist/ge007.cia]

Uses build/ge007.elf and build/ge007.smdh (run builder.py first),
banner/banner.cgfx (tools/banner/build.ps1), and from .tools/: bannertool
and makerom. Without --rom the game data comes from the SD card at run
time (sdmc:/3ds/ge007/rom.z64), as for the .3dsx; with --rom the ROM goes
into the CIA's RomFS (romfs:/rom.z64, tried first) and the CIA is complete.
Never publish a CIA made with --rom. tools/romtocia makes one on the
player's own PC instead.

The sound: any WAV (PCM or float, mono or stereo). The HOME Menu wants
stereo 16-bit and no longer than 2.9375 s (longer sounds glitch or stop
the banner loading), so it is converted to that at 32728 Hz and, if
needed, cut with a short fade at the end.
"""
import argparse
import array
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / ".tools"
BANNERTOOL = TOOLS / "bannertool" / "windows-x86_64" / "bannertool.exe"
MAKEROM = TOOLS / "makerom" / "makerom.exe"

TITLE = "GoldenEye"
PRODUCT_CODE = "CTR-P-GE07"
UNIQUE_ID = 0xE0070          # homebrew range (0x0F800-0xFF7FF)
MAX_SECONDS = 2.9375
FADE_SECONDS = 0.02
SOUND_RATE = 32728           # the 3DS's own output rate, as official banners use
VERSION = "1.0.0"            # bump so a new CIA installs over the last one


def read_wav(path):
    """(rate, channels, [float samples, interleaved]) from a PCM or float WAV."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    fmt = pcm = None
    o = 12
    while o + 8 <= len(data):
        cid, size = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
        body = data[o + 8:o + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"data":
            pcm = body
        o += 8 + size + (size & 1)
    if not fmt or pcm is None:
        sys.exit(f"{path}: no fmt or data chunk")
    tag, channels, rate, _, _, bits = fmt
    if tag == 0xFFFE:                         # WAVE_FORMAT_EXTENSIBLE
        tag = 3 if bits == 32 and b"\x03\x00\x00\x00\x00\x00\x10\x00" in data[:200] else 1
    if tag == 3 and bits == 32:
        a = array.array("f")
        a.frombytes(pcm[:len(pcm) // 4 * 4])
        samples = list(a)
    elif tag == 1 and bits == 16:
        a = array.array("h")
        a.frombytes(pcm[:len(pcm) // 2 * 2])
        samples = [x / 32768.0 for x in a]
    elif tag == 1 and bits == 24:
        samples = [int.from_bytes(pcm[i:i + 3], "little", signed=True) / 8388608.0
                   for i in range(0, len(pcm) - 2, 3)]
    else:
        sys.exit(f"{path}: unsupported WAV format (tag {tag}, {bits} bits)")
    return rate, channels, samples


def resample(s, rate, to):
    """Interleaved stereo s from rate to `to` (linear interpolation)."""
    n = int(len(s) // 2 * to / rate)
    out = []
    for i in range(n):
        x = i * rate / to
        j = int(x)
        f = x - j
        k = min(j + 1, len(s) // 2 - 1)
        out += [s[2 * j] * (1 - f) + s[2 * k] * f, s[2 * j + 1] * (1 - f) + s[2 * k + 1] * f]
    return out


def banner_sound(src, dst, silent=False):
    if silent:
        rate, s = SOUND_RATE, [0.0] * (2 * SOUND_RATE)
    else:
        rate, channels, s = read_wav(src)
        if channels == 1:
            s = [x for v in s for x in (v, v)]
        elif channels != 2:
            sys.exit(f"{src}: {channels} channels; use mono or stereo")
        if rate != SOUND_RATE:
            s = resample(s, rate, SOUND_RATE)
            rate = SOUND_RATE
    frames = len(s) // 2
    keep = min(frames, int(MAX_SECONDS * rate))
    fade = int(FADE_SECONDS * rate) if keep < frames else 0
    out = array.array("h")
    for i in range(keep):
        g = min(1.0, (keep - i) / fade) if fade else 1.0
        for c in range(2):
            out.append(max(-32768, min(32767, round(s[2 * i + c] * g * 32767))))
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(out.tobytes())
    cut = f", cut from {frames / rate:.3f} s" if keep < frames else ""
    print(f"sound: {keep / rate:.3f} s, {rate} Hz stereo{cut}")


def run(cmd):
    print(">", " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.stdout.strip():
        print(r.stdout.strip())
    if r.returncode:
        print(r.stderr.strip())
        sys.exit(f"failed: {Path(str(cmd[0])).name}")


def romfs_rsf(rom, work):
    """ge007.rsf plus a RomFS holding rom (as rom.z64); returns the new rsf."""
    romfs = work / "romfs"
    romfs.mkdir(parents=True, exist_ok=True)
    (romfs / "rom.z64").write_bytes(Path(rom).read_bytes())
    rsf = work / "ge007_romfs.rsf"
    text = Path(__file__).with_name("ge007.rsf").read_text()
    rsf.write_text(text.rstrip() + f"\n\nRomFs:\n  RootPath: {romfs.as_posix()}\n")
    return rsf


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1].strip())
    ap.add_argument("--sound", default=ROOT / "banner" / "Banner new.wav", type=Path)
    ap.add_argument("--cgfx", default=ROOT / "banner" / "banner.cgfx", type=Path)
    ap.add_argument("--elf", default=ROOT / "build" / "ge007.elf", type=Path)
    ap.add_argument("--smdh", default=ROOT / "build" / "ge007.smdh", type=Path)
    ap.add_argument("--out", default=ROOT / "dist" / "ge007.cia", type=Path)
    ap.add_argument("--version", default=VERSION, help="major.minor.micro")
    ap.add_argument("--rom", type=Path, help="put this .z64 ROM in the CIA's RomFS "
                                              "(for your own use only)")
    ap.add_argument("--silent", action="store_true", help="a second of silence for the sound")
    ap.add_argument("--flat", type=Path, help="a flat banner from this PNG instead of the CGFX")
    ap.add_argument("--uid", type=lambda x: int(x, 0), default=UNIQUE_ID,
                    help="unique ID (test builds use their own, to install side by side)")
    ap.add_argument("--title", default=TITLE, help="title in the HOME Menu")
    ap.add_argument("--icon", type=Path, help="icon PNG: makes a new SMDH with --title "
                                              "instead of using build/ge007.smdh")
    ap.add_argument("--cxi", action="store_true",
                    help="also write the program as a .cxi next to the CIA, which "
                         "Azahar boots directly (tools/run_azahar.py --app)")
    args = ap.parse_args()

    for p in (args.sound, args.cgfx, args.elf, args.smdh, BANNERTOOL, MAKEROM):
        if not Path(p).is_file():
            sys.exit(f"missing: {p}")
    work = ROOT / "build" / "cia"
    work.mkdir(parents=True, exist_ok=True)

    wav = work / "banner_sound.wav"
    banner_sound(args.sound, wav, args.silent)
    banner = work / "banner.bin"
    image = ["-i", args.flat] if args.flat else ["-ci", args.cgfx]
    run([BANNERTOOL, "makebanner", *image, "-a", wav, "-o", banner])
    smdh = args.smdh
    if args.icon:
        smdh = work / "icon.smdh"
        run([BANNERTOOL, "makesmdh", "-s", args.title, "-l", args.title,
             "-p", "Rare", "-i", args.icon, "-o", smdh])

    rsf = romfs_rsf(args.rom, work) if args.rom else Path(__file__).with_name("ge007.rsf")
    major, minor, micro = args.version.split(".")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    common = ["-target", "t", "-exefslogo",
              "-rsf", rsf,
              "-elf", args.elf, "-icon", smdh, "-banner", banner,
              f"-DAPP_TITLE={args.title[:8]}", f"-DAPP_PRODUCT_CODE={PRODUCT_CODE}",
              f"-DAPP_UNIQUE_ID=0x{args.uid:05X}", f"-DAPP_VERSION_MAJOR={major}"]
    run([MAKEROM, "-f", "cia", *common, "-major", major, "-minor", minor, "-micro", micro,
         "-o", args.out])
    if args.cxi:
        cxi = args.out.with_suffix(".cxi")
        run([MAKEROM, "-f", "cxi", *common, "-o", cxi])
        print(f"wrote {cxi}")
    print(f"wrote {args.out} ({args.out.stat().st_size // 1024} KB), title ID "
          f"00040000{args.uid:06X}00")


if __name__ == "__main__":
    main()
