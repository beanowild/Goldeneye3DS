"""
export.py - turns Bond (DJ/tuxedo body + Brosnan head) and the silenced PP7
prop into bond.json plus PNG textures, for blender_build.py.

    py -3 tools/banner/export.py <out dir>

Everything is in the game's model units (Bond is about 1700 tall) and in
the rest pose (every joint rotation zero: arms and legs straight out
sideways), with y up and Bond facing +z, as in the files. The pose is
a frame of one of the game's animations, picked in pose.py.
"""
import json
import math
import os
import struct
import sys

from gemodel import Model, Runner, Mesh, bef
import geanim
import getex

OBSEG = geanim.ASSETS + "obseg/"

# guard skeleton joint ids (src/game/... skeleton_guard) by name
JOINT_NAMES = {1: "pelvis", 2: "torso", 3: "head", 4: "upperarm.L", 5: "upperarm.R",
               6: "forearm.L", 7: "forearm.R", 8: "hand.L", 9: "hand.R",
               10: "thigh.L", 11: "thigh.R", 12: "shin.L", 13: "shin.R",
               14: "foot.L", 15: "foot.R"}

# the chr's right hand holds a weapon at Switches[3] (front.c cast screens)
RIGHT_HAND_JOINT = 9


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def run_tree(m, runner, keep_lod, nodes_out=None):
    """Runs every display list node of m in render order. LOD nodes (0x08)
    whose MinDistance isn't 0 are skipped, so only the near meshes come out."""
    def visit(n):
        if n.opcode == 0x08 and bef(m.data, n.data) != 0.0 and keep_lod:
            return
        if n.opcode == 0x0C:   # gunfire (muzzle flash)
            return
        r = m.dl(n)
        if r:
            pri, sec, vtx, nv, typ = r
            runner.seg[4] = vtx
            runner.xlu = False
            runner.run(pri)
            if sec:
                runner.xlu = True
                runner.run(sec)
                runner.xlu = False
        for c in n.children:
            visit(c)
    for root in [x for x in m.nodes.values() if x.parent is None]:
        visit(root)


def export_logo(out):
    """The title screen's GoldenEye logo (constructor_menu04_goldeneyelogo in
    front.c) as plain triangles, its lighting and environment map baked in
    for the view straight on, which is how a billboarded banner logo is
    always seen. Positions are centred, in game units, facing +z."""
    logo = Model(OBSEG + "prop/PgoldeneyelogoZ.bin", 0, 2)
    mesh = Mesh()
    r = Runner(logo, mesh)
    run_tree(logo, r, True)
    # gelogolight: ambient 0x96, one white light towards (77, 77, 46);
    # guLookAtReflect from (0, 0, 4000): lookat x = (-1, 0, 0), y = (0, 1, 0)
    ln = math.sqrt(77 * 77 + 77 * 77 + 46 * 46)
    light = (77 / ln, 77 / ln, 46 / ln)

    def normal(v):
        n = [c - 256 if c > 127 else c for c in v[6:9]]
        l = math.sqrt(sum(c * c for c in n))
        return [c / l for c in n] if l else [0.0, 0.0, 0.0]

    lo = [min(v[1 + k] for v in mesh.verts) for k in range(3)]
    hi = [max(v[1 + k] for v in mesh.verts) for k in range(3)]
    mid = [(a + b) / 2 for a, b in zip(lo, hi)]
    verts, tris, uvs, mats, mat_index = [], [], [], [], {}
    for v in mesh.verts:
        n = normal(v)
        shade = min(255, round(0x96 + 255 * max(0.0, sum(a * b for a, b in zip(n, light)))))
        verts.append(dict(pos=[v[1 + k] - mid[k] for k in range(3)], col=[shade] * 3 + [255],
                          n=n))
    for ids, (timg, xlu, texgen, lit) in mesh.tris:
        if not lit:
            # lighting off (the ring of the 0): the vertex colour as is
            for i in ids:
                verts[i]["col"] = list(mesh.verts[i][6:10])
        key = (timg, texgen)
        if key not in mat_index:
            _, off, fmt, siz, w, h, ws, wt = timg
            assert (fmt, siz) == (0, 2), "logo textures are RGBA16"
            px = bytearray()
            for i in range(w * h):
                px += bytes(getex.rgba5551(struct.unpack_from(">H", logo.data, off + 2 * i)[0]))
            name = f"logo{off:x}"
            getex.write_png(os.path.join(out, f"{name}.png"), w, h, bytes(px))
            mat_index[key] = len(mats)
            mats.append(dict(tex=name, w=w, h=h, wrap_s=ws, wrap_t=wt, xlu=xlu))
        m = mats[mat_index[key]]
        uv = []
        for i in ids:
            if texgen:
                n = verts[i]["n"]
                uv.append(((1.0 - n[0]) / 2, 1.0 - (n[1] + 1.0) / 2))
            else:
                v = mesh.verts[i]
                uv.append((v[4] / m["w"], 1.0 - v[5] / m["h"]))
        tris.append(list(ids) + [mat_index[key]])
        uvs.append(uv)
    return dict(verts=verts, tris=tris, uvs=uvs, materials=mats, size=[b - a for a, b in zip(lo, hi)])


