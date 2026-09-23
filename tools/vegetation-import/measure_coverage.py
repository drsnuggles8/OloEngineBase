#!/usr/bin/env python3
"""Measure what fraction of a plant's drawn surface survives its alpha cutoff.

    python tools/vegetation-import/measure_coverage.py --before-after
    python tools/vegetation-import/measure_coverage.py path/to.obj --cutoff 0.5

This is the number issue #1398 asked to be chosen deliberately rather than
inherited. The repository's whole vegetation library used to be one grass cutout
at 17.9%, so a pine canopy discarded four pixels in five and drew grass blades in
the rest.

MEASURE THE SURFACE, NOT THE SHEET. Averaging alpha over a whole texture answers
a question nobody asked. A UV atlas has unused space, and on pine_tree_01's twig
sheet that unused space is opaque white: measured flat the sheet reads 69.5%,
while the canopy that actually samples it reads 54.4%. Only the second number
describes anything a viewer sees. So every figure here is sampled at points
distributed over the mesh's own triangles and weighted by triangle area.

`--before-after` runs the comparison the issue is about: the generated 48-face
pine wearing grass.png, against the imported plants that replace it.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from import_vegetation import measure_coverage  # noqa: E402
from preview import load_obj  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
VEG = os.path.join(REPO_ROOT, "OloEditor", "SandboxProject", "Assets", "Models", "Vegetation")
TEXTURES = os.path.join(REPO_ROOT, "OloEditor", "assets", "textures")


def alpha_of(path):
    image = Image.open(path)
    if image.mode in ("RGBA", "LA"):
        return np.asarray(image.convert("RGBA"), np.float32)[..., 3] / 255.0
    return np.ones(image.size[::-1], np.float32)


def obj_coverage(obj_path, cutoff, texture_override=None):
    """Per-submesh coverage for an OBJ, plus the area-weighted whole-plant figure."""
    groups, textures = load_obj(obj_path)
    rows, areas, weighted = [], [], []
    for material, tri_p, _, tri_uv in groups:
        texture = texture_override or textures.get(material)
        if not texture or not os.path.exists(texture):
            rows.append((material, None, None))
            continue
        # load_obj returns per-CORNER arrays; measure_coverage wants indexed ones.
        positions = tri_p.reshape(-1, 3)
        uvs = tri_uv.reshape(-1, 2).copy()
        # These OBJs carry top-down (glTF) v, which is what the engine samples:
        # Model flips every OBJ's v and Texture2D flips rows on upload. No flip.
        tris = np.arange(len(positions), dtype=np.int64).reshape(-1, 3)
        v0, v1, v2 = positions[tris[:, 0]], positions[tris[:, 1]], positions[tris[:, 2]]
        area = float((0.5 * np.linalg.norm(np.cross(v1 - v0, v2 - v0), axis=1)).sum())
        value = measure_coverage(positions, uvs, tris, alpha_of(texture), cutoff)
        rows.append((material, value, area))
        areas.append(area)
        weighted.append(value * area)
    total = float(np.sum(areas)) if areas else 0.0
    overall = float(np.sum(weighted) / total) if total > 0 else 0.0
    return rows, overall


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("obj", nargs="*")
    parser.add_argument("--cutoff", type=float, default=0.5)
    parser.add_argument("--texture", help="override the material texture (for the old stand-ins)")
    parser.add_argument("--before-after", action="store_true")
    args = parser.parse_args()

    if args.before_after:
        grass_png = os.path.join(TEXTURES, "grass.png")
        sheet = alpha_of(grass_png)
        print("\nBEFORE - every species wore one grass cutout (issue #1398)\n")
        print(f"  {'plant':22}{'albedo':22}{'coverage at cutoff ' + str(args.cutoff):>26}")
        for stand_in in ("pine.obj", "palm.obj"):
            path = os.path.join(VEG, stand_in)
            if not os.path.exists(path):
                continue
            _, overall = obj_coverage(path, args.cutoff, texture_override=grass_png)
            print(f"  {stand_in:22}{'assets/textures/grass.png':22}{overall:>25.1f}%")
        print(f"  {'(the sheet itself)':22}{'assets/textures/grass.png':22}"
              f"{100 * float((sheet >= args.cutoff).mean()):>25.1f}%")

        print("\nAFTER - imported CC0 scans, one albedo per species per submesh\n")
        print(f"  {'plant':22}{'submesh':22}{'coverage at cutoff ' + str(args.cutoff):>26}")
        for name in sorted(os.listdir(VEG)):
            obj = os.path.join(VEG, name, f"{name}.obj")
            if not os.path.exists(obj):
                continue
            rows, overall = obj_coverage(obj, args.cutoff)
            for material, value, _ in rows:
                shown = f"{value:.1f}%" if value is not None else "no texture"
                print(f"  {name:22}{material:22}{shown:>26}")
            print(f"  {'':22}{'-> whole plant':22}{overall:>25.1f}%")
        return 0

    for path in args.obj:
        rows, overall = obj_coverage(path, args.cutoff, texture_override=args.texture)
        print(f"\n{path}")
        for material, value, area in rows:
            shown = f"{value:.1f}%" if value is not None else "no texture"
            print(f"  {material:28}{shown:>10}")
        print(f"  {'whole plant':28}{overall:>9.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
