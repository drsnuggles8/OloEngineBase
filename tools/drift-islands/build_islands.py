#!/usr/bin/env python3
"""Rebuild the Drift islands' terrain maps (issue #1721).

    python tools/drift-islands/build_islands.py                  # all six
    python tools/drift-islands/build_islands.py Stacks Mesa      # some
    python tools/drift-islands/build_islands.py --preview out/   # also write preview PNGs
    python tools/drift-islands/build_islands.py --check          # regenerate and compare, write nothing
    python tools/drift-islands/build_islands.py --layers         # rebuild the shared layer textures too
    python tools/drift-islands/build_islands.py --stamp-scene    # rewrite the island entities in Drift.olo

Per island it writes, under OloEditor/SandboxProject/Assets/Terrain/DriftIslands/:

    <Island>_Height.png   16-bit grey, 1024 x 1024, 0 = the tile floor (24 m under the sea)
    <Island>_Splat0.png   RGBA8 weights of layers 0-3 (Seabed, Sand, Shingle, Turf)
    <Island>_Splat1.png   RGBA8 weights of layers 4-7 (Heath, Rock, RockBedded, Scree)
    <Island>_Macro.png    RGBA8 colour multiplier (128 = neutral) and dryness in alpha

Image row 0 is the tile's z = 0 edge and column 0 its x = 0 edge, the order the engine reads a
heightmap in. Needs numpy, scipy and Pillow (requirements.txt). The output is deterministic: the
same commit of this tool writes the same bytes.
"""
import argparse
import hashlib
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from olo_islands import layers, pipeline, preview, recipes, scene  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, "OloEditor", "SandboxProject", "Assets", "Terrain", "DriftIslands")
SCENE = os.path.join(REPO, "OloEditor", "SandboxProject", "Assets", "Scenes", "Drift.olo")


def height_from_png(out_dir, name):
    """Metres above the sea, read back from the committed height map."""
    _, height_scale, _, _, _ = recipes.TILES[name]
    h16 = np.asarray(Image.open(os.path.join(out_dir, f"{name}_Height.png")), dtype=np.float64)
    return h16 / 65535.0 * height_scale - 24.0


def encode(result):
    """name -> PIL image, for the four maps of one island."""
    h16 = np.round(result["height01"] * 65535.0).astype(np.uint16)
    name = result["name"]
    return {
        f"{name}_Height.png": Image.fromarray(h16),
        f"{name}_Splat0.png": Image.fromarray(result["splat0"]),
        f"{name}_Splat1.png": Image.fromarray(result["splat1"]),
        f"{name}_Macro.png": Image.fromarray(result["macro"]),
    }


def pixel_digest(img):
    return hashlib.sha256(np.asarray(img).tobytes()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("islands", nargs="*", help="island names (default: all)")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--preview", default="", help="directory for preview PNGs (not written by default)")
    ap.add_argument("--check", action="store_true", help="compare against the files on disk; exit 1 on any difference")
    ap.add_argument("--resolution", type=int, default=pipeline.RESOLUTION)
    ap.add_argument("--layers", action="store_true", help="rebuild Layers/ from the Poly Haven scans (downloads on first use)")
    ap.add_argument("--only-layers", action="store_true", help="rebuild Layers/ and stop")
    ap.add_argument("--refresh-sources", action="store_true", help="re-query Poly Haven and rewrite sources.json")
    ap.add_argument("--stamp-scene", action="store_true", help="rewrite the island entities in Drift.olo from the maps on disk")
    ap.add_argument("--only-stamp", action="store_true", help="stamp the scene from the maps on disk and stop")
    args = ap.parse_args()
    if args.refresh_sources:
        layers.refresh_sources()
    if args.layers or args.only_layers:
        layers.build(os.path.join(args.out, "Layers"), os.path.join(REPO, ".island-source"))
        if args.only_layers:
            return
    names = args.islands or list(recipes.TILES)
    unknown = [n for n in names if n not in recipes.TILES]
    if unknown:
        sys.exit(f"unknown island(s): {', '.join(unknown)}; known: {', '.join(recipes.TILES)}")

    def stamp_scene():
        radii = {n: scene.enclosing_radius(height_from_png(args.out, n), recipes.TILES[n][0]) for n in recipes.TILES}
        scene.stamp(SCENE, radii)
        print("stamped Drift.olo; landfall radii (m): " + ", ".join(f"{n} {r:.0f}" for n, r in radii.items()))

    if args.only_stamp:
        stamp_scene()
        return
    os.makedirs(args.out, exist_ok=True)
    differing = []
    for name in names:
        result = pipeline.generate(name, args.resolution)
        for filename, img in encode(result).items():
            path = os.path.join(args.out, filename)
            if args.check:
                if not os.path.exists(path) or pixel_digest(Image.open(path)) != pixel_digest(img):
                    differing.append(filename)
                continue
            img.save(path, optimize=True)
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            preview.top_down(result).save(os.path.join(args.preview, f"{name}_top.png"))
            for bearing in (270, 0, 90, 180):
                preview.from_sea(result, bearing, offshore=260.0).save(os.path.join(args.preview, f"{name}_sea{bearing:03d}.png"))
    if args.check:
        if differing:
            sys.exit("out of date: " + ", ".join(differing))
        print("all maps match the generator")
    elif args.stamp_scene:
        stamp_scene()


if __name__ == "__main__":
    main()
