#!/usr/bin/env python3
"""
builder.py - one-command build for the GoldenEye 007 3DS port.

    python builder.py --rom path/to/goldeneye.z64

Steps:
  1. verify   check the ROM is the unmodified NTSC-U release (SHA-1);
              .v64/.n64 byte orders are converted to .z64 first
  2. extract  run the decomp's own asset extractor on that ROM
  3. build    configure CMake with the devkitARM toolchain and build
  4. package  lay out dist/3ds/ge007/ to copy onto the SD card

Each step can be skipped (--skip-extract, --skip-build) or run alone
(--only verify|extract|build|package). Standard library only; Python 3.8+.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent

# NTSC-U "GoldenEye 007" (.z64 byte order). Same value as the decomp's
# ge007.u.sha1; that file is read first when present, so it is the authority.
GE_US_SHA1 = "abe01e4aeb033b6c0836819f549c791b26cfde83"

DECOMP_URL = "https://github.com/n64decomp/007"

# Port changes to the decomp (GCC/ARM portability, TARGET_3DS guards), and
# the decomp commit they were made against.
DECOMP_PATCH = ROOT / "patches" / "goldeneye-3ds.patch"
DECOMP_BASE = ROOT / "patches" / "goldeneye-3ds.base"
DEFAULT_DECOMP = ROOT / "external" / "goldeneye"
DEFAULT_BUILD = ROOT / "build"
DEFAULT_DIST = ROOT / "dist"

# Toolchain fetched by tools/fetch_toolchain.py. Preferred over anything
# installed system-wide, so a fetched toolchain always gives the same build.
TOOLS = ROOT / ".tools"

# Must match the first entry of OS_PORT_ROM_PATHS in include/ultra/os_port.h.
SD_DIR = Path("3ds") / "ge007"
SD_ROM_NAME = "rom.z64"

# Written into the decomp after a successful extraction, so the step can be
# skipped next time if the ROM hasn't changed.
EXTRACT_STAMP = ".ge3ds_extracted"

REGIONS = {ord("E"): "NTSC-U (USA)", ord("J"): "NTSC-J (Japan)", ord("P"): "PAL (Europe)"}


class BuildError(Exception):
    """A failure with a message meant for the user. No traceback is shown."""


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def step(title):
    print(f"\n==> {title}", flush=True)


def info(msg):
    print(f"    {msg}", flush=True)


def run(cmd, cwd=None, env=None):
    """Run a command, streaming its output. Raises BuildError on failure."""
    shown = " ".join(str(c) for c in cmd)
    info(f"$ {shown}")
    try:
        subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)
    except FileNotFoundError:
        raise BuildError(f"command not found: {cmd[0]}")
    except subprocess.CalledProcessError as e:
        raise BuildError(f"command failed (exit {e.returncode}): {shown}")


# ---------------------------------------------------------------------------
# 1. ROM verification
# ---------------------------------------------------------------------------

def to_z64(data):
    """Return the ROM in .z64 (big-endian) order, and the format it was in."""
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data, "z64"
    if magic == b"\x37\x80\x40\x12":            # 16-bit halves swapped
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out), "v64"
    if magic == b"\x40\x12\x37\x80":            # 32-bit words reversed
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(out), "n64"
    raise BuildError("this file is not an N64 ROM (unrecognised header)")


def expected_sha1(decomp):
    sha_file = decomp / "ge007.u.sha1"
    if sha_file.is_file():
        token = sha_file.read_text(encoding="ascii", errors="replace").split()
        if token and len(token[0]) == 40:
            return token[0].lower()
    return GE_US_SHA1


def verify_rom(rom_path, decomp):
    step("Verifying ROM")
    if not rom_path.is_file():
        raise BuildError(f"ROM not found: {rom_path}")

    raw = rom_path.read_bytes()
    if len(raw) < 0x1000 or len(raw) % 4:
        raise BuildError(f"{rom_path.name} is not an N64 ROM (size {len(raw)} bytes)")

    z64, fmt = to_z64(raw)
    if fmt != "z64":
        info(f"converted from .{fmt} byte order to .z64")

    name = z64[0x20:0x34].decode("ascii", errors="replace").strip(" \0")
    region = REGIONS.get(z64[0x3E], f"unknown region {z64[0x3E]:#04x}")
    info(f"header: '{name}', {region}, {len(z64) // (1024 * 1024)} MB")

    digest = hashlib.sha1(z64).hexdigest()
    want = expected_sha1(decomp)
    if digest != want:
        hints = []
        if name.upper() != "GOLDENEYE":
            hints.append("This doesn't look like GoldenEye 007 at all.")
        elif z64[0x3E] != ord("E"):
            hints.append(f"This is the {region} release; only NTSC-U is supported for now.")
        else:
            hints.append("It looks like GoldenEye NTSC-U but has been modified "
                         "(a hack, translation, trainer or bad dump).")
        raise BuildError(
            "ROM checksum mismatch.\n"
            f"      expected SHA-1 {want}\n"
            f"      got      SHA-1 {digest}\n"
            "    " + " ".join(hints) + "\n"
            "    You need an unmodified dump of the US cartridge.")

    info(f"SHA-1 OK ({digest})")
    return z64, digest


# ---------------------------------------------------------------------------
# 2. Asset extraction (the decomp's own scripts)
# ---------------------------------------------------------------------------

def local_tool(*parts):
    """Path to a tool under .tools/ if it exists, else None."""
    p = TOOLS.joinpath(*parts)
    return str(p) if p.is_file() else None


def find_bash():
    """bash for the decomp's shell scripts. On Windows, prefer devkitPro's
    MSYS2 (it also provides make, dd and gzip), then Git for Windows."""
    if os.name != "nt":
        return shutil.which("bash")
    candidates = []
    dkp = devkitpro_dir(required=False)
    if dkp:
        candidates.append(dkp / "msys2" / "usr" / "bin" / "bash.exe")
    candidates += [Path(r"C:\devkitPro\msys2\usr\bin\bash.exe"),
                   Path(r"C:\Program Files\Git\bin\bash.exe")]
    for c in candidates:
        if c.is_file():
            return str(c)
    # Plain `bash` on Windows is often the WSL launcher, which can't see
    # Windows paths the same way, so it is deliberately not used.
    return None


def python3_shim_env(tmpdir):
    """The decomp scripts call `python3`. On Windows that name is often
    missing or is the Microsoft Store stub, so put a `python3` wrapper for
    the current interpreter first on PATH."""
    env = os.environ.copy()
    # w64devkit (from fetch_toolchain.py) supplies the gcc and make the
    # extractor needs to build tools/extractor.
    w64 = TOOLS / "w64devkit" / "bin"
    if w64.is_dir():
        env["PATH"] = str(w64) + os.pathsep + env.get("PATH", "")
    if os.name != "nt" and shutil.which("python3"):
        return env
    exe = sys.executable.replace("\\", "/")
    shim = Path(tmpdir) / "python3"
    shim.write_text(f'#!/bin/sh\nexec "{exe}" "$@"\n', newline="\n")
    shim.chmod(0o755)
    env["PATH"] = str(tmpdir) + os.pathsep + env.get("PATH", "")
    return env


def check_decomp(decomp, fetch):
    if not (decomp / "src").is_dir():
        if not fetch:
            raise BuildError(
                f"decomp not found at {decomp}\n"
                f"    Get it with:  git submodule add {DECOMP_URL} external/goldeneye\n"
                "    or rerun with --fetch-decomp to clone it.")
        step("Cloning decomp")
        decomp.parent.mkdir(parents=True, exist_ok=True)
        # autocrlf=false: CRLF line endings break the decomp's bash scripts.
        run(["git", "clone", "--config", "core.autocrlf=false", DECOMP_URL, decomp])
        if DECOMP_BASE.is_file():
            base = DECOMP_BASE.read_text().strip()
            run(["git", "-C", decomp, "checkout", "--quiet", base])

    script = decomp / "scripts" / "extract_baserom.u.sh"
    if not script.is_file():
        raise BuildError(f"{script} missing; is {decomp} really the GoldenEye decomp?")
    if b"\r\n" in script.read_bytes():
        raise BuildError(
            "the decomp was checked out with Windows (CRLF) line endings, which "
            "breaks its shell scripts. Fix with:\n"
            f"    cd {decomp}\n"
            "    git config core.autocrlf false\n"
            "    git rm -rq --cached . && git reset -q --hard")


def git_ok(decomp, *args):
    return subprocess.run(["git", "-C", str(decomp), *args],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def apply_decomp_patch(decomp):
    """Apply patches/goldeneye-3ds.patch to the decomp, once."""
    step("Patching decomp")
    if not DECOMP_PATCH.is_file():
        raise BuildError(f"{DECOMP_PATCH} missing")
    patch = str(DECOMP_PATCH)
    if git_ok(decomp, "apply", "--reverse", "--check", patch):
        info("port patch already applied")
        return
    if not git_ok(decomp, "apply", "--check", patch):
        base = DECOMP_BASE.read_text().strip() if DECOMP_BASE.is_file() else "?"
        raise BuildError(
            "the port patch doesn't apply to this decomp checkout. It was made\n"
            f"    against commit {base}. Either check that out:\n"
            f"      git -C {decomp} checkout {base}\n"
            "    or delete the checkout and rerun with --fetch-decomp.")
    run(["git", "-C", decomp, "apply", "--whitespace=nowarn", patch])
    info("port patch applied")


def extract_assets(z64, digest, decomp, force):
    step("Extracting assets")
    stamp = decomp / EXTRACT_STAMP
    if not force and stamp.is_file() and stamp.read_text().strip() == digest:
        info("already extracted from this ROM (use --force-extract to redo)")
        return

    # The extractor reads baserom.u.z64 from the decomp root.
    baserom = decomp / "baserom.u.z64"
    if not (baserom.is_file() and hashlib.sha1(baserom.read_bytes()).hexdigest() == digest):
        baserom.write_bytes(z64)
        info(f"wrote {baserom}")

    bash = find_bash()
    if not bash:
        raise BuildError(
            "bash not found. On Windows install devkitPro (its MSYS2 provides "
            "bash, make, dd and gzip) or Git for Windows.")

    if stamp.exists():
        stamp.unlink()   # a failed run must not leave a stale "done" marker
    with tempfile.TemporaryDirectory() as tmp:
        env = python3_shim_env(tmp)
        run([bash, "scripts/extract_baserom.u.sh"], cwd=decomp, env=env)

    if not (decomp / "assets" / "obseg").is_dir():
        raise BuildError("the extractor finished but assets/obseg is missing; "
                         "check its output above")
    stamp.write_text(digest + "\n")
    info(f"assets staged in {decomp / 'assets'}")


# ---------------------------------------------------------------------------
# 3. Cross-compilation
# ---------------------------------------------------------------------------

def devkitpro_dir(required=True):
    """Resolve $DEVKITPRO to a real directory. On Windows the installer sets
    it to the MSYS-style /opt/devkitpro, which native programs can't open."""
    candidates = [TOOLS / "devkitpro"]
    env = os.environ.get("DEVKITPRO")
    if env:
        candidates.append(Path(env))
        if os.name == "nt" and env.replace("\\", "/").startswith("/opt/devkitpro"):
            candidates.append(Path(r"C:\devkitPro"))
    candidates += [Path("/opt/devkitpro"), Path(r"C:\devkitPro")]
    for c in candidates:
        if (c / "cmake" / "3DS.cmake").is_file():
            return c
    if required:
        raise BuildError(
            "devkitPro not found (looked for cmake/3DS.cmake under "
            + ", ".join(str(c) for c in candidates) + ").\n"
            "    Install devkitPro with the 3ds-dev group: https://devkitpro.org/wiki/Getting_Started")
    return None


