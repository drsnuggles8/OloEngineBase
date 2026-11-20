"""Grow the keeper's groom on the final exported mesh, in the engine's +Y frame."""

import sys
from pathlib import Path
from math import cos, exp, pi, sin

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "Dog"))
from keeper_foundation import follicle, hair_coordinates, root_uv
from keeper_face import head_point
from build_dog_groom import make_curves, export_groom_abc, layer, GUARD, LONGHAIR, WHISKER

COLLECTION = "KeeperGroom"
RECIPE = [
    ("cheek", "beard", LONGHAIR, 23000, 0.00017),
    ("muzzle", "moustache", GUARD, 3800, 0.00014),
    ("skull", "scalp", LONGHAIR, 13500, 0.00020),
    ("brow", "eyebrow", GUARD, 850, 0.00015),
    ("eyerim", "eyelash", WHISKER, 270, 0.00010),
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
                if v > 0.52 and rng.random() < 0.97 * ((v - 0.52) / 0.48) ** 0.80:
                    continue
                chin = (1 - u) ** 1.25
                length = (0.008 + 0.018 * chin + 0.014 * exp(-(((u - 0.26) / 0.21) ** 2))) * (
                    1 - 0.89 * v**1.9
                )
                flow = np.array(
                    (-0.26 * sin(a) + 0.13 * sin(9 * a + 53 * y) + 0.09, -0.92, 0.34 * cos(a) + 0.065)
                )
            elif name == "moustache":
                if rng.random() > 0.40 + 0.60 * sin(pi * u) ** 0.4:
                    continue
                length = (0.0034 + 0.0055 * v + 0.0068 * sin(pi * u) + 0.004 * u) * rng.uniform(0.8, 1.18)
                flow = np.array((s * (0.32 + 0.90 * sin(pi * u)), -0.72 - 0.30 * u**2, 0.29))
            elif name == "scalp":
                length = (0.018 + 0.008 * max(0, -cos(a))) * (0.52 + 0.48 * v)
                flow = np.array((0.30 * sin(a), -0.72, -0.67))
            elif name == "eyebrow":
                length = (0.004 + 0.006 * (1 - u)) * (0.64 + 0.54 * rng.random())
                flow = np.array(((0.46 + 0.5 * u) * s, 0.77 - 1.34 * u + 0.18 * rng.normal(), 0.42))
            else:
                t = 0.07 + (pi - 0.14) * u
                length = 0.003 + 0.0022 * sin(t)
                flow = np.array((0.50 * cos(t), 0.40, 0.82))
            if name in ("beard", "scalp"):
                # Follow the changing skin tangent down across the jaw and
                # occiput. A constant direction lifts the entire nape into air.
                angle, height = hair_coordinates(name, s, u, v)
                across = head_point(angle + 0.003, height) - head_point(angle - 0.003, height)
                down = head_point(angle, max(1.518, height - 0.001)) - head_point(angle, height + 0.001)
                across /= np.linalg.norm(across)
                down /= np.linalg.norm(down)
                normal = np.cross(down, across)
                if name == "scalp":
                    flow = down + 0.42 * s * across + 0.19 * normal
                    length *= 0.70 + 0.30 * min(1, v * 4)
                else:
                    # A short beard rounds the underside of the chin, where
                    # the tangent itself turns backward into the throat.
                    flow = 0.52 * down + np.array((0.02, -0.56, 0)) - 0.23 * s * across + 0.29 * normal
            length *= rng.uniform(0.67, 1.28)
            flow /= np.linalg.norm(flow)
            outward = np.array((sin(a), 0, cos(a))) if name != "eyelash" else np.array((0, 0, 1))
            t = np.linspace(0, 1, 7)[:, None]
            # Neighbouring roots share a bend, with a small independent stray.
            lock = sin(140 * root[0] + 90 * root[1]) + cos(94 * root[2] - 55 * root[1])
            sweep = np.array((0.15 * lock + rng.normal(0, 0.075), 0.04 * rng.normal(), 0.10 * lock))
            curve = root + t * flow * length + length * (t * (1 - t) * outward * 0.62 + t * t * sweep)
            if name in ("beard", "scalp", "moustache"):
                # Phase shared by roots in one lock, with a few frizzier flyaways.
                # A lock gathers fibres into a soft tip. Offsetting its cell
                # centre with height breaks up the edge of the beard in profile.
                tangent = np.array((cos(a), 0, -sin(a)))
                cell = 28 * u + 0.86 * sin(9 * u + 4 * v) + 0.22 * sin(18 * v)
                gather = np.clip(0.5 - (cell % 1), -0.5, 0.5) * min(0.0048, 0.35 * length)
                phase = 2.8 * lock + 0.4 * cos(280 * x)
                bend = np.sin(2 * pi * t * 1.25 + phase) - sin(phase)
                curve += t * t * gather * tangent + t * bend * length * (0.079 * tangent + 0.035 * outward)
            if name == "beard":
                curve[:, 2] -= 0.25 * length * cos(a) * t[:, 0] ** 2
            pts.append(curve)
            widths.append(width * rng.uniform(0.73, 1.22) * (1 - 0.86 * t[:, 0]))
            uvs.append(root_uv(name, s, u, v))
            # Age dapples the hair in patches. The moustache stays darker
            # through the centre while the cheek and chin carry more silver.
            grey = 0.29 + 0.48 * (0.5 + 0.5 * sin(96 * x + 1.5 * cos(82 * y))) ** 2
            if name == "moustache":
                grey = 0.12 + 0.35 * u**1.4 + 0.09 * sin(16 * u + s)
            elif name in ("eyelash", "eyebrow"):
                grey = 0.12 + 0.13 * u
            salt.append(rng.random() < grey)
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
