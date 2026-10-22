import sys
from gemodel import Model, Runner, Mesh

OBSEG = "M:/Goldeneye 3ds/external/goldeneye/assets/obseg/"
path, nsw, ntex = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
m = Model(OBSEG + path, nsw, ntex)
print("switches", [hex(x) for x in m.switches])
for t in m.textures:
    print("tex", t)
for n, depth in m.walk():
    info = ""
    if n.opcode == 2:
        info = m.group(n)
    elif n.opcode == 0x15:
        info = m.simple_group(n)
    elif m.dl(n):
        p, sec, v, nv, typ = m.dl(n)
        info = f"pri {p and hex(p)} sec {sec and hex(sec)} vtx {v and hex(v)} n {nv} type {typ}"
    print("  " * depth + f"{n.off:05x} op {n.opcode:02x} {info}")
if len(sys.argv) > 4:
    off = int(sys.argv[4], 16)
    n = m.nodes[off]
    p, sec, v, nv, typ = m.dl(n)
    r = Runner(m, Mesh(), log=print)
    r.seg[4] = v
    r.run(p)
    if sec:
        print("secondary")
        r.run(sec)