def pick_generator(override):
    if override:
        return override, None
    ninja = local_tool("ninja", "ninja.exe") or shutil.which("ninja")
    if ninja:
        return "Ninja", ninja
    make = shutil.which("make")
    if not make and os.name == "nt":
        dkp = devkitpro_dir(required=False)
        cand = dkp / "msys2" / "usr" / "bin" / "make.exe" if dkp else None
        if cand and cand.is_file():
            make = str(cand)
    if not make:
        raise BuildError("neither ninja nor make found; install one or pass --generator")
    # Never let CMake fall back to Visual Studio on Windows: it can't drive
    # a GCC cross-toolchain.
    return "Unix Makefiles", make


def space_free(path):
    """devkitPro's CMake files splice paths into flag strings unquoted
    (`-L${CTR_ROOT}/lib`), so a devkitPro path with a space breaks the link.
    On Windows, give it a space-free alias: a directory junction at the root
    of the same drive. A junction copies nothing and needs no admin rights."""
    if " " not in str(path) or os.name != "nt":
        return path
    alias = Path(path.anchor) / "ge007-devkitpro"
    if alias.exists():
        if alias.resolve() != path.resolve():
            raise BuildError(f"{alias} exists but doesn't point to {path}; "
                             "remove it or move the toolchain to a path without spaces")
        return alias
    info(f"devkitPro path has a space; linking {alias} -> {path}")
    subprocess.run(["cmd", "/c", "mklink", "/J", str(alias), str(path)],
                   check=True, stdout=subprocess.DEVNULL)
    return alias


