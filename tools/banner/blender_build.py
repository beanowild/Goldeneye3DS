"""
blender_build.py - builds the banner scene from export.py's output.

    blender -b -P tools/banner/blender_build.py -- <export dir> <out.blend> [preview.png ...]

Makes one armature (the guard skeleton: joints plus the ".half" bones the
game uses to bridge elbows, knees and the waist, driven at half the
joint's rotation), one mesh skinned rigidly to it (every vertex follows
exactly one game matrix, as on the N64), unlit materials (texture times
vertex colour, which is all the game's combiner does for characters) and
the game animation named in pose.py keyed on every frame, the scene left on
pose.py's frame (scrub the timeline for others).

Axes: game y-up / Bond facing +z becomes Blender z-up / facing -y; game
units / 1000 = metres (Bond is about 1.8 m).
"""
import faulthandler
faulthandler.enable()
import bpy
import json
import math
import os
import sys
from mathutils import Euler, Matrix, Quaternion, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
SRC, OUT = argv[0], argv[1]
PREVIEWS = argv[2:]
S = 0.001


def g2b(p):
    return Vector((p[0] * S, -p[2] * S, p[1] * S))


d = json.load(open(os.path.join(SRC, "bond.json")))
scene = bpy.context.scene
for o in list(bpy.data.objects):   # the startup cube, light and camera
    bpy.data.objects.remove(o)

