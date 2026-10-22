"""
banner_glb.py - lays out the HOME Menu banner as a glTF binary for pycgfx:

    world -> worldModel   Bond posed at pose.py's frame; the HOME Menu spins
                          worldModel about its Y axis (faster when you blow
                          into the microphone)
    name  -> nameModel    the GoldenEye logo, at the height of Bond's second
                          jacket button, on the spin axis and drawn over him;
                          pycgfx (patched) makes these bones billboards, so
                          the logo stays still, facing you, while Bond turns

    .tools/venv/Scripts/python.exe tools/banner/banner_glb.py <export dir> <out.glb>
        [--lit] [--vertex-colours]

Units: the HOME Menu camera sits at (0, 1, 44.786) looking down -z with a
30 degree vertical field of view (pycgfx's banner-camera.gltf). glTF and
the game share axes (y up, Bond facing +z), so game positions only scale.
Materials are unlit (KHR_materials_unlit, honoured by the patched pycgfx):
the texture alone. No vertex colours: a model with COLOR_0 crashes or
freezes the HOME Menu (hardware tests, 2026-10-20). Materials whose
vertices share one colour get it multiplied into the texture instead
(flat_colours: the logo's red ring), so only the game's shading gradients
are lost.
"""
import io
import json
import math
import os
import struct
import sys

from PIL import Image, ImageChops

BOND_HEIGHT = 19.5        # banner units, feet to top of head
BOND_FEET_Y = -9.5
# The HOME Menu turns the whole model, not only worldModel (seen on
# hardware), so the logo stands on the spin axis, where turning only spins
# it in place and the billboard keeps it facing the screen. It is drawn on
# top of Bond (extras.ge_on_top, patched pycgfx) rather than in front of him.
LOGO_WIDTH = 28.5         # 24 seen from 7 units nearer, as first approved
LOGO_Z = 0.0
BUTTON_HEIGHT = 0.55      # second jacket button, as a fraction of Bond's height

WRAP = {0: 10497, 1: 33648, 2: 33071}   # N64 wrap / mirror / clamp


# ---- posing (the game's matrix maths, column vectors) -----------------------
def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def translate(p):
    return [[1, 0, 0, p[0]], [0, 1, 0, p[1]], [0, 0, 1, p[2]], [0, 0, 0, 1]]


def quat_euler_zyx(x, y, z):
    """Rz(z) Ry(y) Rx(x) (matrix_4x4_set_rotation_around_xyz) as (w, x, y, z)."""
    cx, sx = math.cos(x / 2), math.sin(x / 2)
    cy, sy = math.cos(y / 2), math.sin(y / 2)
    cz, sz = math.cos(z / 2), math.sin(z / 2)
    return (cz * cy * cx + sz * sy * sx, cz * cy * sx - sz * sy * cx,
            cz * sy * cx + sz * cy * sx, sz * cy * cx - cz * sy * sx)


def quat_half(q):
    """Half the rotation (slerp from identity by 0.5, shortest way)."""
    if q[0] < 0:
        q = tuple(-c for c in q)
    h = (q[0] + 1, q[1], q[2], q[3])
    n = math.sqrt(sum(c * c for c in h))
    return tuple(c / n for c in h)


def quat_mat(q):
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 0],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 0],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1]]


def apply(m, p):
    return [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3)]


def posed_bond(d):
    rots = d["anim"]["frames"][d["anim"]["frame"]]
    g = {}
    for key, b in d["bones"].items():                 # parents come first
        parent = g[b["parent"]] if b["parent"] else translate((0, 0, 0))
        m = mat_mul(parent, translate(b["origin"]))
        if "joint" in b:
            q = quat_euler_zyx(*rots[str(b["joint"])])
            if "half_of" in b:
                q = quat_half(q)
            m = mat_mul(m, quat_mat(q))
        g[key] = m
    out = []
    for v in d["verts"]:
        head = d["bones"][v["bone"]]["head"]
        out.append(apply(g[v["bone"]], [v["pos"][k] - head[k] for k in range(3)]))
    return out


