"""Mesh builder for the keeper. Coordinates throughout are glTF metres, Y up."""

from collections import defaultdict
from math import cos, sin, pi
import json
from pathlib import Path

import numpy as np

TAU = 2 * pi
MORPHS = ["BlinkL", "BlinkR", "JawOpen", "Smile", "BrowRaise", "Squint", "AA", "EE", "OO"]


class Surface:
    def __init__(self, lod=0):
        self.detail = (1, 0.64, 0.35)[lod]
        self.points = []
        self.uvs = []
        self.bones = []
        self.shapes = defaultdict(dict)
        self.faces = defaultdict(list)

    def vertex(self, point, uv, bone):
        self.points.append(tuple(point))
        self.uvs.append(tuple(uv))
        self.bones.append({bone: 1} if isinstance(bone, str) else bone)
        return len(self.points) - 1

    def quad(self, material, a, b, c, d):
        self.faces[material].extend(((a, b, c), (a, c, d)))

    def grid(self, material, cols, rows, sample, bone, invert=False):
        """A wrapped section from bottom to top, with a split UV seam."""
        start = len(self.points)
        for j in range(rows + 1):
            for i in range(cols + 1):
                u, v = i / cols, j / rows
                p = sample(TAU * u, v)
                weights = bone(p) if callable(bone) else bone
                self.vertex(p, (u, 1 - v), weights)
                if i and j:
                    d = len(self.points) - 1
                    a, b, c = d - cols - 2, d - cols - 1, d - 1
                    if invert:
                        self.quad(material, a, c, d, b)
                    else:
                        self.quad(material, a, b, d, c)
        return range(start, len(self.points))

    def loft(self, material, rings, bone, sides, fluting=None):
        """Ring records contain height, centre x/z and half-width/depth."""

        def sample(a, v):
            idx = round(v * (len(rings) - 1))
            y, x, z, rx, rz = rings[idx]
            d = fluting(a, y) if fluting else 0
            return (x + (rx + d) * sin(a), y, z + (rz + d) * cos(a))

        return self.grid(material, sides, len(rings) - 1, sample, bone)

    def oval(self, material, centre, radii, bone, segments=20, rings=12):
        segments = max(6, round(segments * self.detail))
        rings = max(4, round(rings * self.detail))

        def sample(a, v):
            lat = (v - 0.5) * pi
            return (
                centre[0] + radii[0] * cos(lat) * sin(a),
                centre[1] + radii[1] * sin(lat),
                centre[2] + radii[2] * cos(lat) * cos(a),
            )

        return self.grid(material, segments, rings, sample, bone)

    def tube(self, material, path, radii, bone, sides=10):
        """Sweep variable elliptical rings along an arbitrary centreline."""
        sides = max(5, round(sides * self.detail))
        p = np.array(path, dtype=float)
        tangent = np.gradient(p, axis=0)
        tangent /= np.linalg.norm(tangent, axis=1)[:, None]
        ref = (0, 0, 1) if abs(tangent[0, 2]) < 0.85 else (1, 0, 0)
        east = np.cross(tangent, ref)
        east /= np.maximum(np.linalg.norm(east, axis=1)[:, None], 1e-8)
        north = np.cross(east, tangent)

        def sample(a, v):
            i = round(v * (len(p) - 1))
            rx, rz = radii[i]
            return p[i] + rx * sin(a) * east[i] + rz * cos(a) * north[i]

        return self.grid(material, sides, len(p) - 1, sample, bone)