# ---- armature -------------------------------------------------------------
arm_data = bpy.data.armatures.new("Bond_rig")
arm = bpy.data.objects.new("Bond_rig", arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.show_in_front = True
bpy.ops.object.mode_set(mode="EDIT")

bones = d["bones"]
name_of = {k: b["name"] for k, b in bones.items()}
child_of = {"pelvis": "torso", "torso": "head", "upperarm.L": "forearm.L", "forearm.L": "hand.L",
            "upperarm.R": "forearm.R", "forearm.R": "hand.R", "thigh.L": "shin.L",
            "shin.L": "foot.L", "thigh.R": "shin.R", "shin.R": "foot.R"}
head_of = {b["name"]: g2b(b["head"]) for b in bones.values()}
# segments without a child joint: hands carry on along the forearm, feet
# forward along the toes, the head up, the gun along its barrel (game -x)
end_tail = {"hand.L": ("forearm.L", 0.09), "hand.R": ("forearm.R", 0.09)}

ebs = {}
for key, b in bones.items():
    nm = b["name"]
    base = nm[:-5] if nm.endswith(".half") else nm
    eb = arm_data.edit_bones.new(nm)
    eb.head = head_of[base]
    if base in child_of:
        eb.tail = head_of[child_of[base]]
    elif base in end_tail:
        par, ln = end_tail[base]
        eb.tail = eb.head + (head_of[base] - head_of[par]).normalized() * ln
    elif base.startswith("foot"):
        eb.tail = eb.head + Vector((0, -0.12, 0))
    elif base == "head":
        eb.tail = eb.head + Vector((0, 0, 0.22))
    elif base == "pp7":
        eb.tail = eb.head + Vector((-0.2, 0, 0))
    ebs[key] = eb
for key, b in bones.items():
    if b["parent"]:
        ebs[key].parent = ebs[b["parent"]]
bpy.ops.object.mode_set(mode="OBJECT")

# helper bones in their own hidden collection
helpers = arm_data.collections.new("Game half-joints")
main = arm_data.collections.new("Skeleton")
for b in arm_data.bones:
    (helpers if b.name.endswith(".half") else main).assign(b)
helpers.is_visible = False

for key, b in bones.items():
    if "half_of" in b:
        pb = arm.pose.bones[b["name"]]
        c = pb.constraints.new("COPY_ROTATION")
        c.target = arm
        c.subtarget = name_of[b["half_of"]]
        c.owner_space = c.target_space = "LOCAL"
        c.influence = 0.5

# ---- mesh -------------------------------------------------------------------
verts = d["verts"]
remap = {}
co = []
vbone = []
for i, v in enumerate(verts):
    k = (v["bone"], tuple(round(x, 3) for x in v["pos"]))
    if k not in remap:
        remap[k] = len(co)
        co.append(g2b(v["pos"]))
        vbone.append(v["bone"])
    v["new"] = remap[k]

# drop triangles that collapse when equal vertices merge, and repeats
faces, tris, tri_uvs, seen = [], [], [], set()
for t, tuv in zip(d["tris"], d["uvs"]):
    f = [verts[i]["new"] for i in t[:3]]
    k = tuple(sorted(f))
    if len(set(f)) < 3 or k in seen:
        continue
    seen.add(k)
    faces.append(f)
    tris.append(t)
    tri_uvs.append(tuv)
d["tris"], d["uvs"] = tris, tri_uvs
me = bpy.data.meshes.new("Bond")
me.from_pydata(co, [], faces, shade_flat=True)
obj = bpy.data.objects.new("Bond", me)
scene.collection.objects.link(obj)

# (adding a layer moves the others: look each up again after the last add)
me.uv_layers.new(name="UV")
me.color_attributes.new("Shade", "BYTE_COLOR", "CORNER")
uv = me.uv_layers["UV"]
col = me.color_attributes["Shade"]
# from_pydata keeps face order and each face's vertex order, so loop 3i+k
# is corner k of triangle i
uv.data.foreach_set("uv", [x for tuv in d["uvs"] for u in tuv for x in u])
col.data.foreach_set("color", [x / 255.0 for t in d["tris"] for vi in t[:3]
                               for x in verts[vi]["col"]])
# set in one go: the first write adds the attribute, which mustn't happen
# while iterating the polygons
me.polygons.foreach_set("material_index", [t[3] for t in d["tris"]])

members = {}
for vi, bk in enumerate(vbone):
    members.setdefault(name_of[bk], []).append(vi)
for nm, vis in members.items():
    obj.vertex_groups.new(name=nm).add(vis, 1.0, "REPLACE")
obj.parent = arm
mod = obj.modifiers.new("Armature", "ARMATURE")
mod.object = arm

# ---- materials ----------------------------------------------------------------
EXT = {0: "REPEAT", 1: "MIRROR", 2: "EXTEND"}
images = {}
for m in d["materials"]:
    mat = bpy.data.materials.new(f"tex{m['tex']}" + (" xlu" if m["xlu"] else ""))
    mat.use_nodes = True
    mat.use_backface_culling = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    out.location = (900, 0)
    vc = nt.nodes.new("ShaderNodeVertexColor")
    vc.layer_name = "Shade"
    vc.location = (-300, -250)
    mul = nt.nodes.new("ShaderNodeMix")
    mul.data_type = "RGBA"
    mul.blend_type = "MULTIPLY"
    mul.inputs["Factor"].default_value = 1.0
    mul.location = (200, 0)
    amul = nt.nodes.new("ShaderNodeMath")
    amul.operation = "MULTIPLY"
    amul.location = (200, -250)
    nt.links.new(vc.outputs["Color"], mul.inputs[7])
    nt.links.new(vc.outputs["Alpha"], amul.inputs[1])
    if m["tex"] is not None:
        if m["tex"] not in images:
            img = bpy.data.images.load(os.path.join(SRC, f"tex{m['tex']}.png"))
            img.name = f"tex{m['tex']}"
            images[m["tex"]] = img
        tx = nt.nodes.new("ShaderNodeTexImage")
        tx.image = images[m["tex"]]
        tx.interpolation = "Linear"   # the N64 filters bilinearly
        tx.location = (-300, 100)
        ws, wt = m["wrap_s"], m["wrap_t"]
        if ws == wt:
            tx.extension = EXT[ws]
        else:
            # wrap per axis: u and v through their own fold before sampling
            tx.extension = "EXTEND"
            uvn = nt.nodes.new("ShaderNodeUVMap")
            uvn.uv_map = "UV"
            uvn.location = (-900, 100)
            sep = nt.nodes.new("ShaderNodeSeparateXYZ")
            sep.location = (-750, 100)
            comb = nt.nodes.new("ShaderNodeCombineXYZ")
            comb.location = (-480, 100)
            nt.links.new(uvn.outputs["UV"], sep.inputs[0])
            for ax, mode, y in (("X", ws, 200), ("Y", wt, 0)):
                f = nt.nodes.new("ShaderNodeMath")
                f.location = (-620, y)
                if mode == 1:
                    f.operation = "PINGPONG"
                    f.inputs[1].default_value = 1.0
                elif mode == 0:
                    f.operation = "FRACT"
                else:
                    f.operation = "MINIMUM"
                    f.inputs[1].default_value = 1.0
                nt.links.new(sep.outputs[ax], f.inputs[0])
                nt.links.new(f.outputs[0], comb.inputs[ax])
            nt.links.new(comb.outputs[0], tx.inputs["Vector"])
        nt.links.new(tx.outputs["Color"], mul.inputs[6])
        nt.links.new(tx.outputs["Alpha"], amul.inputs[0])
    else:
        mul.inputs[6].default_value = (1, 1, 1, 1)
        amul.inputs[0].default_value = 1.0
    emit = nt.nodes.new("ShaderNodeEmission")
    emit.location = (450, 100)
    nt.links.new(mul.outputs[2], emit.inputs["Color"])
    transp = nt.nodes.new("ShaderNodeBsdfTransparent")
    transp.location = (450, -100)
    mix = nt.nodes.new("ShaderNodeMixShader")
    mix.location = (700, 0)
    nt.links.new(amul.outputs[0], mix.inputs[0])
    nt.links.new(transp.outputs[0], mix.inputs[1])
    nt.links.new(emit.outputs[0], mix.inputs[2])
    nt.links.new(mix.outputs[0], out.inputs["Surface"])
    mat.surface_render_method = "BLENDED" if m["xlu"] else "DITHERED"
    obj.data.materials.append(mat)

# ---- pose: the game's own animation, keyed on every frame --------------------------
# Each game matrix is parent * T(origin) * Rz Ry Rx (modelBuildGroupMatrices);
# ".half" matrices use half the joint's rotation. A bone's pose (armature
# space) then is C * G * T(-rest position) * C^-1 * its rest matrix, C being
# the game-to-Blender axes and scale.
C = Matrix(((S, 0, 0, 0), (0, 0, -S, 0), (0, S, 0, 0), (0, 0, 0, 1)))
C_inv = C.inverted()
pbs = arm.pose.bones
anim = d["anim"]


def game_matrices(rots):
    g = {}
    for key, b in bones.items():
        parent = g[b["parent"]] if b["parent"] else Matrix.Identity(4)
        t = Matrix.Translation(Vector(b["origin"]))
        if "joint" not in b:                       # the gun: no joint of its own
            g[key] = parent @ t
            continue
        q = Euler(rots[str(b["joint"])], "XYZ").to_quaternion()
        if "half_of" in b:
            q = Quaternion().slerp(q, 0.5)
        g[key] = parent @ t @ q.to_matrix().to_4x4()
    return g


def apply_pose(rots):
    g = game_matrices(rots)
    for key, b in bones.items():                   # parents come first
        if "half_of" in b:
            continue                               # driven by its constraint
        pb = pbs[b["name"]]
        rest = Matrix.Translation(-Vector(b["head"]))
        pb.matrix = C @ g[key] @ rest @ C_inv @ pb.bone.matrix_local
        bpy.context.view_layer.update()


action_bones = [b["name"] for b in bones.values() if "half_of" not in b]
for pb in pbs:
    pb.rotation_mode = "QUATERNION"
for f, rots in enumerate(anim["frames"]):
    apply_pose(rots)
    for nm in action_bones:
        pbs[nm].keyframe_insert("rotation_quaternion", frame=f)
        pbs[nm].keyframe_insert("location", frame=f)
arm.animation_data.action.name = anim["name"]
for fc in arm.animation_data.action.fcurves:
    for k in fc.keyframe_points:
        k.interpolation = "LINEAR"
scene.frame_start, scene.frame_end = 0, len(anim["frames"]) - 1
scene.render.fps = 30
scene.frame_set(anim["frame"])

# stand on the floor
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()
ev = obj.evaluated_get(dg)
low = min((obj.matrix_world @ v.co).z for v in ev.data.vertices)
arm.location.z -= low
bpy.context.view_layer.update()

# ---- camera, world, previews ---------------------------------------------------
world = bpy.data.worlds.new("World")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.35, 0.6, 0.95, 1)
scene.world = world
scene.render.engine = "BLENDER_EEVEE_NEXT"
scene.view_settings.view_transform = "Standard"
scene.render.resolution_x, scene.render.resolution_y = 800, 1000
scene.render.film_transparent = False

