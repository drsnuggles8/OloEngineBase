#!/usr/bin/env python3
"""Generate the committed #1013 recording workload, with 3600 distinct meshes.

Distinct position accessors and material factors preserve separate meshes through
Assimp's PreTransformVertices pass. Each imported mesh gets its own index window,
so the shadow caster key cannot collapse the workload into one instanced cube.
No external assets, random state, timestamps or third-party packages are needed.
"""

import json
import pathlib
import struct

from generate_material_lab_scene import SceneWriter, camera, mesh, material
from generate_courtyard_scene import model, point_light, spot_light

ROOT = pathlib.Path(__file__).resolve().parents[3]
ASSETS = ROOT / "OloEditor" / "SandboxProject" / "Assets"
GRID = 60


def make_model():
    document = {
        "asset": {"version": "2.0", "generator": "OloEngine recording benchmark #1013"},
        "scene": 0, "scenes": [{"nodes": list(range(GRID * GRID))}],
        "nodes": [], "meshes": [], "materials": [],
        "accessors": [], "bufferViews": [], "buffers": [],
    }
    binary = bytearray()

    def accessor(values, kind, components, component_type, target, bounds=False):
        while len(binary) % 4:
            binary.append(0)
        start = len(binary)
        fmt = "f" if component_type == 5126 else "H"
        binary.extend(struct.pack("<" + fmt * len(values), *values))
        view = len(document["bufferViews"])
        document["bufferViews"].append({
            "buffer": 0, "byteOffset": start, "byteLength": len(binary) - start,
            "target": target,
        })
        result = {"bufferView": view, "componentType": component_type,
                  "count": len(values) // components, "type": kind}
        if bounds:
            result["min"] = [min(values[i::components]) for i in range(components)]
            result["max"] = [max(values[i::components]) for i in range(components)]
        index = len(document["accessors"])
        document["accessors"].append(result)
        return index

    # Counter-clockwise faces with explicit face normals.
    faces = [
        ((1, 0, 0), [(1, 0, 1), (1, 0, -1), (1, 1, -1), (1, 1, 1)]),
        ((-1, 0, 0), [(-1, 0, -1), (-1, 0, 1), (-1, 1, 1), (-1, 1, -1)]),
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]),
        ((0, 0, 1), [(-1, 0, 1), (1, 0, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(1, 0, -1), (-1, 0, -1), (-1, 1, -1), (1, 1, -1)]),
    ]
    normals = accessor([c for normal, _ in faces for _ in range(4) for c in normal],
                       "VEC3", 3, 5126, 34962)
    indices = accessor([4 * face + index for face in range(6) for index in (0, 1, 2, 0, 2, 3)],
                       "SCALAR", 1, 5123, 34963)
    for index in range(GRID * GRID):
        x, z = index % GRID, index // GRID
        height = 0.7 + ((x * 17 + z * 31) % 19) * 0.12
        center_x, center_z = (x - (GRID - 1) / 2) * 1.5, (z - (GRID - 1) / 2) * 1.5
        positions = [c for _, corners in faces for px, py, pz in corners
                     for c in (center_x + px * 0.42, py * height, center_z + pz * 0.42)]
        position = accessor(positions, "VEC3", 3, 5126, 34962, bounds=True)
        document["nodes"].append({"name": f"Caster{index:04}", "mesh": index})
        document["meshes"].append({"name": f"Caster{index:04}", "primitives": [{
            "attributes": {"POSITION": position, "NORMAL": normals},
            "indices": indices, "material": index,
        }]})
        document["materials"].append({
            "name": f"Unique{index:04}",
            "pbrMetallicRoughness": {"baseColorFactor": [0.2 + index / 16384, 0.36, 0.52, 1],
                                    "metallicFactor": 0, "roughnessFactor": 0.8},
        })
    document["buffers"] = [{"byteLength": len(binary)}]
    encoded = json.dumps(document, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
            + struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
            + struct.pack("<II", len(binary), 0x004E4942) + binary)


def make_scene():
    scene = SceneWriter("ParallelRecording", 1013_000_001_000_000)
    scene.raw("Entities:\n")
    scene.entity("BenchmarkCamera", (0, 50, 85), (-0.61, 0, 0), components=camera())
    scene.entity("UniqueShadowCasters3600", components=model("SandboxProject/Assets/Models/Benchmark/RecordingCasters.glb"))
    scene.entity("Ground", (0, -0.15, 0), scale=(110, 0.2, 110),
                 components=mesh(1) + material((0.42, 0.42, 0.42), 0, 0.9))
    scene.entity("Sun", components=(
        "    DirectionalLightComponent:\n"
        "      Direction: [-0.6, -1, -0.3]\n"
        "      Color: [1, 1, 1]\n"
        "      Intensity: 3\n"
        "      CastShadows: true\n"
        "      ShadowBias: 0.0015\n"
        "      ShadowNormalBias: 0.1\n"
        "      MaxShadowDistance: 180\n"
        "      CascadeSplitLambda: 0.8\n"))
    for i, x in enumerate((-24, 24)):
        scene.entity(f"PointShadow{i}", (x, 7, -12),
                     components=point_light((1, 0.8, 0.55), 35, 38, True))
        scene.entity(f"SpotShadow{i}", (x, 18, 28),
                     components=spot_light((0, -1, -0.25), (0.55, 0.75, 1), 45, 65, 32, 48, True))
    return scene.text()


if __name__ == "__main__":
    model_path = ASSETS / "Models" / "Benchmark" / "RecordingCasters.glb"
    scene_path = ASSETS / "Scenes" / "Benchmark" / "ParallelRecording.olo"
    model_path.parent.mkdir(parents=True, exist_ok=True)
    scene_path.parent.mkdir(parents=True, exist_ok=True)
    model_path.write_bytes(make_model())
    scene_path.write_text(make_scene(), encoding="utf-8", newline="\n")
    print(f"Wrote {GRID * GRID} unique caster meshes: {model_path.stat().st_size} bytes")