class Gltf:
    """Separate little-endian buffer, shared vertex streams and per-material primitives."""

    def __init__(self):
        self.binary = bytearray()
        self.doc = dict(
            asset=dict(version="2.0", generator="Stacks Light keeper"),
            scene=0,
            scenes=[dict(nodes=[0])],
            nodes=[],
            meshes=[],
            buffers=[],
            bufferViews=[],
            accessors=[],
            materials=[],
        )

    def accessor(self, values, kind, dtype="<f4", bounds=False):
        data = np.ascontiguousarray(values, dtype=dtype)
        if dtype == "<f4":
            # Blender flushes subnormal floats to zero. Canonicalize vanishing
            # expression deltas so standalone NumPy writes the same buffer.
            data = np.where(np.abs(data) < np.finfo(np.float32).tiny, np.float32(0), data)
        self.binary.extend(bytes((-len(self.binary)) % 4))
        offset = len(self.binary)
        self.binary.extend(data.tobytes())
        views, acc = self.doc["bufferViews"], self.doc["accessors"]
        desc = dict(
            bufferView=len(views),
            type=kind,
            count=len(data),
            componentType={"<f4": 5126, "<u4": 5125, "<u2": 5123}[dtype],
        )
        views.append(dict(buffer=0, byteOffset=offset, byteLength=data.nbytes))
        if bounds:
            flat = data.reshape(len(data), -1)
            desc.update(min=flat.min(axis=0).tolist(), max=flat.max(axis=0).tolist())
        acc.append(desc)
        return len(acc) - 1

    def mesh(self, surface, palette, skeleton=None, expressions=False):
        positions = np.asarray(surface.points, dtype=np.float32)
        normals = np.zeros_like(positions)
        nondegenerate = {}
        for material, triangles in surface.faces.items():
            tris = np.array(triangles)
            p = positions[tris]
            n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
            # A pole closes an oval with a triangle fan. Drop the collapsed
            # half-quads in *exported* precision before writing the index stream.
            keep = np.linalg.norm(n, axis=1) > 1e-12
            tris, n = tris[keep], n[keep]
            nondegenerate[material] = tris
            for column in tris.T:
                np.add.at(normals, column, n)
        # Coincident UV seams and poles must agree on the lighting normal.
        keys, inv = np.unique(np.round(positions, 7), axis=0, return_inverse=True)
        smooth = np.zeros_like(keys)
        np.add.at(smooth, inv, normals)
        normals = smooth[inv]
        lengths = np.linalg.norm(normals, axis=1)
        normals[lengths < 1e-14] = (0, 1, 0)
        normals /= np.linalg.norm(normals, axis=1)[:, None]
        primitives = []
        for material, triangles in nondegenerate.items():
            verts, local_indices = np.unique(triangles.ravel(), return_inverse=True)
            attr = dict(
                POSITION=self.accessor(positions[verts], "VEC3", bounds=True),
                NORMAL=self.accessor(normals[verts], "VEC3"),
                TEXCOORD_0=self.accessor(np.array(surface.uvs)[verts], "VEC2"),
            )
            color, rough, metal = palette[material]
            pbr = dict(baseColorFactor=[*color, 1], roughnessFactor=rough, metallicFactor=metal)
            primitive = dict(
                attributes=attr,
                material=len(self.doc["materials"]),
                indices=self.accessor(local_indices, "SCALAR", "<u4"),
            )
            self.doc["materials"].append(dict(name=material, pbrMetallicRoughness=pbr))
            if skeleton:
                joints = np.zeros((len(verts), 4), dtype="<u2")
                weights = np.zeros((len(verts), 4))
                for row, vertex in enumerate(verts):
                    for col, (name, weight) in enumerate(surface.bones[vertex].items()):
                        joints[row, col] = skeleton.index(name)
                        weights[row, col] = weight
                attr.update(
                    JOINTS_0=self.accessor(joints, "VEC4", "<u2"), WEIGHTS_0=self.accessor(weights, "VEC4")
                )
            if skeleton or expressions:
                # Share the zero stream within a primitive. Most garments have
                # no facial expressions, but glTF requires equal target counts.
                zero = None
                primitive["targets"] = []
                for name in MORPHS:
                    delta = np.array([surface.shapes[name].get(i, (0, 0, 0)) for i in verts])
                    if delta.any():
                        accessor = self.accessor(delta, "VEC3", bounds=True)
                    else:
                        if zero is None:
                            zero = self.accessor(delta, "VEC3", bounds=True)
                        accessor = zero
                    primitive["targets"].append(dict(POSITION=accessor))
            primitives.append(primitive)
        mesh = dict(name="Keeper" if skeleton else "KeeperEyeball", primitives=primitives)
        if skeleton or expressions:
            mesh.update(weights=[0.0] * len(MORPHS), extras=dict(targetNames=MORPHS))
        self.doc["meshes"].append(mesh)

    def write(self, path):
        path = Path(path)
        buffer = path.with_suffix(".bin")
        buffer.write_bytes(self.binary)
        self.doc["buffers"] = [dict(byteLength=len(self.binary), uri=buffer.name)]
        path.write_text(json.dumps(self.doc, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