def main(out):
    os.makedirs(out, exist_ok=True)

    # ---- body skeleton -------------------------------------------------
    body = Model(OBSEG + "chr/CdjbondZ.bin", 7, 13)
    bones = {}          # matrix key -> bone
    world = {}          # matrix key -> rest position

    def group_parent(n):
        p = n.parent
        while p is not None and p.opcode != 2:
            p = p.parent
        return p

    for n, _ in body.walk():
        if n.opcode != 2:
            continue
        g = body.group(n)
        gp = group_parent(n)
        pkey = None
        base = (0.0, 0.0, 0.0)
        if gp is not None:
            pkey = f"b{body.group(gp)['mtx'][0]}"
            base = world[pkey]
        key = f"b{g['mtx'][0]}"
        world[key] = add(base, g["origin"])
        name = JOINT_NAMES.get(g["joint"], f"joint{g['joint']}")
        bones[key] = dict(name=name, parent=pkey, head=world[key], joint=g["joint"],
                          origin=g["origin"])
        if g["mtx"][1] >= 0:
            # matrix 1: the joint's rotation halved, for vertices that bridge
            # it and its parent (modelBuildGroupMatrices)
            hkey = f"b{g['mtx'][1]}"
            world[hkey] = world[key]
            bones[hkey] = dict(name=name + ".half", parent=pkey, head=world[key], half_of=key,
                               joint=g["joint"], origin=g["origin"])

    mesh = Mesh()
    r = Runner(body, mesh)
    run_tree(body, r, True)
    body_verts = len(mesh.verts)

    # ---- head: rendered at the head placeholder with the head joint's
    # matrix, which is also the head file's matrix 0
    head = Model(OBSEG + "chr/CheadbrosnanZ.bin", 2, 5)
    rh = Runner(head, mesh)
    run_tree(head, rh, True)
    for i in range(body_verts, len(mesh.verts)):
        v = mesh.verts[i]
        mesh.verts[i] = (("b", v[0]),) + v[1:]
    for i in range(body_verts):
        v = mesh.verts[i]
        mesh.verts[i] = (("b", v[0]),) + v[1:]

    # ---- gun: the prop's GroupSimple root sits at its origin in the hand
    # joint's space (process_15_subposition)
    gun = Model(OBSEG + "prop/PchrwppksilZ.bin", 3, 6)
    gun_start = len(mesh.verts)
    rg = Runner(gun, mesh)
    run_tree(gun, rg, True)
    gun_root = [n for n in gun.nodes.values() if n.opcode == 0x15][0]
    gorigin = gun.simple_group(gun_root)["origin"]
    hand_key = [k for k, b in bones.items() if b.get("joint") == RIGHT_HAND_JOINT][0]
    world["g0"] = add(world[hand_key], gorigin)
    bones["g0"] = dict(name="pp7", parent=hand_key, head=world["g0"], origin=gorigin)
    for i in range(gun_start, len(mesh.verts)):
        v = mesh.verts[i]
        mesh.verts[i] = (("g", v[0]),) + v[1:]

    # ---- textures and materials ----------------------------------------
    tex_size = {}
    mats = []
    mat_index = {}
    tris = []
    for ids, mat in mesh.tris:
        if mat not in mat_index:
            timg, xlu, _, _ = mat
            entry = dict(xlu=xlu, tex=None)
            if timg:
                num, sm, tm = timg
                if num not in tex_size:
                    w, h, px = getex.decode(num)
                    getex.write_png(os.path.join(out, f"tex{num}.png"), w, h, px)
                    tex_size[num] = (w, h)
                entry.update(tex=num, wrap_s=sm, wrap_t=tm)
            mat_index[mat] = len(mats)
            mats.append(entry)
        tris.append(list(ids) + [mat_index[mat]])

    verts = []
    tri_uv = []
    for t in tris:
        m = mats[t[3]]
        uv = []
        for i in t[:3]:
            v = mesh.verts[i]
            if m["tex"] is not None:
                w, h = tex_size[m["tex"]]
                uv.append((v[4] / w, 1.0 - v[5] / h))
            else:
                uv.append((0.0, 0.0))
        tri_uv.append(uv)
    for v in mesh.verts:
        (kind, mi) = v[0]
        key = f"{kind}{mi}"
        pos = add(world[key], v[1:4])
        verts.append(dict(bone=key, pos=pos, col=list(v[6:10])))

    # every frame of the animation the pose comes from (pose.py)
    pose = {}
    exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "pose.py")).read(), pose)
    anims = geanim.Anims()
    nframes = anims.header(pose["ANIM"])["frames"]
    frames = [anims.rotations(pose["ANIM"], f) for f in range(nframes)]
    anim = dict(name=pose["ANIM"], frame=pose["FRAME"],
                frames=[{str(j): r for j, r in fr.items()} for fr in frames])

    logo = export_logo(out)

    json.dump(dict(bones=bones, verts=verts, tris=tris, uvs=tri_uv, materials=mats, anim=anim,
                   logo=logo),
              open(os.path.join(out, "bond.json"), "w"))
    print(f"{len(verts)} verts, {len(tris)} tris, {len(mats)} materials, "
          f"{len(tex_size)} textures, {len(bones)} bones")
    lo = [min(v["pos"][k] for v in verts[gun_start:]) for k in range(3)]
    hi = [max(v["pos"][k] for v in verts[gun_start:]) for k in range(3)]
    print("gun bounds", lo, hi)
    lo = [min(v["pos"][k] for v in verts[:gun_start]) for k in range(3)]
    hi = [max(v["pos"][k] for v in verts[:gun_start]) for k in range(3)]
    print("bond bounds", lo, hi)


if __name__ == "__main__":
    main(sys.argv[1])
