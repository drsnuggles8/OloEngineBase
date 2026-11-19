"""Build the original keeper, his eye and the LOD/scene contract from scratch.

Run with Python + NumPy, or Blender in background mode (see README).
"""

import argparse
from math import cos, pi, radians, sin
import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from keeper_mesh import Gltf, MORPHS, Surface
from keeper_face import EYE_R, EYE_X, EYE_Y, EYE_Z, face
from keeper_dress import PALETTE, body
import keeper_foundation
from keeper_finishes import bake, dress_materials

CLIPS = ["Idle", "Wave", "LanternRaise"]
LEVELS = ["Keeper", "KeeperLOD1", "KeeperLOD2"]
EYES = [
    {"bone": "eye_" + suffix, "centre": [sign * EYE_X, EYE_Y, EYE_Z]}
    for sign, suffix in [(1, "L"), (-1, "R")]
]


def bones():
    joints = {
        "root": (None, (0, 0, 0)),
        "pelvis": ("root", (0, 0.90, 0)),
        "spine": ("pelvis", (0, 1.06, 0)),
        "chest": ("spine", (0, 1.26, 0)),
        "neck": ("chest", (0, 1.453, -0.004)),
        "head": ("neck", (0, 1.529, -0.018)),
        "jaw": ("head", (0, 1.602, -0.012)),
    }
    for eye in EYES:
        joints[eye["bone"]] = ("head", eye["centre"])
    for sign, suffix in [(1, "L"), (-1, "R")]:
        for name, parent, xyz in [
            ("upperarm", "chest", (0.171, 1.400, 0)),
            ("forearm", "upperarm", (0.241, 1.15, 0.02)),
            ("hand", "forearm", (0.251, 0.944, 0.054)),
            ("thigh", "pelvis", (0.098, 0.90, 0)),
            ("shin", "thigh", (0.108, 0.470, 0.011)),
            ("boot", "shin", (0.111, 0.13, -0.004)),
        ]:
            parent = parent if parent in ("chest", "pelvis") else parent + "_" + suffix
            joints[name + "_" + suffix] = (parent, (sign * xyz[0], xyz[1], xyz[2]))
    joints["lantern"] = ("hand_L", (0.229, 0.826, 0.058))
    return joints


def quaternion(angles):
    x, y, z = [radians(a) / 2 for a in angles]
    cx, cy, cz, sx, sy, sz = cos(x), cos(y), cos(z), sin(x), sin(y), sin(z)
    return [
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    ]