cam_data = bpy.data.cameras.new("Camera")
cam_data.lens = 85
cam = bpy.data.objects.new("Camera", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam


def look(from_, at):
    cam.location = Vector(from_)
    cam.rotation_euler = (Vector(at) - Vector(from_)).to_track_quat("-Z", "Y").to_euler()


VIEWS = {"front": ((0, -6.0, 1.15), (0, 0, 0.98)),
         "game": ((-0.9, 2.6, 1.75), (-0.1, 0, 1.45)),
         "side": ((6.0, 0, 1.15), (0, 0, 0.98)),
         "left": ((-6.0, 0, 1.15), (0, 0, 0.98)),
         "three": ((2.6, -5.4, 1.3), (0, 0, 0.98)),
         "back": ((0, 6.0, 1.15), (0, 0, 0.98))}
for p in PREVIEWS:
    view = os.path.splitext(os.path.basename(p))[0].split("_")[-1]
    if view == "sheet":
        # frames (every 5th, or $SHEET = start:end:step) from the Dam intro's
        # camera angle, for picking one
        scene.render.resolution_x, scene.render.resolution_y = 320, 400
        look(*VIEWS["game"])
        rng = [int(x) for x in os.environ.get("SHEET", f"0:{len(anim['frames'])}:5").split(":")]
        for f in range(*rng):
            scene.frame_set(f)
            scene.render.filepath = p.replace("sheet", f"sheet{f:03d}")
            bpy.ops.render.render(write_still=True)
        scene.render.resolution_x, scene.render.resolution_y = 800, 1000
        scene.frame_set(anim["frame"])
        continue
    look(*VIEWS.get(view, VIEWS["front"]))
    scene.render.filepath = p
    bpy.ops.render.render(write_still=True)
look(*VIEWS["three"])

bpy.ops.file.pack_all()
bpy.ops.wm.save_as_mainfile(filepath=OUT, compress=True)
print("saved", OUT)