def toolchain_file(dkp, cmake):
    """devkitPro's 3DS.cmake refuses a CMake that isn't MSYS2's own. A
    native Windows CMake (e.g. the one fetch_toolchain.py downloads) gets
    our equivalent entry point, which reuses devkitPro's platform files."""
    native_windows = os.name == "nt" and "msys" not in Path(cmake).as_posix().lower()
    if native_windows:
        return ROOT / "cmake" / "3DS-native-windows.cmake"
    return dkp / "cmake" / "3DS.cmake"


def build(args, decomp):
    step("Building")
    cmake = local_tool("cmake", "bin", "cmake.exe") or shutil.which("cmake")
    if not cmake:
        raise BuildError("cmake not found (need 3.13 or newer). On Windows, "
                         "python tools/fetch_toolchain.py downloads one.")

    dkp = space_free(devkitpro_dir())
    env = os.environ.copy()
    env["DEVKITPRO"] = dkp.as_posix()
    env["DEVKITARM"] = (dkp / "devkitARM").as_posix()

    generator, make = pick_generator(args.generator)
    build_dir = args.build_dir

    if args.clean and build_dir.exists():
        info(f"removing {build_dir}")
        shutil.rmtree(build_dir)

    configure = [
        cmake, "-S", ROOT, "-B", build_dir, "-G", generator,
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file(dkp, cmake).as_posix()}",
        f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DGE_INTEGRATE={'OFF' if args.harness else 'ON'}",
    ]
    if make:
        configure.append(f"-DCMAKE_MAKE_PROGRAM={Path(make).as_posix()}")
    if not args.harness:
        configure += [f"-DGE_DECOMP_DIR={decomp.as_posix()}",
                      f"-DGE_BOOT_ENTRY={args.boot_entry}"]
    if args.stream_rom:
        configure.append("-DOS_PORT_ROM_MODE=PAL_ROM_STREAM")

    # Configure every run: it's quick on an existing build directory, and
    # it picks up changed options. Switching generators needs --clean.
    run(configure, env=env)
    run([cmake, "--build", build_dir, "--parallel", str(args.jobs)], env=env)

    out = build_dir / "ge007.3dsx"
    if not out.is_file():
        raise BuildError(f"build finished but {out} was not produced")
    # 3dsxtool accepts relocations that every loader rejects; catch them here.
    check = subprocess.run([sys.executable, str(ROOT / "tools" / "check_3dsx.py"),
                            str(out), str(build_dir / "ge007.elf")])
    if check.returncode != 0:
        raise BuildError("the .3dsx has invalid relocations (see above); it would not load")
    info(f"built {out} ({out.stat().st_size // 1024} KB)")
    return out


