#!/usr/bin/env python3
"""
run_azahar.py - run the built game in the Azahar 3DS emulator and show
what it logged.

    python tools/run_azahar.py [--seconds 45] [--all] [--shots 10,30 [--shot-dir DIR]]
                               [--keys 8:m,12:a]

Uses the portable Azahar in .tools/azahar (python tools/fetch_toolchain.py
--with-emulator), copies dist/3ds/ge007/ onto its virtual SD card, runs
the game for a while, then prints:
  * the port's own log lines (pal_log -> svcOutputDebugString), and
  * the first emulator-side faults (unmapped memory accesses, CPU
    exceptions), each mapped to a function and source line when
    build/ge007.elf is present.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AZAHAR = ROOT / ".tools" / "azahar"            # the one you play with (your settings)
AZAHAR_AUTO = ROOT / ".tools" / "azahar-auto"  # a copy for scripted runs: default keys
DIST = ROOT / "dist" / "3ds" / "ge007"
ELF = ROOT / "build" / "ge007.elf"
ADDR2LINE = ROOT / ".tools/devkitpro/devkitARM/bin/arm-none-eabi-addr2line.exe"

# Show Debug.Emulated (svcOutputDebugString); keep everything else at Info.
CONFIG = ("[Miscellaneous]\nlog_filter\\default=false\n"
          "log_filter=*:Info Debug.Emulated:Trace Service.APT:Warning\n")

FAULT = re.compile(r"(unmapped \w+ (?:0x[0-9A-F]+ )?@ 0x[0-9A-F]+ at PC 0x([0-9A-F]+)|Prefetch|Data abort|"
                   r"Undefined instruction|svcBreak|Failed to load)", re.I)


def where(pcs):
    if not (ELF.is_file() and ADDR2LINE.is_file() and pcs):
        return {}
    out = subprocess.run([str(ADDR2LINE), "-f", "-e", str(ELF)] + pcs,
                         capture_output=True, text=True).stdout.splitlines()
    res = {}
    for i, pc in enumerate(pcs):
        if 2 * i + 1 < len(out):
            src = re.sub(r".*/external/goldeneye/", "", out[2 * i + 1])
            res[pc] = f"{out[2 * i]} ({src})"
    return res


def disable_close_prompt(cfg):
    # Azahar writes it as UTF-8 (paths, key names); keep any other bytes as they are.
    text = cfg.read_text(encoding="utf-8", errors="surrogateescape")
    new = (text.replace(r"confirmClose\default=true", r"confirmClose\default=false")
               .replace("confirmClose=true", "confirmClose=false"))
    if new != text:
        cfg.write_text(new, encoding="utf-8", errors="surrogateescape")


def set_stereo(cfg, slider):
    """Azahar's stereo output: side by side with the 3D slider at slider
    percent, or off (None). Both eyes then show in --shots, left eye left."""
    text = cfg.read_text(encoding="utf-8", errors="surrogateescape")
    values = {"render_3d": 1 if slider else 0, "factor_3d": slider or 0,
              "disable_right_eye_render": 0}
    for key, value in values.items():
        text = re.sub(r"^%s\\default=\w+$" % key, r"%s\\default=false" % key, text, flags=re.M)
        v = ("false" if not value else "true") if key.startswith("disable") else "%d" % value
        text = re.sub(r"^%s=.*$" % key, "%s=%s" % (key, v), text, flags=re.M)
    cfg.write_text(text, encoding="utf-8", errors="surrogateescape")


def write_png(path, w, h, bgra):
    """Top-down 32-bit BGRA rows -> an RGB PNG (no Pillow needed)."""
    rows = bytearray()
    for y in range(h):
        row = bgra[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b"\0" + rgb

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def find_window(pid):
    """pid's largest visible top-level window: (hwnd, width, height), or None."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    best = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            best.append((r.right * r.bottom, hwnd, r.right, r.bottom))
        return True
    user32.EnumWindows(enum, 0)
    if not best:
        return None
    _, hwnd, w, h = max(best)
    return hwnd, w, h


