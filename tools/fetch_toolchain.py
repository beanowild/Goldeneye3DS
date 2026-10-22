#!/usr/bin/env python3
"""
fetch_toolchain.py - download a self-contained 3DS toolchain into .tools/.

    python tools/fetch_toolchain.py            # everything
    python tools/fetch_toolchain.py --list     # show what would be fetched

Fetches, into <repo>/.tools/:
  devkitpro/   devkitARM, libctru, citro3d, citro2d, 3DS CMake files and
               tools (3dsxtool, smdhtool, picasso, tex3ds), unpacked from
               devkitPro's pacman repository without needing MSYS2/pacman.
               Package SHA-256 sums are checked against the repo database.
  cmake/       a pinned CMake release
  ninja/       a pinned Ninja release
  w64devkit/   (Windows) a host GCC + make/sh/dd/gzip, for host unit tests
               and the decomp's asset extractor

builder.py looks in .tools/ automatically, so after this a plain
`python builder.py --rom ...` works with nothing installed system-wide.

Windows x86-64 only for now: on Linux/macOS install devkitPro with its
official pacman (https://devkitpro.org/wiki/Getting_Started).

Needs the `zstandard` module (pip install zstandard): devkitPro packages
are .pkg.tar.zst and Python's standard library can't read zstd before 3.14.
"""

import argparse
import hashlib
import io
import lzma
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = ROOT / ".tools"

# pkg.devkitpro.org sits behind Cloudflare, which rejects Python's default
# User-Agent.
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ge007-3ds-fetch-toolchain"

DKP_REPOS = {
    "dkp-libs":    "https://pkg.devkitpro.org/packages/",
    "dkp-windows": "https://pkg.devkitpro.org/packages/windows/x86_64/",
}

# What we need from the 3ds-dev group. Left out: examples, gdb, catnip, and
# devkit-env (an MSYS2 profile script). Dependencies are added automatically.
DKP_WANTED = [
    "devkitARM", "libctru", "citro3d", "citro2d", "3ds-cmake",
    "3dstools", "picasso", "tex3ds", "3dslink", "general-tools",
]

# Host tools: pinned releases (all older than two weeks), with SHA-256 so a
# swapped download is caught. Each archive unpacks to one top-level folder,
# which is renamed to `dest`.
HOST_TOOLS = [
    {
        "name": "cmake",
        "url": "https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip",
        "sha256": "5ab6e1faf20256ee4f04886597e8b6c3b1bd1297b58a68a58511af013710004b",
        "dest": "cmake",
    },
    {
        "name": "ninja",
        "url": "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip",
        "sha256": "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a",
        "dest": "ninja",
        "flat": True,     # archive holds ninja.exe at its root
    },
    {
        # Optional (--with-emulator): runs the build for testing, see
        # tools/run_azahar.py. Latest stable release more than two weeks old.
        "name": "azahar",
        "url": "https://github.com/azahar-emu/azahar/releases/download/2126.1.2/azahar-windows-msvc-2126.1.2.zip",
        "sha256": "bbff556d1736301debdec2872a5e9f3678c52e84261aabaa4f708e55db9ae139",
        "dest": "azahar",
        "optional": True,
    },
    {
        "name": "w64devkit",
        "url": "https://github.com/skeeto/w64devkit/releases/download/v1.23.0/w64devkit-1.23.0.zip",
        "sha256": "5c7dce6762be3e0dba648a9317790444c0e2f1ef3e677315c115727d7a549539",
        "dest": "w64devkit",
    },
]


def log(msg):
    print(msg, flush=True)


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


# ---------------------------------------------------------------------------
# devkitPro packages
# ---------------------------------------------------------------------------

def parse_db(blob, repo):
    """pacman .db: a gzipped tar of <pkg>/desc files with %KEY% sections."""
    pkgs = {}
    with tarfile.open(fileobj=io.BytesIO(blob)) as t:
        for m in t.getmembers():
            if not m.name.endswith("/desc"):
                continue
            d, key = {}, None
            for line in t.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    d[key] = []
                elif line and key:
                    d[key].append(line)
            d["_repo"] = repo
            pkgs[d["NAME"][0]] = d
            for prov in d.get("PROVIDES", []):
                pkgs.setdefault(dep_name(prov), d)
    return pkgs


def dep_name(dep):
    for op in (">=", "<=", "=", ">", "<"):
        dep = dep.split(op)[0]
    return dep.strip()


def resolve(pkgs, wanted):
    order, seen = [], set()

    def visit(name):
        d = pkgs.get(name)
        if d is None:
            # MSYS2 system packages (bash, etc.): not needed outside MSYS2.
            log(f"  (skipping dependency not in devkitPro repos: {name})")
            return
        real = d["NAME"][0]
        if real in seen:
            return
        seen.add(real)
        for dep in d.get("DEPENDS", []):
            visit(dep_name(dep))
        order.append(d)

    for w in wanted:
        visit(w)
    return order


