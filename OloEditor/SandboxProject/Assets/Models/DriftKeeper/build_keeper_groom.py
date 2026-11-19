"""Grow the keeper's groom on the final exported mesh, in the engine's +Y frame."""

import sys
from pathlib import Path
from math import cos, pi, sin

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "Dog"))
from keeper_foundation import follicle, root_uv
from build_dog_groom import make_curves, export_groom_abc, layer, GUARD, LONGHAIR, WHISKER

COLLECTION = "KeeperGroom"
RECIPE = [
    ("cheek", "beard", LONGHAIR, 23000, 0.00034),
    ("muzzle", "moustache", GUARD, 3800, 0.00024),
    ("skull", "scalp", LONGHAIR, 13500, 0.00030),
    ("brow", "eyebrow", GUARD, 2100, 0.00026),
    ("eyerim", "eyelash", WHISKER, 270, 0.00014),
]


def grow(body):
    rng = np.random.default_rng(1871)
    groups = []
    for region, name, role, count, width in RECIPE:
        pts = []
        uvs = []
        widths = []
        salt = []
        while len(pts) < count:
            s = rng.choice((-1, 1))
            u, v = rng.random(2)
            root = follicle(name, s, u, v)
            x, y, z = root
            a = np.arctan2(x, z + 0.009)
            if name == "beard":
                length = 0.026 + 0.050 * (1 - u) + 0.010 * cos(5 * a) - 0.28 * (y - 1.529)
                length *= 1 - 0.73 * v**4
                flow = np.array((-0.17 * sin(a) + 0.045 * s + 0.08 * sin(9 * a), -1, 0.26 + 0.13 * cos(a)))
            elif name == "moustache":
                length = 0.014 + 0.013 * (1 - u)
                flow = np.array(((0.70 + 0.21 * u) * s, -0.76 + 0.24 * u, 0.36))
            elif name == "scalp":
                if y < 1.656 and 1.30 < abs(a) < 1.90:
                    continue
                length = 0.021 + 0.012 * cos(a) ** 2
                flow = np.array((0.24 * sin(a), -0.9, -0.38))
            elif name == "eyebrow":
                length = 0.006 + 0.004 * (1 - u)
                flow = np.array((0.78 * s, 0.34 - 0.63 * u, 0.38))
            else:
                t = 0.07 + (pi - 0.14) * u
                length = 0.003 + 0.0022 * sin(t)
                flow = np.array((0.50 * cos(t), 0.40, 0.82))
            length *= rng.uniform(0.74, 1.22)
            flow /= np.linalg.norm(flow)
            outward = np.array((sin(a), 0, cos(a))) if name != "eyelash" else np.array((0, 0, 1))
            t = np.linspace(0, 1, 7)[:, None]
            # Neighbouring roots share a bend, with a small independent stray.
            lock = sin(140 * root[0] + 90 * root[1]) + cos(94 * root[2] - 55 * root[1])
            sweep = np.array((0.15 * lock + rng.normal(0, 0.075), 0.04 * rng.normal(), 0.10 * lock))
            curve = root + t * flow * length + length * (t * (1 - t) * outward * 0.82 + t * t * sweep)
            if name in ("beard", "scalp", "moustache"):
                # Phase shared by roots in one lock, with a few frizzier flyaways.
                phase = 3.2 * lock + 0.45 * cos(310 * x)
                bend = np.sin(2 * pi * t + phase) - sin(phase)
                curve += t * bend * length * np.array((0.052, 0.008, 0.029))
            pts.append(curve)
            widths.append(width * rng.uniform(0.73, 1.22) * (1 - 0.86 * t[:, 0]))
            uvs.append(root_uv(name, s, u, v))
            salt.append(
                rng.random() < (0.53 + 0.16 * sin(100 * root[0]) * cos(90 * root[2]))
                if name not in ("eyelash", "eyebrow")
                else rng.random() < 0.16
            )
        salt = np.array(salt)
        for silver in (False, True):
            mask = salt == silver
            tint = (0.43, 0.40, 0.33) if silver else (0.074, 0.052, 0.036)
            lay = layer(
                region,
                role,
                0,
                0,
                width,
                7,
                suffix=name + ("_silver" if silver else "_pepper"),
                tint=tint,
                tip_tint=(1.05, 1.03, 1.0),
                stiffness=0.88,
            )
            groups.append(
                dict(
                    layer=lay,
                    points=np.array(pts)[mask],
                    widths=np.array(widths)[mask],
                    uv=np.array(uvs)[mask],
                    guide=np.arange(mask.sum()) % 13 == 0,
                )
            )
    obs = make_curves(groups, body, COLLECTION)
    for ob, g in zip(obs, groups):
        mat = bpy.data.materials.new(ob.name)
        mat.diffuse_color = (*g["layer"]["tint"], 1)
        mat.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = mat.diffuse_color
        ob.data.materials.append(mat)
    return obs


def main():
    dest = Path(sys.argv[sys.argv.index("--") + 1]) if "--" in sys.argv else HERE
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str((dest / "KeeperHairFoundation.gltf").resolve()))
    body = next(o for o in bpy.context.selected_objects if o.type == "MESH")
    grow(body)
    sys.path.insert(0, str(HERE.parent / "DriftLighthouse"))
    from lighthouse_package import create_staging_directory, publish_package

    with create_staging_directory() as staging:
        source = Path(staging) / "Keeper.abc"
        export_groom_abc(str(source), COLLECTION, cards=(0.006 * 0.24, 4))
        publish_package(source.parent, dest, [source.name])


if __name__ == "__main__":
    main()