# ---------------------------------------------------------------------------
# 4. Packaging
# ---------------------------------------------------------------------------

SD_README = """\
GoldenEye 007 for Nintendo 3DS (homebrew port)
===============================================

Copy the "3ds" folder from here to the root of your SD card, merging it
with the "3ds" folder already there. You should end up with:

    sdmc:/3ds/ge007/ge007.3dsx
    sdmc:/3ds/ge007/rom.z64

Start it from the Homebrew Launcher.

rom.z64 is a copy of your own cartridge dump. Don't share this folder.
"""


def package(z64, args, built):
    step("Packaging")
    out = args.dist_dir / SD_DIR
    # Empty the folder rather than delete it: on Windows a folder that is
    # some process's current directory can't be removed.
    out.mkdir(parents=True, exist_ok=True)
    for p in out.iterdir():
        shutil.rmtree(p) if p.is_dir() else p.unlink()

    # The icon, name and description are inside the .3dsx (its embedded
    # SMDH, from res/icon.png). The Homebrew Launcher needs no separate
    # metadata file.
    shutil.copy2(built, out / "ge007.3dsx")

    if args.no_rom:
        info(f"--no-rom: copy your ROM to sdmc:/{SD_DIR.as_posix()}/{SD_ROM_NAME} yourself")
    elif z64 is not None:
        # The port reads game data from the ROM at runtime (os_pi.c), so the
        # verified, byte-order-normalised ROM is the asset package.
        (out / SD_ROM_NAME).write_bytes(z64)

    (out / "README.txt").write_text(SD_README, newline="\r\n")

    info(f"SD card layout ready in {args.dist_dir}:")
    for p in sorted(out.rglob("*")):
        info(f"  {p.relative_to(args.dist_dir).as_posix():40} {p.stat().st_size // 1024:>8} KB")


