"""Bake PBR finishes from the two CC0 scans bundled with this model."""

import json
import struct
import tempfile
from pathlib import Path
from zipfile import ZipFile
import zlib

import bpy
import numpy as np


def png(path, rgb):
    """Write the Blender bottom-up RGB samples as a deterministic 8-bit PNG."""
    data = (np.clip(rgb[::-1], 0, 1) * 255 + 0.5).astype(np.uint8)
    height, width, _ = data.shape

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload))
        )

    # Subtract each pixel's left neighbour to compress fine grain efficiently.
    data[:, 1:] = data[:, 1:] - data[:, :-1]
    rows = b"".join(b"\x01" + row.tobytes() for row in data)
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b"")
    )


def scan(archive, scratch, family, channel, size):
    path = Path(scratch) / (channel + ".jpg")
    path.write_bytes(archive.read(f"{family}/{channel}.jpg"))
    image = bpy.data.images.load(str(path), check_existing=False)
    image.colorspace_settings.name = "Non-Color"
    if tuple(image.size) != (size, size):
        image.scale(size, size)
    pixels = np.empty(size * size * 4, np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(size, size, 4)[:, :, :3]


def bake(out):
    with (
        ZipFile(Path(__file__).with_name("SurfaceSources.zip")) as archive,
        tempfile.TemporaryDirectory() as scratch,
    ):

        def maps(family, size):
            return [
                scan(archive, scratch, family, channel, size) for channel in ("Diffuse", "nor_gl", "arm")
            ]

        color, normal, arm = maps("weathered_brown_planks", 2048)
        light = color.mean(axis=2)
        salt = np.clip((light - 0.21) * 1.3, 0, 0.45)[:, :, None]
        png(out / "QuayOakColor.png", (1 - salt) * color * 1.14 + salt * [0.65, 0.61, 0.53])
        png(out / "QuayOakNormal.png", normal)
        arm[:, :, 1] = 0.43 + 0.54 * arm[:, :, 1]
        arm[:, :, 2] = 0
        png(out / "QuayOakORM.png", arm)

        # Across the saw cut the oak exposes annual rings and radial checks.
        # Shift the pith within the chart per beam in the generator.
        v, u = np.mgrid[:512, :512] / 512
        x, y = u - 0.38, v - 0.64
        r = np.hypot(x * 1.06, y)
        phase = 2 * np.pi * (32 * r + 0.27 * np.sin(9 * x + 6 * y) + 0.10 * np.sin(36 * y) * np.sin(29 * x))
        latewood = np.clip(np.cos(phase) * 1.9 - 0.44, 0, 1)
        theta = np.arctan2(y, x)
        checks = np.exp(-95 * np.abs(np.sin(3 * theta + 0.24 * r + 0.05 * np.sin(29 * r)))) * np.clip(
            (r - 0.07) * 5, 0, 1
        )
        pores = np.random.default_rng(1717).normal(0, 0.022, r.shape)
        value = 0.56 - 0.084 * latewood - 0.19 * checks + pores
        png(out / "OakEndColor.png", value[:, :, None] * np.array([1.03, 0.85, 0.64]))
        slope_y, slope_x = np.gradient(0.24 * latewood + 0.45 * checks - pores)
        n = np.stack([slope_x, slope_y, np.ones_like(r)], axis=2)
        n /= np.linalg.norm(n, axis=2, keepdims=True)
        png(out / "OakEndNormal.png", 0.5 + 0.5 * n)
        png(
            out / "OakEndORM.png",
            np.stack([0.98 - 0.18 * checks, 0.80 + 0.09 * latewood, np.zeros_like(r)], axis=2),
        )

        color, normal, arm = maps("rusty_metal_03", 1024)
        light = color.mean(axis=2)
        pits = np.clip((0.245 - light) * 9 + (1 - arm[:, :, 0]) * 0.65, 0, 1)
        enamel = np.array([0.245, 0.386, 0.343]) * (0.79 + 0.48 * light[:, :, None])
        rust = np.array([0.375, 0.201, 0.111]) * (0.72 + light[:, :, None])
        png(out / "FoundryColor.png", enamel + (rust - enamel) * pits[:, :, None])
        normal[:, :, :2] = 0.5 + (normal[:, :, :2] - 0.5) * (0.32 + 0.55 * pits[:, :, None])
        normal[:, :, 2] = 0.5 + 0.5 * np.sqrt(
            np.clip(1 - ((normal[:, :, :2] * 2 - 1) ** 2).sum(axis=2), 0, 1)
        )
        png(out / "FoundryNormal.png", normal)
        arm[:, :, 1] = 0.38 + 0.32 * arm[:, :, 1] + 0.22 * pits
        arm[:, :, 2] = 0.43 * (1 - pits)
        png(out / "FoundryORM.png", arm)

        color, normal, arm = maps("rusty_metal_03", 512)
        grain = color.mean(axis=2)
        pores = np.clip((0.30 - grain) * 2.4, 0, 0.60)
        micro = np.random.default_rng(1854).normal(0, 0.012, grain.shape)
        relief = normal.copy()
        relief[:, :, :2] = 0.5 + 0.24 * (relief[:, :, :2] - 0.5)
        relief[:, :, 2] = 0.5 + 0.5 * np.sqrt(
            np.clip(1 - ((relief[:, :, :2] * 2 - 1) ** 2).sum(axis=2), 0, 1)
        )
        png(out / "MetalToolNormal.png", relief)
        # Oiled bar and rolled iron keep dark valleys and exposed metal on
        # the high spots. Yellow metal oxidises to brown at the same pits.
        for family, high, low, rough, metal in [
            ("WroughtIron", [0.40, 0.39, 0.35], [0.21, 0.17, 0.14], 0.41, 0.74),
            ("ToolSteel", [0.63, 0.62, 0.58], [0.32, 0.32, 0.29], 0.31, 0.87),
            ("WorkedBronze", [0.68, 0.51, 0.29], [0.31, 0.25, 0.17], 0.32, 0.79),
        ]:
            col = np.array(high) + (np.array(low) - high) * pores[:, :, None]
            png(out / (family + "Color.png"), col + (micro + 0.16 * (grain - 0.4))[:, :, None])
            png(
                out / (family + "ORM.png"),
                np.stack(
                    [
                        0.96 - 0.18 * pores,
                        rough + 0.20 * pores + 0.10 * (arm[:, :, 1] - 0.5),
                        metal - 0.24 * pores,
                    ],
                    axis=2,
                ),
            )

        # Six strands with finer wires wound into each crown. The chart on
        # the line follows the rope around both sheaves and onto the barrel.
        v, u = np.mgrid[:256, :256] / 256
        angle = 2 * np.pi * (u - v)
        crown = (0.5 + 0.5 * np.cos(6 * angle)) ** 0.55
        wire = 0.5 + 0.5 * np.cos(2 * np.pi * (30 * u - 37 * v))
        light = 0.29 + 0.24 * crown + 0.055 * wire * crown
        png(out / "RunningWireColor.png", light[:, :, None] * np.array([1.00, 0.95, 0.84]))
        height = 0.72 * crown + 0.10 * wire * crown
        dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 2.8
        dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 1.6
        n = np.stack([-dx, -dy, np.ones_like(dx)], axis=2)
        n /= np.linalg.norm(n, axis=2, keepdims=True)
        png(out / "RunningWireNormal.png", 0.5 + 0.5 * n)
        png(
            out / "RunningWireORM.png",
            np.stack([0.68 + 0.32 * crown, 0.58 - 0.23 * crown, 0.55 + 0.29 * crown], axis=2),
        )


def bind_maps(descriptor, surfaces):
    # Share external images between the two glTFs. PNGs are ordinary engine
    # texture assets, so the pack writer can resolve every imported map by path.
    doc = json.loads(descriptor.read_text())
    doc["images"] = []
    doc["textures"] = []
    indices = {}

    def texture(name):
        if name not in indices:
            index = len(doc["images"])
            doc["images"].append({"uri": name})
            doc["textures"].append({"source": index})
            indices[name] = index
        return {"index": indices[name]}

    for mat in doc["materials"]:
        surface = surfaces.get(mat["name"])
        if not surface:
            continue
        family, tint = surface
        pbr = mat["pbrMetallicRoughness"]
        pbr.update(
            baseColorFactor=[*tint, 1],
            metallicFactor=1,
            roughnessFactor=1,
            baseColorTexture=texture(family + "Color.png"),
            metallicRoughnessTexture=texture(family + "ORM.png"),
        )
        surface_normal = "MetalTool" if family in ("WroughtIron", "ToolSteel", "WorkedBronze") else family
        mat["normalTexture"] = texture(surface_normal + "Normal.png")
        mat["occlusionTexture"] = texture(family + "ORM.png")
    descriptor.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
