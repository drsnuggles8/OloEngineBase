"""Generate the procedural glTF hit and mask scenes for CAP-04 verification."""

import argparse
import copy
import io
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image
import yaml


EDITOR = Path(__file__).resolve().parents[3] / "OloEditor"
SOURCE = EDITOR / "SandboxProject/Assets/Scenes/ReflectionHierarchyTest.olo"
VISUAL_ROOT = EDITOR / "assets/tests/visual"
ARMS = (
    "off", "on", "tier", "midrough", "gateend", "textured-a", "textured-b", "masked",
    "reference-a", "reference-b", "textured-albedo", "textured-normal", "textured-emissive",
    "textured-mr", "textured-motion", "emitter-a", "emitter-b", "reference-emitter-a",
    "reference-emitter-b",
)


def write_yaml(path, data):
    path.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")


def png(array):
    stream = io.BytesIO()
    Image.fromarray(array).save(stream, format="PNG")
    return stream.getvalue()


def make_glb(path, alternate_maps=(), is_mask=False, constant_emissive=None, albedo_only=False):
    data = bytearray()
    views = []
    accessors = []

    def view(blob, target=None):
        offset = len(data)
        data.extend(blob)
        data.extend(b"\0" * (-len(data) % 4))
        item = {"buffer": 0, "byteOffset": offset, "byteLength": len(blob)}
        if target:
            item["target"] = target
        views.append(item)
        return len(views) - 1

    def accessor(arr, component, format_name, target, bounds=False):
        obj = {
            "bufferView": view(arr.tobytes(), target),
            "componentType": component,
            "count": len(arr),
            "type": format_name,
        }
        if bounds:
            obj.update(min=arr.min(axis=0).tolist(), max=arr.max(axis=0).tolist())
        accessors.append(obj)
        return len(accessors) - 1
    normal = accessor(np.array([(0, 0, 1)] * 4, dtype="<f4"), 5126, "VEC3", 34962)
    uv = accessor(np.array([(0, 1), (1, 1), (1, 0), (0, 0)], dtype="<f4"), 5126, "VEC2", 34962)
    indices = accessor(np.array([0, 1, 2, 0, 2, 3], dtype="<u2"), 5123, "SCALAR", 34963)
    primitives = []
    materials = []
    images = []
    textures = []

    def image(array):
        images.append({"bufferView": view(png(array)), "mimeType": "image/png"})
        textures.append({"source": len(images) - 1, "sampler": 0})
        return {"index": len(images) - 1}

    y, x = np.indices((256, 256))
    region = ((x // 32 + y // 32) % 2) != 0
    checker = ((x // 2 + y // 2) % 2) != 0
    albedo = np.where(
        (region ^ ("albedo" in alternate_maps))[..., None], [235, 45, 12, 255], [40, 170, 230, 255]
    ).astype("uint8")
    albedo[:, :, :3] = np.where(checker[..., None], albedo[:, :, :3] // 3, albedo[:, :, :3])
    normals = np.where(
        (region ^ ("normal" in alternate_maps))[..., None], [65, 182, 220], [198, 81, 225]
    ).astype("uint8")
    emission = np.where(
        (region ^ ("emissive" in alternate_maps))[..., None], [20, 150, 240], [170, 20, 65]
    ).astype("uint8")
    metal_rough = np.where(
        (region ^ ("mr" in alternate_maps))[..., None], [255, 96, 180], [255, 220, 30]
    ).astype("uint8")
    if constant_emissive is not None:
        emission[:] = constant_emissive
    if is_mask:
        albedo[:] = [247, 208, 35, 255]
        albedo[:, :, 3] = np.where((x // 32) % 2 == 0, 0, 255)

    quad_spans = [(-1, 1)] if is_mask else [(-1, 0), (0, 1)]
    for material_index, (x0, x1) in enumerate(quad_spans):
        positions = accessor(
            np.array([(x0, -1, 0), (x1, -1, 0), (x1, 1, 0), (x0, 1, 0)], dtype="<f4"),
            5126, "VEC3", 34962, True,
        )
        primitives.append({
            "attributes": {"POSITION": positions, "NORMAL": normal, "TEXCOORD_0": uv},
            "indices": indices,
            "material": material_index,
        })
        mat = {
            "name": "MaskedStripe" if is_mask else f"TextureTile{material_index}",
            "doubleSided": False,
            "pbrMetallicRoughness": {
                "baseColorFactor": [.8, .8, .8, 1],
                "metallicFactor": 0.0 if albedo_only else .65,
                "roughnessFactor": 1.0 if albedo_only else .7,
                "baseColorTexture": image(albedo if material_index == 0 else albedo[:, ::-1]),
            },
        }
        if is_mask:
            mat.update(alphaMode="MASK", alphaCutoff=.5)
        elif not albedo_only:
            mat["pbrMetallicRoughness"]["metallicRoughnessTexture"] = image(metal_rough)
            mat["normalTexture"] = {**image(normals), "scale":1.0}
            mat["emissiveTexture"] = image(emission if material_index == 0 else emission[:, ::-1])
            mat["emissiveFactor"] = [.32, .32, .32]
        materials.append(mat)
    root = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}], "meshes": [{"primitives": primitives}],
        "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(data)}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "textures": textures, "images": images, "materials": materials,
    }
    chunk = json.dumps(root, separators=(",", ":")).encode()
    chunk += b" " * (-len(chunk) % 4)
    path.write_bytes(
        struct.pack("<III", 0x46546c67, 2, 28 + len(chunk) + len(data))
        + struct.pack("<II", len(chunk), 0x4e4f534a) + chunk
        + struct.pack("<II", len(data), 0x004e4942) + data
    )

def make_scenes(rendering_path, destination, arms, sky_only=False):
    destination.mkdir(parents=True, exist_ok=True)
    base = yaml.safe_load(SOURCE.read_text(encoding="utf-8"))
    base["PostProcessSettings"].update(FXAAEnabled=False)
    base["RenderingPath"] = rendering_path
    scene_suffix = "" if rendering_path == "Deferred" else f"-{rendering_path.lower()}"
    if sky_only:
        base["Entities"] = [e for e in base["Entities"] if "DirectionalLightComponent" not in e]
        scene_suffix += "-sky-only"
    map_controls = {f"textured-{name}": (name,) for name in ("albedo", "normal", "emissive", "mr")}
    aggregate = ("albedo", "normal", "emissive", "mr")
    map_controls.update({"textured-b": aggregate, "reference-b": aggregate})
    emission_controls = {
        "emitter-a": (16, 64, 192), "emitter-b": (192, 64, 16),
        "reference-emitter-a": (16, 64, 192), "reference-emitter-b": (192, 64, 16),
    }
    for arm in arms:
        scene = copy.deepcopy(base)
        scene["Scene"] = f"HybridReflectionEvidence-{arm}{scene_suffix}.olo"
        scene["PostProcessSettings"]["RTReflectionEnabled"] = arm != "off" and not arm.startswith("reference-")
        if arm == "tier":
            scene["PostProcessSettings"]["RTReflectionTierDebugView"] = True
        if arm in ("midrough", "gateend"):
            floor = next(e for e in scene["Entities"] if e["TagComponent"]["Tag"] == "MirrorFloor")
            floor["MaterialComponent"]["Roughness"] = .175 if arm == "midrough" else .31
        if arm.startswith(("textured-", "reference-", "emitter-")) or arm == "masked":
            mesh = destination / f"{arm}.glb"
            make_glb(mesh, alternate_maps=map_controls.get(arm, ()),
                     constant_emissive=emission_controls.get(arm), albedo_only=arm == "textured-motion")
            target = next(e for e in scene["Entities"] if e["TagComponent"]["Tag"] == "RedPillarBehind")
            del target["MeshComponent"]
            del target["MaterialComponent"]
            target["TransformComponent"] = {
                "Translation": [-2.35, 8.1, -9], "Scale": [2.15, 1.65, 1], "Rotation": [0.0, 0.15, 0.18],
            }
            target["ModelComponent"] = {"FilePath": mesh.as_posix(), "Visible": True}
            if arm == "masked":
                mask = destination / "alpha-card.glb"
                make_glb(mask, is_mask=True)
                scene["Entities"].append({
                    "Entity": 820000000000010,
                    "TagComponent": {"Tag": "MaskedForegroundCard"},
                    "TransformComponent": {
                        "Translation": [-2.15, 7.8, -8.15], "Scale": [2.0, 1.55, 1], "Rotation": [0.0, 0.15, 0.18],
                    },
                    "ModelComponent": {"FilePath": mask.as_posix(), "Visible": True},
                })
        scene_path = destination / f"{arm}{scene_suffix}.olo"
        write_yaml(scene_path, scene)
        yield scene_path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, default=VISUAL_ROOT / "HybridReflectionFixture1355")
    parser.add_argument("--path", choices=("Forward", "ForwardPlus", "Deferred"), default="Deferred")
    parser.add_argument("--sky-only", action="store_true")
    parser.add_argument("--arms", nargs="+", choices=ARMS, default=ARMS)
    args = parser.parse_args()

    destination = args.out_dir.resolve()
    if not destination.is_relative_to(VISUAL_ROOT.resolve()):
        parser.error("choose an output directory under OloEditor/assets/tests/visual")
    for scene in make_scenes(args.path, destination, args.arms, args.sky_only):
        print(scene)


if __name__ == "__main__":
    main()