# ---- glTF writer -----------------------------------------------------------------
class Glb:
    def __init__(s, unlit=True, vertex_colours=False):
        s.unlit = unlit
        s.vertex_colours = vertex_colours
        s.bin = bytearray()
        s.j = dict(asset=dict(version="2.0", generator="ge007-3ds tools/banner"),
                   extensionsUsed=["KHR_materials_unlit"], scene=0, scenes=[dict(nodes=[])],
                   nodes=[], meshes=[], materials=[], textures=[], images=[], samplers=[],
                   accessors=[], bufferViews=[], buffers=[])
        s.image_ids = {}
        s.sampler_ids = {}

    def view(s, data, target=None):
        while len(s.bin) % 4:
            s.bin.append(0)
        bv = dict(buffer=0, byteOffset=len(s.bin), byteLength=len(data))
        if target:
            bv["target"] = target
        s.bin += data
        s.j["bufferViews"].append(bv)
        return len(s.j["bufferViews"]) - 1

    def accessor(s, values, kind, comp=5126, target=34962):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        fmt = {5126: "f", 5123: "H"}[comp]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        acc = dict(bufferView=s.view(struct.pack(f"<{len(flat)}{fmt}", *flat), target),
                   componentType=comp, count=len(values), type=kind)
        if kind == "VEC3" and comp == 5126:
            acc["min"] = [min(v[k] for v in values) for k in range(3)]
            acc["max"] = [max(v[k] for v in values) for k in range(3)]
        s.j["accessors"].append(acc)
        return len(s.j["accessors"]) - 1

    def image(s, path, tint=None):
        key = (path, tint)
        if key not in s.image_ids:
            im = Image.open(path).convert("RGBA")
            if tint:
                im = ImageChops.multiply(im, Image.new("RGBA", im.size, tint))
            # the PICA wants power-of-two sides of at least 8; the game has
            # 1x1 colour swatches and 48-wide textures (UVs are normalised,
            # so nearest resampling keeps every texel where it was)
            def pow2(x):
                return max(8, 1 << (x - 1).bit_length())
            size = (pow2(im.width), pow2(im.height))
            if size != im.size:
                im = im.resize(size, Image.NEAREST)
            buf = io.BytesIO()
            im.save(buf, "PNG")
            s.j["images"].append(dict(name=os.path.splitext(os.path.basename(path))[0],
                                      mimeType="image/png", bufferView=s.view(buf.getvalue())))
            s.image_ids[key] = (len(s.j["images"]) - 1, im.getextrema()[3][0] < 255)
        return s.image_ids[key]

    def material(s, name, path, wrap_s, wrap_t, xlu, tint=None, on_top=False):
        """tint: a colour multiplied into the texture (see flat_colours);
        on_top: drawn over everything else (extras.ge_on_top)."""
        img, has_alpha = s.image(path, tint)
        key = (wrap_s, wrap_t)
        if key not in s.sampler_ids:
            s.j["samplers"].append(dict(magFilter=9729, minFilter=9729,
                                        wrapS=WRAP[wrap_s], wrapT=WRAP[wrap_t]))
            s.sampler_ids[key] = len(s.j["samplers"]) - 1
        s.j["textures"].append(dict(source=img, sampler=s.sampler_ids[key]))
        m = dict(name=name, pbrMetallicRoughness=dict(baseColorTexture=dict(index=len(s.j["textures"]) - 1),
                                           metallicFactor=0.0, roughnessFactor=1.0))
        if s.unlit:
            m["extensions"] = dict(KHR_materials_unlit={})
        if on_top:
            m["extras"] = dict(ge_on_top=True)
        if xlu:
            m["alphaMode"] = "BLEND"
        elif has_alpha:
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
        s.j["materials"].append(m)
        return len(s.j["materials"]) - 1

    def mesh(s, name, pos, cols, tris, uvs, mats):
        """One primitive per material; every corner its own vertex."""
        prims = []
        for mi, mat in enumerate(mats):
            P, U, C = [], [], []
            for t, tuv in zip(tris, uvs):
                if t[3] != mi:
                    continue
                for vi, uv in zip(t[:3], tuv):
                    P.append(pos[vi])
                    U.append((uv[0], 1.0 - uv[1]))   # glTF v runs down the image
                    C.append([c / 255.0 for c in cols[vi]])
            if not P:
                continue
            attrs = dict(POSITION=s.accessor(P, "VEC3"), TEXCOORD_0=s.accessor(U, "VEC2"))
            if s.vertex_colours:
                attrs["COLOR_0"] = s.accessor(C, "VEC4")
            prims.append(dict(attributes=attrs,
                              indices=s.accessor(list(range(len(P))), "SCALAR", 5123, 34963),
                              material=mat))
        s.j["meshes"].append(dict(name=name, primitives=prims))
        return len(s.j["meshes"]) - 1

    def node(s, name, parent=None, **kw):
        s.j["nodes"].append(dict(name=name, **kw))
        i = len(s.j["nodes"]) - 1
        if parent is None:
            s.j["scenes"][0]["nodes"].append(i)
        else:
            s.j["nodes"][parent].setdefault("children", []).append(i)
        return i

    def write(s, path):
        if not s.unlit:
            s.j["extensionsUsed"].remove("KHR_materials_unlit")
        s.j["buffers"].append(dict(byteLength=len(s.bin)))
        for k in [k for k, v in s.j.items() if v == []]:
            del s.j[k]
        js = json.dumps(s.j).encode()
        js += b" " * (-len(js) % 4)
        while len(s.bin) % 4:
            s.bin.append(0)
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(s.bin))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js
        out += struct.pack("<II", len(s.bin), 0x004E4942) + s.bin
        open(path, "wb").write(out)


