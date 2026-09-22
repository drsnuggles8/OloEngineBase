#!/usr/bin/env python3
"""Render an imported plant from several angles, so the import can be LOOKED at.

    python tools/vegetation-import/preview.py pine broadleaf
    python tools/vegetation-import/preview.py --all --out docs/images/vegetation

A coverage percentage cannot tell you whether a tree reads as a tree. Issue #1398
exists because the foliage was signed off on renderer measurements while the
image was wrong, so this tool is deliberately a picture and not a number: an
orthographic turntable with alpha testing and one directional light, drawn
straight from the committed OBJ, MTL and textures.

It is NOT a substitute for the engine. It shares no code with the renderer, so it
cannot confirm anything about shading, wind, LOD or the impostor bake - only that
the geometry, the material split and the cutout that landed on disk are the ones
intended. Engine-side verification is a separate step.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_MODELS = os.path.join(REPO_ROOT, "OloEditor", "SandboxProject", "Assets", "Models", "Vegetation")

LIGHT = np.array([0.4, 0.75, 0.5])
LIGHT = LIGHT / np.linalg.norm(LIGHT)
SKY = np.array([0.26, 0.30, 0.38])
GROUND = np.array([0.16, 0.15, 0.13])


def load_obj(path):
    """Return [(material, positions, normals, uvs, tris)] and {material: texture path}."""
    positions, uvs, normals = [], [], []
    # An OBJ need not declare a material at all - the generated stand-ins this
    # work replaces do not - and faces before the first `usemtl` are still
    # geometry. Starting with a default group rather than None is what stops
    # such a file loading as zero triangles and measuring as 0% coverage.
    groups, current, faces = [], "default", []
    mtl_path = None
    base = os.path.dirname(path)
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("v "):
                positions.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("vt "):
                uvs.append([float(x) for x in line.split()[1:3]])
            elif line.startswith("vn "):
                normals.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("mtllib "):
                mtl_path = os.path.join(base, line.split(None, 1)[1].strip())
            elif line.startswith("usemtl "):
                if faces:
                    groups.append((current, faces))
                current, faces = line.split(None, 1)[1].strip(), []
            elif line.startswith("f "):
                corner = []
                for token in line.split()[1:4]:
                    bits = token.split("/")
                    corner.append((int(bits[0]) - 1,
                                   int(bits[1]) - 1 if len(bits) > 1 and bits[1] else -1,
                                   int(bits[2]) - 1 if len(bits) > 2 and bits[2] else -1))
                faces.append(corner)
    if faces:
        groups.append((current, faces))

    positions = np.array(positions, np.float32)
    uvs = np.array(uvs, np.float32) if uvs else np.zeros((1, 2), np.float32)
    normals = np.array(normals, np.float32) if normals else np.zeros((1, 3), np.float32)

    textures = {}
    if mtl_path and os.path.exists(mtl_path):
        name = None
        for line in open(mtl_path, "r", encoding="utf-8"):
            if line.startswith("newmtl "):
                name = line.split(None, 1)[1].strip()
            elif line.startswith("map_Kd ") and name:
                textures[name] = os.path.join(base, line.split(None, 1)[1].strip().replace("/", os.sep))

    out = []
    for material, faces in groups:
        index = np.array(faces, np.int64)                    # (F, 3, 3)
        out.append((material, positions[index[..., 0]], normals[index[..., 2]], uvs[index[..., 1]]))
    return out, textures


def render(groups, textures, size, azimuth, elevation, cutoff=0.5, background=(0.10, 0.11, 0.14)):
    colour = np.tile(np.array(background, np.float32), (size, size, 1))
    zbuffer = np.full((size, size), -np.inf, np.float32)

    ca, sa = np.cos(azimuth), np.sin(azimuth)
    ce, se = np.cos(elevation), np.sin(elevation)
    forward = np.array([ce * sa, se, ce * ca])
    right = np.array([ca, 0.0, -sa])
    up = np.cross(forward, right)

    everything = np.concatenate([g[1].reshape(-1, 3) for g in groups])
    centre = np.array([0.0, 0.5, 0.0])
    radius = float(np.abs(everything - centre).max()) * 1.12

    for material, tri_p, tri_n, tri_uv in groups:
        texture = None
        if material in textures and os.path.exists(textures[material]):
            texture = np.asarray(Image.open(textures[material]).convert("RGBA"), np.float32) / 255.0
        flat = tri_p.reshape(-1, 3) - centre
        x = (flat @ right) / radius * 0.5 + 0.5
        y = (flat @ up) / radius * 0.5 + 0.5
        depth = -(flat @ forward)
        px = np.stack([x * size, (1.0 - y) * size], -1).reshape(-1, 3, 2)
        depth = depth.reshape(-1, 3)
        lambert = np.clip(tri_n.reshape(-1, 3) @ LIGHT, -1.0, 1.0).reshape(-1, 3)

        for t in range(len(px)):
            p = px[t]
            x0 = max(int(np.floor(p[:, 0].min())), 0)
            x1 = min(int(np.ceil(p[:, 0].max())) + 1, size)
            y0 = max(int(np.floor(p[:, 1].min())), 0)
            y1 = min(int(np.ceil(p[:, 1].max())) + 1, size)
            if x1 <= x0 or y1 <= y0:
                continue
            d = (p[1, 1] - p[2, 1]) * (p[0, 0] - p[2, 0]) + (p[2, 0] - p[1, 0]) * (p[0, 1] - p[2, 1])
            if abs(d) < 1e-12:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            b0 = ((p[1, 1] - p[2, 1]) * (gx - p[2, 0]) + (p[2, 0] - p[1, 0]) * (gy - p[2, 1])) / d
            b1 = ((p[2, 1] - p[0, 1]) * (gx - p[2, 0]) + (p[0, 0] - p[2, 0]) * (gy - p[2, 1])) / d
            b2 = 1.0 - b0 - b1
            mask = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
            if not mask.any():
                continue
            z = b0 * depth[t, 0] + b1 * depth[t, 1] + b2 * depth[t, 2]
            window = zbuffer[y0:y1, x0:x1]
            mask &= z > window
            if not mask.any():
                continue
            if texture is not None:
                u = b0 * tri_uv[t, 0, 0] + b1 * tri_uv[t, 1, 0] + b2 * tri_uv[t, 2, 0]
                v = b0 * tri_uv[t, 0, 1] + b1 * tri_uv[t, 1, 1] + b2 * tri_uv[t, 2, 1]
                th, tw = texture.shape[:2]
                # These OBJs carry top-down (glTF) v, as the engine samples it.
                sx = np.clip((u % 1.0) * (tw - 1), 0, tw - 1).astype(np.int32)
                sy = np.clip((v % 1.0) * (th - 1), 0, th - 1).astype(np.int32)
                texel = texture[sy, sx]
                mask &= texel[..., 3] >= cutoff
                if not mask.any():
                    continue
                albedo = texel[..., :3]
            else:
                albedo = np.full(mask.shape + (3,), 0.5, np.float32)

            shade = b0 * lambert[t, 0] + b1 * lambert[t, 1] + b2 * lambert[t, 2]
            # Two-sided, as foliage is drawn: the sign of the normal does not
            # decide whether a leaf is lit, only how much.
            intensity = np.abs(shade)[..., None]
            hemi = (0.5 + 0.5 * np.sign(shade)[..., None]) * SKY + (0.5 - 0.5 * np.sign(shade)[..., None]) * GROUND
            lit = albedo * (0.25 * hemi + 1.15 * intensity)
            window[mask] = z[mask]
            colour[y0:y1, x0:x1][mask] = np.clip(lit, 0.0, 1.0)[mask]

    # A gentle filmic-ish curve, so a dark conifer albedo is not read as black.
    return np.clip(colour, 0, 1) ** (1.0 / 1.6)


def contact_sheet(name, model_dir, size, angles, out_dir, cutoff=0.5):
    obj = os.path.join(model_dir, f"{name}.obj")
    groups, textures = load_obj(obj)
    tiles = []
    for azimuth, elevation, label in angles:
        image = render(groups, textures, size, np.radians(azimuth), np.radians(elevation), cutoff)
        tiles.append((label, (image * 255).astype(np.uint8)))
    sheet = Image.new("RGB", (size * len(tiles), size), (18, 18, 22))
    for i, (_, tile) in enumerate(tiles):
        sheet.paste(Image.fromarray(tile), (i * size, 0))
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{name}_preview.png")
    sheet.save(path)
    total = sum(len(g[1]) for g in groups)
    print(f"  {name:11} {total:>7,} tris, {len(groups)} submesh(es) -> {path}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("species", nargs="*")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--models", default=DEFAULT_MODELS)
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "docs", "images", "vegetation"))
    parser.add_argument("--size", type=int, default=420)
    parser.add_argument("--cutoff", type=float, default=0.5)
    args = parser.parse_args()

    names = args.species
    if args.all or not names:
        names = sorted(d for d in os.listdir(args.models)
                       if os.path.isdir(os.path.join(args.models, d))
                       and os.path.exists(os.path.join(args.models, d, f"{d}.obj")))

    angles = [(0, 8, "front"), (55, 14, "three-quarter"), (140, 4, "side"), (250, 28, "high")]
    for name in names:
        contact_sheet(name, os.path.join(args.models, name), args.size, angles, args.out, args.cutoff)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