# ---------------------------------------------------------------------------

def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Build the GoldenEye 007 3DS port from your own ROM.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="example:\n  python builder.py --rom baserom.us.z64")
    p.add_argument("--rom", type=Path, help="your NTSC-U GoldenEye ROM (.z64, .v64 or .n64)")
    p.add_argument("--decomp-dir", type=Path, default=DEFAULT_DECOMP,
                   help="GoldenEye decomp checkout (default: external/goldeneye)")
    p.add_argument("--fetch-decomp", action="store_true",
                   help=f"git clone {DECOMP_URL} if the decomp is missing")
    p.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    p.add_argument("--dist-dir", type=Path, default=DEFAULT_DIST)
    p.add_argument("--config", default="Release", choices=["Release", "Debug", "RelWithDebInfo"])
    p.add_argument("--generator", help="CMake generator (default: Ninja, else Unix Makefiles)")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    p.add_argument("--boot-entry", default="init",
                   help="decomp function main.c calls to boot the game (default: init)")
    p.add_argument("--stream-rom", action="store_true",
                   help="read the ROM from SD on every DMA instead of loading it into RAM")
    p.add_argument("--harness", action="store_true",
                   help="build only the input-test app (no decomp, no ROM needed)")
    p.add_argument("--clean", action="store_true", help="delete the build directory first")
    p.add_argument("--force-extract", action="store_true", help="re-run asset extraction")
    p.add_argument("--skip-extract", action="store_true")
    p.add_argument("--skip-build", action="store_true")
    p.add_argument("--no-rom", action="store_true", help="don't copy the ROM into dist/")
    p.add_argument("--only", choices=["verify", "extract", "build", "package"],
                   help="run just one step")
    args = p.parse_args(argv)

    args.decomp_dir = args.decomp_dir.resolve()
    args.build_dir = args.build_dir.resolve()
    args.dist_dir = args.dist_dir.resolve()

    needs_rom = not args.harness and args.only != "build"
    if needs_rom and args.rom is None:
        p.error("--rom is required (or use --harness to build the input test only)")
    return args


def main(argv=None):
    args = parse_args(argv)
    want = (lambda s: args.only == s) if args.only else (lambda s: True)

    z64 = digest = None
    if args.rom is not None and (want("verify") or want("extract") or want("package")):
        z64, digest = verify_rom(args.rom.resolve(), args.decomp_dir)
    if args.only == "verify":
        return

    if not args.harness and want("extract") and not args.skip_extract:
        check_decomp(args.decomp_dir, args.fetch_decomp)
        extract_assets(z64, digest, args.decomp_dir, args.force_extract)

    built = args.build_dir / "ge007.3dsx"
    if want("build") and not args.skip_build:
        if not args.harness:
            check_decomp(args.decomp_dir, args.fetch_decomp)
            apply_decomp_patch(args.decomp_dir)
        built = build(args, args.decomp_dir)

    if want("package"):
        if not built.is_file():
            raise BuildError(f"{built} doesn't exist; run the build step first")
        package(z64, args, built)

    print("\nDone.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except BuildError as e:
        print(f"\nerror: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