def press_key(pid, key, hold=0.15):
    """Post a key press (a letter, or 'up'/'down'/'left'/'right') to Azahar's
    window: no focus needed. Azahar's default keys: A = A, S = B, M = Start,
    N = Select, T/G/F/H = d-pad, arrows = circle pad."""
    import ctypes
    win = find_window(pid)
    if not win:
        return False
    vk = {"up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27}.get(key.lower(), ord(key.upper()[0]))
    user32 = ctypes.windll.user32
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.PostMessageW(win[0], 0x0100, vk, 1 | (scan << 16))                      # WM_KEYDOWN
    time.sleep(hold)
    user32.PostMessageW(win[0], 0x0101, vk, 1 | (scan << 16) | (3 << 30))          # WM_KEYUP
    return True


def capture_window(pid, path):
    """Save the client area of pid's largest visible window with PrintWindow,
    which (unlike a hotkey) needs neither focus nor the window on top.
    Returns False if there was no window to capture."""
    import ctypes
    from ctypes import wintypes
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    # Without this, display scaling makes GetClientRect report scaled-down
    # sizes and only the top-left part of the window is captured.
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # per-monitor v2
    except (AttributeError, OSError):
        pass
    user32.GetDC.restype = wintypes.HDC
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    for f, args in ((gdi32.CreateCompatibleDC, [wintypes.HDC]),
                    (gdi32.CreateCompatibleBitmap, [wintypes.HDC, ctypes.c_int, ctypes.c_int]),
                    (gdi32.SelectObject, [wintypes.HDC, wintypes.HGDIOBJ]),
                    (gdi32.DeleteObject, [wintypes.HGDIOBJ]),
                    (gdi32.DeleteDC, [wintypes.HDC]),
                    (user32.ReleaseDC, [wintypes.HWND, wintypes.HDC]),
                    (user32.PrintWindow, [wintypes.HWND, wintypes.HDC, wintypes.UINT]),
                    (gdi32.GetDIBits, [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                                       ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT])):
        f.argtypes = args
    win = find_window(pid)
    if not win:
        return False
    hwnd, w, h = win
    if w <= 0 or h <= 0:
        return False
    wdc = user32.GetDC(hwnd)
    dc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, h)
    gdi32.SelectObject(dc, bmp)
    user32.PrintWindow(hwnd, dc, 3)            # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    hdr = ctypes.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0), 44)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(dc, bmp, 0, h, buf, hdr, 0)   # top-down 32-bit BGRA
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(hwnd, wdc)
    write_png(path, w, h, buf.raw)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--all", action="store_true", help="print every fault, not just the first few")
    ap.add_argument("--shots", help="comma-separated seconds at which to capture Azahar's "
                    "window: what the emulator really shows")
    ap.add_argument("--mine", action="store_true",
                    help="use .tools/azahar and its settings (yours) instead of the "
                         "scripted-run copy in .tools/azahar-auto, whose keyboard keys are "
                         "Azahar's defaults")
    ap.add_argument("--stage", type=int, help="start this LEVELID (33 = Dam, 34 = Facility, "
                    "35 = Runway) on Agent at boot, skipping the menus (autostage.txt)")
    ap.add_argument("--gun", type=int, help="start levels holding this ITEM_IDS gun, with "
                    "ammo (13 = AR33, 8 = KF7; autogun.txt)")
    ap.add_argument("--tokens", help="the game's developer token string, e.g. -nochr (no "
                    "guards; tokens.txt)")
    ap.add_argument("--keys", help="comma-separated SECONDS:KEY presses sent to Azahar's "
                    "window, e.g. 8:m,12:a (see press_key for the key names)")
    ap.add_argument("--bench", type=int, help="render graphics task N 120 more times with the "
                    "game held, and log the timing; the game steps one tick a frame (autobench.txt)")
    ap.add_argument("--stems", type=int, help="at boot, render this MUSIC_TRACKS number (7 = "
                    "Facility) to WAVs, one per channel, in the emulated sdmc:/3ds/ge007/stems/")
    ap.add_argument("--stems-seconds", type=int, default=180, help="length of each stem")
    ap.add_argument("--app", help="boot this file instead of the .3dsx (e.g. a .cxi built "
                    "by tools/cia/make_cia.py --cxi, to test the CIA's program and exheader)")
    ap.add_argument("--stereo", type=int, help="show stereo 3D side by side with the 3D "
                    "slider at this percent (scripted copy only; 0 or absent: off)")
    ap.add_argument("--shot-dir", default=str(ROOT / "build" / "shots"),
                    help="where --shots pictures are saved (default build/shots)")
    args = ap.parse_args()

    if not (AZAHAR / "azahar.exe").is_file():
        sys.exit("Azahar not found; run: python tools/fetch_toolchain.py --with-emulator")
    home = AZAHAR if args.mine else AZAHAR_AUTO
    if not args.mine and not (home / "azahar.exe").is_file():
        # Everything but user/: settings, saves and the log stay separate.
        home.mkdir(parents=True, exist_ok=True)
        for f in AZAHAR.iterdir():
            if f.name == "user":
                continue
            if f.is_dir():
                shutil.copytree(f, home / f.name, dirs_exist_ok=True)
            else:
                shutil.copy2(f, home / f.name)
    exe = home / "azahar.exe"
    if not (DIST / "ge007.3dsx").is_file():
        sys.exit("dist/3ds/ge007/ge007.3dsx missing; run builder.py first")

    user = home / "user"             # a user/ folder next to the exe = portable mode
    (user / "config").mkdir(parents=True, exist_ok=True)
    cfg = user / "config" / "qt-config.ini"
    if not cfg.exists():
        cfg.write_text(CONFIG)
    sd = user / "sdmc" / "3ds" / "ge007"
    sd.mkdir(parents=True, exist_ok=True)
    for f in DIST.iterdir():
        if f.is_dir():
            shutil.copytree(f, sd / f.name, dirs_exist_ok=True)   # e.g. crash/
        else:
            shutil.copy2(f, sd / f.name)
    auto = sd / "autostage.txt"
    if args.stage:
        auto.write_text("%d\n" % args.stage)
    elif auto.exists():
        auto.unlink()
    tokens = sd / "tokens.txt"
    if args.tokens:
        tokens.write_text(args.tokens + "\n")
    elif tokens.exists():
        tokens.unlink()
    bench = sd / "autobench.txt"
    if args.bench:
        bench.write_text("%d\n" % args.bench)
    elif bench.exists():
        bench.unlink()
    for name, value in (("stems.txt", args.stems), ("stems_seconds.txt", args.stems_seconds)):
        if args.stems:
            (sd / name).write_text(str(value))
        elif (sd / name).exists():
            (sd / name).unlink()
    gun = sd / "autogun.txt"
    if args.gun:
        gun.write_text("%d\n" % args.gun)
    elif gun.exists():
        gun.unlink()

    # No "stop emulation?" prompt, so a polite close works unattended. Qt
    # ignores the value while its "\default" flag is true, so clear both.
    disable_close_prompt(cfg)
    if not args.mine:
        set_stereo(cfg, args.stereo)

    proc = subprocess.Popen([str(exe), args.app or str(sd / "ge007.3dsx")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    events = [(float(t), "shot", None) for t in args.shots.split(",")] if args.shots else []
    for item in (args.keys.split(",") if args.keys else []):
        t, key = item.split(":", 1)
        events.append((float(t), "key", key))   # KEY or KEY/HOLDSECONDS
    events.sort(key=lambda e: e[0])
    out = Path(args.shot_dir)
    start = time.time()
    for t, kind, key in events:
        if t >= args.seconds:
            break
        try:
            proc.wait(timeout=max(0.0, start + t - time.time()))
            break                                       # it exited early
        except subprocess.TimeoutExpired:
            pass
        if kind == "key":
            name, _, hold = key.partition("/")
            # On a thread: a long hold must not hold up later shots and keys.
            def press(name=name, hold=hold, key=key, t=t):
                if not press_key(proc.pid, name, float(hold) if hold else 0.15):
                    print(f"key {key} at {t:g}s: no Azahar window")
            threading.Thread(target=press, daemon=True).start()
            continue
        out.mkdir(parents=True, exist_ok=True)
        name = out / f"shot_{t:g}s.png"
        if capture_window(proc.pid, name):
            print("screenshot:", name)
        else:
            print(f"screenshot at {t:g}s: no Azahar window to capture")
    try:
        proc.wait(timeout=max(0.0, start + args.seconds - time.time()))
    except subprocess.TimeoutExpired:
        # The normal way out: the game runs until we stop it. Close the
        # window rather than kill the process, so Azahar flushes its log
        # (debug lines are buffered and lost on a hard kill).
        subprocess.run(["taskkill", "/PID", str(proc.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()

    log = (user / "log" / "azahar_log.txt").read_text(errors="replace").splitlines()
    print("---- port log ----")
    for line in log:
        if "OutputDebugString" in line:
            print("  " + line.split("OutputDebugString:", 1)[1].split(": ", 1)[-1])

    faults = [l for l in log if FAULT.search(l)]
    if not faults:
        print("---- no faults logged ----")
        return
    shown = faults if args.all else faults[:8]
    pcs = sorted({m.group(2) for l in shown if (m := FAULT.search(l)) and m.group(2)})
    loc = where(["0x" + pc for pc in pcs])
    print(f"---- faults ({len(faults)} total) ----")
    for l in shown:
        m = FAULT.search(l)
        extra = loc.get("0x" + m.group(2), "") if m.group(2) else ""
        print("  " + l.split(": ", 1)[-1][:110] + ("  <- " + extra if extra else ""))


if __name__ == "__main__":
    main()
