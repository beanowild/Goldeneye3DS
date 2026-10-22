#!/usr/bin/env python3
"""
azahar_threads.py - run the game in Azahar, then stop it after a while and
print where every thread is (PC and LR, mapped to functions), using
Azahar's GDB stub. For hangs that don't fault, memory corruption
(--watch), and looking at the renderer's output (--screenshot).

    python tools/azahar_threads.py [--seconds 30] [--watch ADDR] [--stack ID]
                                   [--screenshot out.png] [--profile 400]

--profile N samples every thread's PC N times (stop, read, continue, a
few tens of ms apart) and prints where each thread spends its time. Start
a level first with run_azahar.py --stage (its autostage.txt stays put).

Needs no gdb: speaks the GDB remote protocol directly. Uses the same
portable Azahar and SD card layout as run_azahar.py (run that once first).
"""

import argparse
import re
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_azahar as ra  # noqa: E402

PORT = 24689


class Gdb:
    def __init__(self, port):
        deadline = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.buf = b""

    def _read_packet(self):
        while True:
            while b"#" not in self.buf or len(self.buf) < self.buf.index(b"#") + 3:
                self.buf += self.s.recv(65536)
            start = self.buf.find(b"$")
            if start < 0:
                self.buf = b""
                continue
            end = self.buf.index(b"#", start)
            body = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return body.decode(errors="replace")

    def cmd(self, body, reply=True):
        csum = sum(body.encode()) & 0xFF
        self.s.sendall(b"$%s#%02x" % (body.encode(), csum))
        return self._read_packet() if reply else None

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self._read_packet()