def flat_colours(verts, tris, nmats):
    """Per material, the one vertex colour all its triangles share, or None.
    Those are baked into the texture, since the banner can't have vertex
    colours (the ring of the logo's 0 is red this way)."""
    seen = [set() for _ in range(nmats)]
    for t in tris:
        for i in t[:3]:
            seen[t[3]].add(tuple(verts[i]["col"]))
    return [next(iter(c)) if len(c) == 1 and next(iter(c)) != (255, 255, 255, 255) else None
            for c in seen]


def colours(verts, tris, flat):
    """Vertex colours with the baked ones turned white."""
    cols = [v["col"] for v in verts]
    for t in tris:
        if flat[t[3]]:
            for i in t[:3]:
                cols[i] = [255, 255, 255, 255]
    return cols


def main(src, out, unlit=True, vertex_colours=False):
    d = json.load(open(os.path.join(src, "bond.json")))
    g = Glb(unlit, vertex_colours)

    # Bond: feet on BOND_FEET_Y, spinning about his pelvis (the game origin)
    pos = posed_bond(d)
    lo = min(p[1] for p in pos)
    hi = max(p[1] for p in pos)
    k = BOND_HEIGHT / (hi - lo)
    pos = [[p[0] * k, (p[1] - lo) * k + BOND_FEET_Y, p[2] * k] for p in pos]
    flat = flat_colours(d["verts"], d["tris"], len(d["materials"]))
    mats = [g.material(f"bond{i}_tex{m['tex']}", os.path.join(src, f"tex{m['tex']}.png"),
                       m["wrap_s"], m["wrap_t"], m["xlu"], flat[i])
            for i, m in enumerate(d["materials"])]
    bond = g.mesh("Bond", pos, colours(d["verts"], d["tris"], flat), d["tris"], d["uvs"], mats)
    world = g.node("world")
    g.node("worldModel", world, mesh=bond)

    # logo: centred on the name bone, which stands in front of Bond
    lg = d["logo"]
    kl = LOGO_WIDTH / lg["size"][0]
    lpos = [[c * kl for c in v["pos"]] for v in lg["verts"]]
    lflat = flat_colours(lg["verts"], lg["tris"], len(lg["materials"]))
    lmats = [g.material(f"logo{i}", os.path.join(src, f"{m['tex']}.png"),
                        m["wrap_s"], m["wrap_t"], m["xlu"], lflat[i], on_top=True)
             for i, m in enumerate(lg["materials"])]
    logo = g.mesh("GoldenEye logo", lpos, colours(lg["verts"], lg["tris"], lflat), lg["tris"],
                  lg["uvs"], lmats)
    name = g.node("name", translation=[0.0, BOND_FEET_Y + BUTTON_HEIGHT * BOND_HEIGHT, LOGO_Z])
    g.node("nameModel", name, mesh=logo)

    g.write(out)
    print(f"wrote {out}: {len(d['tris']) + len(lg['tris'])} triangles")


if __name__ == "__main__":
    # --lit: pycgfx's own materials (HOME Menu lighting); --vertex-colours:
    # add COLOR_0, which crashes the HOME Menu (tested 2026-10-20). Both only
    # for narrowing down hardware problems.
    flags = sys.argv[3:]
    main(sys.argv[1], sys.argv[2], "--lit" not in flags, "--vertex-colours" in flags)