def animate(file, ids):
    animations = []
    for name, length in zip(CLIPS, [5, 3.6, 4]):
        samplers, channels = [], []

        def track(bone, keys):
            times = file.accessor([t * length for t, _ in keys], "SCALAR", bounds=True)
            rots = file.accessor([quaternion(angles) for _, angles in keys], "VEC4")
            channels.append(dict(sampler=len(samplers), target=dict(node=ids[bone], path="rotation")))
            samplers.append(dict(input=times, output=rots, interpolation="LINEAR"))

        # Every clip has a rest key for each joint it does not move. Changing
        # from the wave to the lantern must release the right shoulder.
        if name == "Idle":
            for bone, amp in [
                ("pelvis", (0, 0.25, 0.42)),
                ("spine", (1.0, 0, 0.8)),
                ("chest", (-1.8, 0.5, -0.5)),
                ("head", (0.7, 1.6, -0.4)),
            ]:
                track(
                    bone,
                    [
                        (0, (0, 0, 0)),
                        (0.25, amp),
                        (0.55, (0, 0, 0)),
                        (0.80, tuple(-0.65 * a for a in amp)),
                        (1, (0, 0, 0)),
                    ],
                )
        elif name == "Wave":
            for bone, pose in [
                ("upperarm_R", (-14, 3, -72)),
                ("forearm_R", (-8, 5, -86)),
                ("chest", (0, -6, -3)),
                ("head", (2, 5, 4)),
            ]:
                track(
                    bone,
                    [
                        (0, (0, 0, 0)),
                        (0.18, tuple(0.74 * a for a in pose)),
                        (0.29, pose),
                        (0.74, pose),
                        (0.9, tuple(0.35 * a for a in pose)),
                        (1, (0, 0, 0)),
                    ],
                )
            track(
                "hand_R",
                [
                    (0, (0, 0, 0)),
                    (0.28, (5, 65, 0)),
                    (0.36, (5, 72, -18)),
                    (0.46, (5, 57, 16)),
                    (0.55, (5, 72, -19)),
                    (0.65, (5, 57, 16)),
                    (0.74, (5, 65, -10)),
                    (1, (0, 0, 0)),
                ],
            )
        else:
            for bone, pose in [
                ("upperarm_L", (-52, 0, 21)),
                ("forearm_L", (-57, 0, 0)),
                ("hand_L", (52, -8, -2)),
                ("chest", (2, 3, -3)),
            ]:
                track(
                    bone,
                    [
                        (0, (0, 0, 0)),
                        (0.14, tuple(0.16 * a for a in pose)),
                        (0.42, pose),
                        (0.7, tuple(0.99 * a for a in pose)),
                        (1, pose),
                    ],
                )
        if name == "LanternRaise":
            # The bail pivots through the fingers. Cancel the arm's world
            # rotation at each key so the lamp hangs vertically under gravity.
            rotations = []
            keys = [(0, 0), (0.14, 0.16), (0.42, 1), (0.7, 0.99), (1, 1)]
            for _, amount in keys:
                q = np.array([0.0, 0.0, 0.0, 1.0])
                for pose in ((2, 3, -3), (-52, 0, 21), (-57, 0, 0), (52, -8, -2)):
                    b = np.array(quaternion([angle * amount for angle in pose]))
                    xyz = q[3] * b[:3] + b[3] * q[:3] + np.cross(q[:3], b[:3])
                    q = np.append(xyz, q[3] * b[3] - np.dot(q[:3], b[:3]))
                rotations.append([*-q[:3], q[3]])
            times = file.accessor([t * length for t, _ in keys], "SCALAR", bounds=True)
            rots = file.accessor(rotations, "VEC4")
            channels.append(dict(sampler=len(samplers), target=dict(node=ids["lantern"], path="rotation")))
            samplers.append(dict(input=times, output=rots, interpolation="LINEAR"))
        moving = {c["target"]["node"] for c in channels}
        for bone in (
            "pelvis",
            "chest",
            "spine",
            "head",
            "upperarm_R",
            "forearm_R",
            "hand_R",
            "upperarm_L",
            "forearm_L",
            "hand_L",
            "lantern",
        ):
            if ids[bone] not in moving:
                track(bone, [(0, (0, 0, 0)), (1, (0, 0, 0))])
        animations.append(dict(name=name, samplers=samplers, channels=channels))
    file.doc["animations"] = animations