def decompress(name, blob):
    if name.endswith(".zst"):
        try:
            import zstandard
        except ImportError:
            sys.exit("error: devkitPro packages are zstd-compressed; "
                     "run `pip install zstandard` first")
        return zstandard.ZstdDecompressor().stream_reader(io.BytesIO(blob)).read()
    if name.endswith(".xz"):
        return lzma.decompress(blob)
    raise ValueError(f"unknown package compression: {name}")


def install_pkg(d, dest):
    fname = d["FILENAME"][0]
    url = DKP_REPOS[d["_repo"]] + fname
    blob = fetch(url)

    want = d.get("SHA256SUM", [None])[0]
    got = hashlib.sha256(blob).hexdigest()
    if want and got != want:
        sys.exit(f"error: checksum mismatch for {fname}\n  expected {want}\n  got      {got}")

    tar_bytes = decompress(fname, blob)
    n = 0
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
        for m in t.getmembers():
            # Packages install under /opt/devkitpro inside MSYS2; keep the
            # part below that. Skip pacman metadata (.PKGINFO, .MTREE, ...).
            name = m.name.rstrip("/")
            if name.startswith(".") or name in ("opt", "opt/devkitpro"):
                continue
            if name.startswith("opt/devkitpro/"):
                name = name[len("opt/devkitpro/"):]
            if not name or name.startswith("/") or ".." in Path(name).parts:
                continue
            target = dest / name
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
                n += 1
            elif m.issym() or m.islnk():
                # Windows can't always make symlinks; copy the target file
                # once everything else is unpacked (see fix_links).
                link_list.append((target, m.linkname, m.issym()))
    log(f"  {d['NAME'][0]:22} {d['VERSION'][0]:18} {len(blob) // 1024:>7} KB  {n} files")


link_list = []


def fix_links(dest):
    for target, linkname, is_sym in link_list:
        src = (target.parent / linkname) if is_sym else (dest / linkname.replace("opt/devkitpro/", "", 1))
        if src.is_file() and not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)


def fetch_devkitpro(dest, list_only):
    log("devkitPro: reading package databases")
    pkgs = {}
    for repo, base in DKP_REPOS.items():
        pkgs.update(parse_db(fetch(base + repo + ".db"), repo))
    order = resolve(pkgs, DKP_WANTED)

    if list_only:
        for d in order:
            log(f"  {d['NAME'][0]:22} {d['VERSION'][0]:18} {d['FILENAME'][0]}")
        return

    dkp = dest / "devkitpro"
    dkp.mkdir(parents=True, exist_ok=True)
    log(f"devkitPro: installing {len(order)} packages into {dkp}")
    for d in order:
        install_pkg(d, dkp)
    fix_links(dkp)


# ---------------------------------------------------------------------------
# Host tools
# ---------------------------------------------------------------------------

def fetch_host_tool(tool, dest, list_only):
    if list_only:
        log(f"  {tool['name']:22} {tool['url']}")
        return
    out = dest / tool["dest"]
    if out.exists():
        log(f"  {tool['name']:22} already present")
        return

    blob = fetch(tool["url"])
    digest = hashlib.sha256(blob).hexdigest()
    if tool["sha256"] and digest != tool["sha256"]:
        sys.exit(f"error: checksum mismatch for {tool['url']}")

    tmp = dest / (tool["dest"] + ".partial")
    if tmp.exists():
        shutil.rmtree(tmp)
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        z.extractall(tmp)
    if tool.get("flat"):
        tmp.rename(out)
    else:
        (top,) = [p for p in tmp.iterdir()]
        top.rename(out)
        tmp.rmdir()
    log(f"  {tool['name']:22} {len(blob) // 1024:>7} KB  sha256 {digest[:16]}...")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    p.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    p.add_argument("--list", action="store_true", help="show what would be fetched")
    p.add_argument("--skip-devkitpro", action="store_true")
    p.add_argument("--skip-host-tools", action="store_true")
    p.add_argument("--with-emulator", action="store_true",
                   help="also fetch the Azahar 3DS emulator (for tools/run_azahar.py)")
    args = p.parse_args()

    if os.name != "nt" and not args.list:
        sys.exit("Only Windows is handled here. On Linux/macOS install devkitPro "
                 "with its pacman: https://devkitpro.org/wiki/Getting_Started")

    args.dest.mkdir(parents=True, exist_ok=True)
    if not args.skip_devkitpro:
        fetch_devkitpro(args.dest, args.list)
    if not args.skip_host_tools:
        log("host tools:")
        for tool in HOST_TOOLS:
            if tool.get("optional") and not args.with_emulator:
                continue
            fetch_host_tool(tool, args.dest, args.list)
    log("done")


if __name__ == "__main__":
    main()
