"""
home_preview.py - renders the banner .glb the way the HOME Menu shows it
(its camera, worldModel spinning, the name logo billboarded) at several spin
angles, using CBC-for-YANBF's software preview (.tools/cbc-for-yanbf, MIT).
It's an approximation: the console's exact spin speed isn't known. As on
the console (extras.ge_on_top), the logo is drawn after Bond, over him.

    .tools/venv/Scripts/python.exe tools/banner/home_preview.py <banner.glb> <out.png>
"""
import dataclasses
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".tools", "cbc-for-yanbf", "app"))
import preview  # noqa: E402
from PIL import Image  # noqa: E402

ANGLES = (0, 45, 90, 135, 180, 270)
SCREEN = (400, 240)   # the 3DS top screen


def main(glb, out):
    mesh = preview.load_glb(glb)
    spin = preview.find_spin_node(mesh)
    bills = preview.find_billboard_nodes(mesh)
    sheet = Image.new("RGB", (SCREEN[0] * 3, SCREEN[1] * 2))
    logo_nodes = {i for i in mesh.node_order if mesh.node_names[i] in ("name", "nameModel")}
    on_logo = [n in logo_nodes for n, _ in mesh.vert_src]
    for i, a in enumerate(ANGLES):
        posed = preview.pose(mesh, math.radians(a), spin, bills)
        bond = dataclasses.replace(posed, tris=[t for t in posed.tris if not on_logo[t[0]]])
        logo = dataclasses.replace(posed, tris=[t for t in posed.tris if on_logo[t[0]]])
        im = preview.render(bond, size=SCREEN, ss=2, background=Image.new("RGB", SCREEN, (40, 40, 60)))
        im = preview.render(logo, size=SCREEN, ss=2, background=im.convert("RGB").resize(SCREEN))
        sheet.paste(im.convert("RGB").resize(SCREEN), ((i % 3) * SCREEN[0], (i // 3) * SCREEN[1]))
    sheet.save(out)
    print(f"{out}: {mesh.triangle_count} triangles, spin angles {ANGLES}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