def export_body(path, lod):
    surface = Surface(lod)
    face(surface, lod)
    body(surface, lod)
    file = Gltf()
    joints = bones()
    names = list(joints)
    file.mesh(surface, PALETTE, names)
    dress_materials(file)
    nodes = file.doc["nodes"]
    nodes.append(dict(name="KeeperBody", mesh=0, skin=0))
    ids = {name: 1 + i for i, name in enumerate(names)}
    matrices = []
    for name, (parent, point) in joints.items():
        position = np.array(point, dtype=float)
        local = position - np.array(joints[parent][1]) if parent else position
        nodes.append(dict(name=name, translation=local.tolist()))
        if parent:
            nodes[ids[parent]].setdefault("children", []).append(ids[name])
        inverse = np.identity(4)
        inverse[:3, 3] = -position
        matrices.append(inverse.T.flatten())
    file.doc["scenes"][0]["nodes"].append(ids["root"])
    file.doc["skins"] = [
        dict(
            name="KeeperRig",
            skeleton=ids["root"],
            joints=list(ids.values()),
            inverseBindMatrices=file.accessor(matrices, "MAT4"),
        )
    ]
    animate(file, ids)
    file.write(path)
    return sum(file.doc["accessors"][p["indices"]]["count"] // 3 for p in file.doc["meshes"][0]["primitives"])


def export_eye(out):
    surface = Surface()
    surface.oval("KeeperSclera", (0, 0, 0), (1, 1, 1), "head", 64, 40)
    surface.uvs = [(0.5 + 0.5 * x, 0.5 - 0.5 * y) for x, y, _ in surface.points]
    file = Gltf()
    file.mesh(surface, PALETTE)
    file.doc["nodes"] = [dict(name="KeeperEyeball", mesh=0)]
    file.doc.update(images=[dict(uri="KeeperIrisColor.png")], textures=[dict(source=0)])
    pbr = file.doc["materials"][0]["pbrMetallicRoughness"]
    pbr.update(baseColorFactor=[1, 1, 1, 1], baseColorTexture=dict(index=0))
    file.write(out / "KeeperEyeball.gltf")
    # Portable PNG writer: Blender's bundled Python does not include Pillow.
    from struct import pack
    from zlib import compress, crc32

    size = 512
    axis = np.linspace(-1, 1, size)
    x, y = np.meshgrid(axis, axis)
    a, r = np.arctan2(y, x), np.hypot(x, y) / 0.48
    fibres = (
        0.48
        + 0.14 * np.sin(109 * a + 7 * r)
        + 0.11 * np.sin(73 * a - 8 * r)
        + 0.09 * np.sin(147 * a + np.sin(24 * a))
    )
    pigment = np.array([0.18, 0.235, 0.19]) + fibres[..., None] * [0.23, 0.20, 0.14]
    pigment *= (1 - 0.47 * np.exp(-(((r - 0.95) / 0.060) ** 2)))[..., None]
    pigment *= (1 - 0.87 * np.clip((0.39 - r) / 0.025, 0, 1))[..., None]
    rgba = np.where((r < 1)[..., None], pigment, np.array([0.84, 0.79, 0.72]))
    data = np.uint8(np.clip(rgba * 255, 0, 255))

    def chunk(tag, data):
        return pack(">I", len(data)) + tag + data + pack(">I", crc32(tag + data))

    (out / "KeeperIrisColor.png").write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", compress(b"".join(b"\x00" + row.tobytes() for row in data)))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out", nargs="?", type=Path, default=HERE)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else None)
    sys.path.insert(0, str(HERE.parent / "DriftLighthouse"))
    from lighthouse_package import create_staging_directory, publish_package

    with create_staging_directory() as staging:
        staged = Path(staging)
        generate(staged)
        files = sorted(staged.iterdir(), key=lambda p: (p.suffix in (".gltf", ".json"), p.name))
        publish_package(staged, args.out, [p.name for p in files])


def generate(destination):
    bake(destination, PALETTE)
    lods = []
    for i, (name, max_dist) in enumerate(zip(LEVELS, [18, 78, 1000])):
        filename = name + ".gltf"
        count = export_body(destination / filename, i)
        lods.append(dict(source=filename, maxDistance=max_dist, triangles=count))
        print(name, count, "triangles")
    export_eye(destination)
    keeper_foundation.export(destination)
    profiles = {
        name: "Assets/Models/DriftKeeper/" + target + ".oloskin"
        for name, target in [
            ("KeeperSkin", "KeeperFace"),
            ("KeeperEar", "KeeperFace"),
            ("KeeperEye", "KeeperEye"),
            ("KeeperLip", "KeeperLip"),
            ("KeeperGum", "KeeperGum"),
            ("KeeperTongue", "KeeperTongue"),
            ("KeeperTeeth", "KeeperTeeth"),
        ]
    }
    rig = dict(
        clips=CLIPS,
        morphs=MORPHS,
        eyes=EYES,
        eyeRadius=EYE_R,
        lods=lods,
        profiles=profiles,
        materials=list(PALETTE),
        groomSource="Keeper.abc",
        # Second tread, clear of the door sill and handrail through both gestures.
        placement=dict(translation=[1.436795, -0.082, -2.888602], yawRadians=radians(162)),
    )
    (destination / "Keeper.rig.json").write_text(json.dumps(rig, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