def regs(gdb):
    raw = gdb.cmd("g")
    return [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little") for i in range(0, 16 * 8, 8)]


def read_mem(gdb, addr, size):
    out = bytearray()
    while len(out) < size:
        n = min(0x800, size - len(out))
        out += bytes.fromhex(gdb.cmd("m%x,%x" % (addr + len(out), n)))
    return bytes(out)


def write_png(path, width, height, pixels):
    """pixels: rows of (r, g, b) tuples."""
    rows = b"".join(b"\0" + bytes(c for p in row for c in p) for row in pixels)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def screenshot(gdb, path):
    """Decode the PICA colour buffer: 240x400 (portrait), RGBA8 stored as
    A B G R bytes, 8x8 tiles in Morton order, rows bottom-up. Written out
    rotated to the 400x240 picture the top screen shows."""
    nm = ra.ADDR2LINE.with_name("arm-none-eabi-nm.exe")
    syms = subprocess.run([str(nm), str(ra.ELF)], capture_output=True, text=True).stdout
    m = re.search(r"^([0-9a-f]+) \w gfx_debug_colorbuf$", syms, re.M)
    if not m:
        print("screenshot: gfx_debug_colorbuf not in the ELF")
        return
    addr = int.from_bytes(read_mem(gdb, int(m.group(1), 16), 4), "little")
    if addr == 0:
        print("screenshot: the renderer hasn't started yet")
        return
    W, H = 240, 400
    raw = read_mem(gdb, addr, W * H * 4)
    nonzero = sum(1 for b in raw if b)
    print("screenshot: buffer at %08x, %d of %d bytes nonzero, alpha bytes 0xff: %d"
          % (addr, nonzero, len(raw), sum(1 for b in raw[0::4] if b == 0xFF)))

    def morton(x, y):
        o = 0
        for i in range(3):
            o |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
        return o

    img = [[(0, 0, 0)] * 400 for _ in range(240)]
    for ty in range(H // 8):
        for tx in range(W // 8):
            base = (ty * (W // 8) + tx) * 64 * 4
            for py in range(8):
                for px in range(8):
                    o = base + morton(px, py) * 4
                    _, b_, g_, r_ = raw[o:o + 4]
                    fx, fy = tx * 8 + px, H - 1 - (ty * 8 + py)   # buffer coords, y down
                    # buffer x runs top to bottom of the screen, y right to left
                    img[fx][399 - fy] = (r_, g_, b_)
    write_png(path, 400, 240, img)
    print("screenshot:", path)


def thread_ids(gdb):
    threads = []
    r = gdb.cmd("qfThreadInfo")
    while r.startswith("m"):
        threads += r[1:].split(",")
        r = gdb.cmd("qsThreadInfo")
    return threads


def profile(gdb, n, top, lines=0):
    """Sample each thread's PC n times; print the busiest functions."""
    import collections
    import random
    samples = collections.defaultdict(list)       # thread -> [pc]
    callers = collections.defaultdict(list)       # thread -> [lr]: who called a leaf
    for i in range(n):
        for t in thread_ids(gdb):
            gdb.cmd("Hg" + t)
            r = regs(gdb)
            samples[t].append(r[15])
            callers[t].append(r[14] & ~1)
        gdb.cmd("c", reply=False)
        time.sleep(random.uniform(0.02, 0.06))
        gdb.interrupt()
    pcs = sorted({"0x%x" % pc for v in list(samples.values()) + list(callers.values()) for pc in v})
    loc = {}
    for i in range(0, len(pcs), 800):             # Windows command-line limit
        loc.update(ra.where(pcs[i:i + 800]))
    for t, v in samples.items():
        funcs = collections.Counter(loc.get("0x%x" % pc, "?").split()[0] if loc.get("0x%x" % pc)
                                    else "?" for pc in v)
        print("thread %s: %d samples" % (t, len(v)))
        for name, c in funcs.most_common(top):
            if c * 100 < len(v):
                break
            print("   %5.1f%%  %s" % (100.0 * c / len(v), name))
        # library leaves (memcpy, fminf, ...): their callers, from lr
        leaf = collections.Counter()
        for pc, lr in zip(v, callers[t]):
            f = (loc.get("0x%x" % pc) or "?").split()[0]
            if f in ("memcpy", "memset", "memcmp", "fminf", "fmaxf", "__fpclassifyf", "roundf", "floorf",
                     "ceilf") or f.startswith("svc"):
                who = (loc.get("0x%x" % lr) or "?").split()[0]
                leaf[f + " <- " + (who if who != "??" else "?? lr 0x%x" % lr)] += 1
        for name, c in leaf.most_common(12):
            if c * 200 < len(v):
                break
            print("   %5.1f%%  %s" % (100.0 * c / len(v), name))
        # --lines: the busiest source lines of a thread that is mostly running
        busy = sum(1 for pc in v if not (loc.get("0x%x" % pc) or "?").startswith("svc"))
        if lines and busy * 2 > len(v):
            by_line = collections.Counter((loc.get("0x%x" % pc) or "?").replace("M:/Goldeneye 3ds/", "")
                                          for pc in v)
            print("   busiest lines:")
            for name, c in by_line.most_common(lines):
                print("   %5.1f%%  %s" % (100.0 * c / len(v), name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--watch", help="hex address: after --seconds, stop on the next write to it")
    ap.add_argument("--watch-timeout", type=int, default=120)
    ap.add_argument("--watch-value", help="hex: keep going past writes until the watched "
                    "word holds this value (e.g. 0 for a return address being zeroed)")
    ap.add_argument("--stack", help="thread id (as printed) whose stack to scan for return addresses")
    ap.add_argument("--screenshot", help="PNG to write the renderer's colour buffer to")
    ap.add_argument("--profile", type=int, help="sample every thread's PC this many times")
    ap.add_argument("--top", type=int, default=25, help="functions listed per thread (--profile)")
    ap.add_argument("--lines", type=int, default=0,
                    help="also list this many busiest source lines of a busy thread (--profile)")
    args = ap.parse_args()

    home = ra.AZAHAR_AUTO if (ra.AZAHAR_AUTO / "azahar.exe").is_file() else ra.AZAHAR
    cfg = home / "user" / "config" / "qt-config.ini"
    text = cfg.read_text(encoding="utf-8", errors="surrogateescape")
    on = (text.replace("use_gdbstub=false", "use_gdbstub=true")
              .replace(r"use_gdbstub\default=true", r"use_gdbstub\default=false"))
    if args.screenshot:
        # The hardware renderers keep what they draw on the host GPU and
        # don't write it back to emulated VRAM, which is what we read. The
        # software renderer draws into VRAM itself.
        on = re.sub(r"(?m)^graphics_api\\default=.*$", r"graphics_api\\default=false", on)
        on = re.sub(r"(?m)^graphics_api=.*$", "graphics_api=0", on)
    cfg.write_text(on, encoding="utf-8", errors="surrogateescape")
    proc = None
    try:
        proc = subprocess.Popen([str(home / "azahar.exe"),
                                 str(home / "user/sdmc/3ds/ge007/ge007.3dsx")],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        gdb = Gdb(PORT)
        gdb.cmd("?")
        gdb.cmd("c", reply=False)          # the stub halts at start
        time.sleep(args.seconds)
        gdb.interrupt()
        if args.watch:
            print("watch:", gdb.cmd("Z2,%s,4" % args.watch))
            gdb.cmd("c", reply=False)
            gdb.s.settimeout(args.watch_timeout)
            want = int(args.watch_value, 16) if args.watch_value else None
            hits = 0
            try:
                while True:
                    stop = gdb._read_packet()
                    hits += 1
                    if want is None:
                        print("stopped:", stop)
                        break
                    gdb.s.settimeout(10)
                    val = int.from_bytes(read_mem(gdb, int(args.watch, 16), 4), "little")
                    if val == want:
                        print("stopped after %d writes, value %08x:" % (hits, val), stop)
                        break
                    gdb.s.settimeout(args.watch_timeout)
                    gdb.cmd("c", reply=False)
            except socket.timeout:
                print("watchpoint not hit (%d writes seen)" % hits)
                gdb.interrupt()
            gdb.s.settimeout(10)

        if args.screenshot:
            screenshot(gdb, args.screenshot)

        if args.profile:
            profile(gdb, args.profile, args.top, args.lines)
            gdb.cmd("k", reply=False)
            return

        threads = []
        r = gdb.cmd("qfThreadInfo")
        while r.startswith("m"):
            threads += r[1:].split(",")
            r = gdb.cmd("qsThreadInfo")

        rows = []
        for t in threads:
            gdb.cmd("Hg" + t)
            w = regs(gdb)
            rows.append((t, w[15], w[14], w[13]))
        pcs = []
        for _, pc, lr, _ in rows:
            pcs += ["0x%x" % pc, "0x%x" % (lr & ~1)]
        loc = ra.where(pcs)
        for t, pc, lr, sp in rows:
            print("thread %s  sp %08x" % (t, sp))
            print("   pc %08x  %s" % (pc, loc.get("0x%x" % pc, "")))
            print("   lr %08x  %s" % (lr, loc.get("0x%x" % (lr & ~1), "")))
            if args.stack and t == args.stack:
                # Words on the stack that look like return addresses into
                # .text: a rough backtrace when LR is unusable.
                raw = gdb.cmd("m%x,%x" % (sp, 1024))
                words = [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little")
                         for i in range(0, len(raw) - 7, 8)]
                code = [w for w in words if 0x100000 <= w < 0x200000]
                cl = ra.where(["0x%x" % (w - 4) for w in code])
                for w in code:
                    print("      ret? %08x  %s" % (w, cl.get("0x%x" % (w - 4), "")))
        gdb.cmd("k", reply=False)
    finally:
        cfg.write_text(text, encoding="utf-8", errors="surrogateescape")
        time.sleep(1)
        if proc and proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
