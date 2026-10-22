"""
gemodel.py - reads GoldenEye model files (obseg C*Z.bin / P*Z.bin, as
extracted into external/goldeneye/assets/obseg) and runs their Fast3D display
lists to get plain triangles: positions in each joint's local space, texture
coordinates, vertex colours and the joint each vertex belongs to.

File layout (load_object_fill_header in objecthandler_2.c): numSwitches
pointers, numtextures ModelFileTextures (12 bytes), then the node tree.
Pointers are 0x05000000 + file offset.

Rendering (model.c): each display list node sets segment 4 to its vertex
array and segment 5 to the file; G_MTX loads 0x03000000 + 0x40 * matrix,
the model's per-joint matrices. Vertices stay in the RSP's 16-entry cache
across nodes, which is how a child joint's triangles stitch onto its
parent's vertices.
"""
import struct

SEG_FILE = 0x05000000


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def bes16(b, o): return struct.unpack_from(">h", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def bef(b, o): return struct.unpack_from(">f", b, o)[0]


class Node:
    def __init__(s, off):
        s.off = off
        s.children = []
        s.parent = None


class Model:
    def __init__(s, path, num_switches, num_textures):
        s.data = open(path, "rb").read()
        d = s.data
        s.switches = [be32(d, 4 * i) for i in range(num_switches)]
        to = 4 * num_switches
        s.textures = []
        for i in range(num_textures):
            o = to + 12 * i
            s.textures.append(dict(id=be32(d, o), w=d[o + 4], h=d[o + 5], mips=d[o + 6],
                                   type=d[o + 7], depth=d[o + 8], sflags=d[o + 9], tflags=d[o + 10]))
        s.root_off = to + 12 * num_textures
        s.nodes = {}
        s.root = s.read_node(s.root_off, None)

    def ptr(s, p):
        if p == 0:
            return None
        assert (p & 0xFF000000) == SEG_FILE, hex(p)
        return p & 0xFFFFFF

    def read_node(s, off, parent):
        """The node at off and its siblings after it (recursively children)."""
        first = None
        prev = None
        while off is not None:
            d = s.data
            n = Node(off)
            n.opcode = be16(d, off) & 0xFF
            n.data = s.ptr(be32(d, off + 4))
            n.parent = parent
            s.nodes[off] = n
            if first is None:
                first = n
            if parent is not None:
                parent.children.append(n)
            child = s.ptr(be32(d, off + 0x14))
            nxt = s.ptr(be32(d, off + 0x0C))
            if n.opcode == 0x08:          # LOD: child is Affects
                child = s.ptr(be32(d, n.data + 8))
            elif n.opcode == 0x12:        # switch: child is Controls
                child = s.ptr(be32(d, n.data))
            elif n.opcode == 0x17:        # head placeholder
                child = None
            if child is not None and child not in s.nodes:
                s.read_node(child, n)
            off = nxt if nxt not in s.nodes else None
        return first

    def walk(s, n=None, depth=0):
        if n is None:
            n = s.root
            for r in [x for x in s.nodes.values() if x.parent is None]:
                yield from s.walk(r, 0)
            return
        yield n, depth
        for c in n.children:
            yield from s.walk(c, depth + 1)

    # ---- per-opcode data ------------------------------------------------
    def group(s, n):
        """Opcode 2: origin relative to the parent joint, joint id, matrix."""
        d = s.data
        o = n.data
        return dict(origin=struct.unpack_from(">3f", d, o), joint=be16(d, o + 0xC),
                    mtx=[bes16(d, o + 0xE + 2 * i) for i in range(3)])

    def simple_group(s, n):
        d = s.data
        o = n.data
        return dict(origin=struct.unpack_from(">3f", d, o), g1=bes16(d, o + 0xC), g2=be16(d, o + 0xE))

    def dl(s, n):
        """(primary, secondary, vertex array offset, vertex count) or None."""
        d = s.data
        o = n.data
        if n.opcode == 0x18:
            return (s.ptr(be32(d, o)), s.ptr(be32(d, o + 4)), s.ptr(be32(d, o + 8)), bes16(d, o + 0xC),
                    bes16(d, o + 0x18))
        if n.opcode == 0x04:
            return (s.ptr(be32(d, o)), s.ptr(be32(d, o + 4)), s.ptr(be32(d, o + 0xC)), be16(d, o + 0x10),
                    d[o + 0x12])
        if n.opcode == 0x16:
            return (s.ptr(be32(d, o + 8)), None, s.ptr(be32(d, o + 4)), be32(d, o), 0)
        return None


# ---- display list interpreter -------------------------------------------
G_MTX, G_VTX, G_DL = 0x01, 0x04, 0x06
G_TRI4, G_CLEARGEO, G_SETGEO, G_ENDDL = 0xB1, 0xB6, 0xB7, 0xB8
G_SETOTHERMODE_L, G_SETOTHERMODE_H = 0xB9, 0xBA
G_TEXTURE, G_MOVEWORD, G_TRI1 = 0xBB, 0xBC, 0xBF
G_SETTIMG, G_SETCOMBINE, G_SETENV, G_SETPRIM = 0xFD, 0xFC, 0xFB, 0xFA
G_SETTILE, G_LOADBLOCK, G_SETTILESIZE, G_LOADTLUT = 0xF5, 0xF3, 0xF2, 0xF0
G_LIGHTING, G_TEXTURE_GEN = 0x20000, 0x40000


class Mesh:
    """Output: verts are (joint matrix index, x, y, z, s, t, r, g, b, a)
    with s, t in texels; tris are triples of indices into verts plus a
    material key (Runner.material)."""

    def __init__(s):
        s.verts = []
        s.tris = []


class Runner:
    def __init__(s, model, mesh, log=None):
        s.m = model
        s.mesh = mesh
        s.cache = [None] * 16
        s.mtx = -1
        s.seg = {}
        s.tex_s = s.tex_t = 1.0
        s.tex_on = False
        s.timg = None
        s.img = None
        s.tile_wrap = (0, 0)
        s.geom = 0
        s.combine = 0
        s.othermode_l = 0
        s.env = s.prim = (255, 255, 255, 255)
        s.log = log
        s.xlu = False    # set by the caller for secondary (translucent) lists

    def resolve(s, addr):
        seg = (addr >> 24) & 0xF
        if seg == 5:
            return addr & 0xFFFFFF
        if seg in s.seg:
            return s.seg[seg] + (addr & 0xFFFFFF)
        return None

    def material(s):
        """(texture, translucent, texgen, lit): texture is a GE texture
        number with wrap modes, or ("emb", file offset, format, size, w, h,
        wrap s, wrap t) for one the display list loads from the file itself;
        texgen means s, t come from the normal instead, and lit that vertex
        r, g, b is a normal, not a colour."""
        return (s.timg if s.tex_on else None, s.xlu, bool(s.geom & G_TEXTURE_GEN),
                bool(s.geom & G_LIGHTING))

    def run(s, off):
        d = s.m.data
        stack = []
        while off is not None:
            w0, w1 = be32(d, off), be32(d, off + 4)
            off += 8
            op = w0 >> 24
            if s.log:
                s.log(f"  {off - 8:06x}: {w0:08x} {w1:08x}")
            if op == G_ENDDL:
                off = stack.pop() if stack else None
            elif op == G_DL:
                tgt = s.resolve(w1)
                if tgt is None:
                    continue
                if ((w0 >> 16) & 0xFF) == 0:
                    stack.append(off)
                off = tgt
            elif op == G_MTX:
                assert (w1 >> 24) == 3, hex(w1)
                s.mtx = (w1 & 0xFFFFFF) // 0x40
            elif op == G_VTX:
                par = (w0 >> 16) & 0xFF
                first, n = par & 0xF, (par >> 4) + 1
                src = s.resolve(w1)
                for i in range(n):
                    o = src + 16 * i
                    x, y, z = struct.unpack_from(">3h", d, o)
                    sv, tv = struct.unpack_from(">2h", d, o + 8)
                    r, g, b, a = d[o + 12:o + 16]
                    v = (s.mtx, x, y, z, sv * s.tex_s / 32.0, tv * s.tex_t / 32.0, r, g, b, a)
                    s.mesh.verts.append(v)
                    s.cache[first + i] = len(s.mesh.verts) - 1
            elif op == G_TRI1:
                s.tri((w1 >> 16 & 0xFF) // 10, (w1 >> 8 & 0xFF) // 10, (w1 & 0xFF) // 10)
            elif op == G_TRI4:
                for i in range(4):
                    x, y = (w1 >> (8 * i)) & 0xF, (w1 >> (8 * i + 4)) & 0xF
                    z = (w0 >> (4 * i)) & 0xF
                    if x or y or z:
                        s.tri(x, y, z)
            elif op == G_TEXTURE:
                s.tex_s = ((w1 >> 16) & 0xFFFF) / 65536.0
                s.tex_t = (w1 & 0xFFFF) / 65536.0
                s.tex_on = (w0 & 0xFF) != 0
            elif op == 0xC0:
                # GE's texture command (texLoadFromGdl in tex.c): texture
                # number and wrap modes, expanded to RDP loads at load time
                s.timg = (w1 & 0xFFF, (w0 >> 22) & 3, (w0 >> 20) & 3)
            elif op == G_SETTIMG:
                s.img = (s.resolve(w1), (w0 >> 21) & 7, (w0 >> 19) & 3)
            elif op == G_SETTILE:
                if ((w1 >> 24) & 7) == 0:
                    s.tile_wrap = ((w1 >> 8) & 3, (w1 >> 18) & 3)
            elif op == G_SETTILESIZE:
                if ((w1 >> 24) & 7) == 0 and s.img:
                    w = ((w1 >> 12) & 0xFFF) // 4 + 1
                    h = (w1 & 0xFFF) // 4 + 1
                    s.timg = ("emb",) + s.img + (w, h) + s.tile_wrap
            elif op == G_SETGEO:
                s.geom |= w1
            elif op == G_CLEARGEO:
                s.geom &= ~w1
            elif op == G_SETCOMBINE:
                s.combine = ((w0 & 0xFFFFFF) << 32) | w1
            elif op == G_SETOTHERMODE_L:
                sft, ln = (w0 >> 8) & 0xFF, w0 & 0xFF
                mask = ((1 << ln) - 1) << sft
                s.othermode_l = (s.othermode_l & ~mask) | (w1 & mask)
            elif op == G_SETENV:
                s.env = tuple(w1.to_bytes(4, "big"))
            elif op == G_SETPRIM:
                s.prim = tuple(w1.to_bytes(4, "big"))
            elif op == G_MOVEWORD:
                if (w0 & 0xFF) == 6:   # G_MW_SEGMENT
                    s.seg[((w0 >> 8) & 0xFFFF) >> 2 & 0xF] = s.resolve(w1)

    def tri(s, a, b, c):
        ids = (s.cache[a], s.cache[b], s.cache[c])
        if None in ids:
            return
        s.mesh.tris.append((ids, s.material()))
