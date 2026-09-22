#!/usr/bin/env python3
"""Import CC0 vegetation scans from Poly Haven into game-ready committed assets (issue #1398).

    python tools/vegetation-import/import_vegetation.py            # every species
    python tools/vegetation-import/import_vegetation.py pine grass # named ones
    python tools/vegetation-import/import_vegetation.py --list

WHAT PROBLEM THIS SOLVES. The repository owned no vegetation art: two generated
meshes of 48 and 57 faces, and one 512x512 photo of a grass tuft that textured
every species including the pines. At the authored alpha cutoff that texture
passes 17.9% of its texels, so a pine canopy was a handful of big triangles
discarding four pixels in five and drawing grass blades in the rest.

WHAT THIS TOOL PRODUCES. Per species, a directory under
OloEditor/SandboxProject/Assets/Models/Vegetation/ holding an OBJ with one
submesh per material, its own albedo maps, and the provenance files the
repository's InfiniteScanHead convention asks for.

WHY IT IS NOT JUST A DOWNLOADER. A Poly Haven hero conifer is 17.4 million
triangles in a 949 MB buffer, which can neither be committed nor drawn. Ground
cover imports almost directly; trees do not, and the two halves of a tree need
opposite treatments:

  * the trunk and branches are solid surfaces, so they decimate (decimate.py);
  * the canopy is several hundred thousand individually modelled needles, which
    do NOT decimate - collapsing a thin needle strip gives a sliver, not a
    needle. The canopy is instead rebuilt from cross-cards whose texture is
    BAKED OUT OF THE SCAN (raster.py, cards.py), so the alpha in the result is
    real geometry coverage rather than a painted guess.

RE-RUNNABLE, NOT A ONE-OFF. Everything is driven by recipes.json - source asset,
submesh split, triangle budgets, card counts - so a budget change is an edit and
a re-run, and the committed asset can always be rebuilt from its source.
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from olo_veg import cards, decimate, licence, polyhaven, raster  # noqa: E402
from olo_veg.gltf import Gltf  # noqa: E402
from olo_veg.wavefront import write_mtl, write_obj  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO_ROOT, "OloEditor", "SandboxProject", "Assets", "Models", "Vegetation")
# OUTSIDE the sandbox project on purpose. The editor registers every asset it
# finds under SandboxProject/Assets/, git-ignored or not, so a download cache
# in there put 65 source .jpg textures into AssetRegistry.oar and CI - which
# never fetches them - failed with "registered asset path missing on disk".
DEFAULT_CACHE = os.path.join(REPO_ROOT, ".vegetation-source")


# ---------------------------------------------------------------------------
# geometry gathering
# ---------------------------------------------------------------------------

def gather(gltf, mesh_indices, materials):
    """Merge every primitive in `materials` into one vertex/triangle soup.

    Returns (positions, normals, uvs, tris, tex_index, used_materials) where
    tex_index selects which of used_materials each TRIANGLE came from - a foliage
    group may span several source materials (a tree's leaves and the twigs they
    hang off), and the card bake has to sample each triangle from its own map.
    """
    order = {name: i for i, name in enumerate(materials)}
    parts, tex = [], []
    offset = 0
    for mesh_index, _, material, prim in gltf.primitives():
        if mesh_indices is not None and mesh_index not in mesh_indices:
            continue
        if material not in order:
            continue
        positions, normals, uvs, tris = gltf.read_primitive(prim)
        parts.append((positions, normals, uvs, tris + offset))
        tex.append(np.full(len(tris), order[material], np.int32))
        offset += len(positions)
    if not parts:
        return None
    positions = np.concatenate([p[0] for p in parts])
    normals = np.concatenate([p[1] for p in parts])
    uvs = np.concatenate([p[2] for p in parts])
    tris = np.concatenate([p[3] for p in parts]).astype(np.uint32)
    return positions, normals, uvs, tris, np.concatenate(tex)


def normalize(groups):
    """Move every group into the foliage authoring frame: base at origin, unit height.

    FoliageRenderer assumes exactly this and rescales nothing - it warns that a
    mesh spanning any other range "is drawn at the wrong size in BOTH near mesh
    and far impostor, consistently". The real height in metres is returned so it
    can be recorded and used to set a layer's MinHeight/MaxHeight honestly.
    """
    lows = np.array([g["positions"].min(axis=0) for g in groups])
    highs = np.array([g["positions"].max(axis=0) for g in groups])
    low, high = lows.min(axis=0), highs.max(axis=0)
    height = float(high[1] - low[1])
    centre = np.array([(low[0] + high[0]) * 0.5, low[1], (low[2] + high[2]) * 0.5], np.float32)
    scale = 1.0 / max(height, 1e-6)
    for group in groups:
        group["positions"] = ((group["positions"] - centre) * scale).astype(np.float32)
    return height


# ---------------------------------------------------------------------------
# textures
# ---------------------------------------------------------------------------

def load_map(path, size=None):
    image = Image.open(path)
    if size and image.size != (size, size):
        image = image.resize((size, size), Image.LANCZOS)
    return image


def build_albedo(diffuse_path, alpha_path, size):
    """RGBA albedo: colour from the diffuse map, cutout from the standalone alpha.

    The glTF package ships jpg, which has no alpha channel at all, so the mask
    always has to come from the separate png map. A species with no alpha map is
    opaque by nature (bark) and gets a full-alpha channel.
    """
    rgb = load_map(diffuse_path, size).convert("RGB")
    if not alpha_path:
        # Bark and trunks are opaque by nature. Writing them as RGB rather than
        # RGBA-with-a-constant-alpha keeps the intent readable: a cutout channel
        # on a trunk invites someone to wonder what it masks.
        return rgb
    return Image.merge("RGBA", (*rgb.split(), load_map(alpha_path, size).convert("L")))


def measure_coverage(positions, uvs, tris, alpha, cutoff, samples=9):
    """Fraction of the mesh SURFACE whose albedo survives the alpha test.

    Measured over the texels the mesh actually samples, area-weighted - not over
    the whole image. Those are very different numbers and only this one describes
    what a viewer sees: pine_tree_01's twig atlas reads 69.5% across the sheet,
    but most of that is unused opaque background, and the sheet figure would have
    reported a canopy that does not exist.
    """
    if len(tris) == 0:
        return 0.0
    height, width = alpha.shape
    v0, v1, v2 = positions[tris[:, 0]], positions[tris[:, 1]], positions[tris[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(v1 - v0, v2 - v0), axis=1)
    t0, t1, t2 = uvs[tris[:, 0]], uvs[tris[:, 1]], uvs[tris[:, 2]]
    rng = np.random.default_rng(11)
    hits = np.zeros(len(tris))
    for _ in range(samples):
        r1, r2 = rng.random(len(tris)), rng.random(len(tris))
        root = np.sqrt(r1)
        b0, b1, b2 = (1 - root), root * (1 - r2), root * r2
        u = b0 * t0[:, 0] + b1 * t1[:, 0] + b2 * t2[:, 0]
        v = b0 * t0[:, 1] + b1 * t1[:, 1] + b2 * t2[:, 1]
        x = np.clip((u % 1.0) * (width - 1), 0, width - 1).astype(np.int32)
        y = np.clip((v % 1.0) * (height - 1), 0, height - 1).astype(np.int32)
        hits += alpha[y, x] >= cutoff
    total = area.sum()
    if total <= 0:
        return 0.0
    return float(100.0 * np.average(hits / samples, weights=area))


def bake_billboard(parts, size, alpha_threshold=0.5):
    """Bake the finished plant into the flat-card texture the far rung draws.

    A foliage layer has two shapes over one instance stream: the authored mesh up
    close, and a single quad beyond MeshViewDistance. FoliageRenderer builds that
    quad at x in [-0.5, 0.5], y in [0, 1] with uv (0,0) at the bottom-left - the
    same authoring frame the mesh is normalized into - so projecting the plant
    along -Z over exactly that rectangle gives a billboard that lines up with its
    own geometry at the hand-over distance.

    Baking it is not a nicety. The layer albedo is what the card samples, and
    pointing it at a source UV ATLAS (which is what every scanned plant ships)
    draws the atlas sheet on the card: cones, bark swatches and all. That is a
    close cousin of the defect issue #1398 is about, so the card gets a picture
    of the plant instead.

    parts: [(positions, uvs, tris, rgba_path)]. Returns (image, coverage, width).
    """
    width = 0.0
    for positions, _, tris, _ in parts:
        width = max(width, float(np.abs(positions[tris.reshape(-1), 0]).max()) * 2.0)
    # A plant wider than the quad would be CROPPED, and cropping is the worse
    # failure: fern_02 spreads 2.31x its own height, so a straight projection cut
    # 57% of its fronds off the billboard. Squeezing x instead keeps the whole
    # plant, at the cost of a card narrower than the mesh it hands over from.
    # Ground contact and height stay exact either way, and a plant that already
    # fits (every tree here) is projected untouched.
    squeeze = min(1.0, 1.0 / width) if width > 0 else 1.0

    textures, corners, uvs, depths, index = [], [], [], [], []
    for slot, (positions, part_uvs, tris, texture_path) in enumerate(parts):
        rgba = np.asarray(Image.open(texture_path).convert("RGBA"), np.float32) / 255.0
        textures.append((rgba[..., :3], rgba[..., 3]))
        flat = positions[tris.reshape(-1)]
        x = (flat[:, 0] * squeeze + 0.5) * size
        y = (1.0 - flat[:, 1]) * size
        corners.append(np.stack([x, y], -1).reshape(-1, 3, 2))
        uvs.append(part_uvs[tris.reshape(-1)].astype(np.float64).reshape(-1, 3, 2))
        depths.append(flat[:, 2].reshape(-1, 3).astype(np.float64))
        index.append(np.full(len(tris), slot, np.int32))

    rgb, alpha = raster.rasterize(np.concatenate(corners).astype(np.float64),
                                 np.concatenate(uvs), np.concatenate(depths),
                                 np.concatenate(index), textures, size, alpha_threshold)
    rgb = raster.dilate_rgb(rgb, alpha)
    image = Image.fromarray(
        (np.clip(np.concatenate([rgb, alpha[..., None]], -1), 0, 1) * 255).astype(np.uint8), "RGBA")
    return image, float(100.0 * alpha.mean()), width, squeeze


# ---------------------------------------------------------------------------
# per-species import
# ---------------------------------------------------------------------------

def import_species(recipe, out_root, cache_dir, log=print):
    name = recipe["name"]
    source = recipe["source"]
    resolution = recipe.get("resolution", "1k")
    cutoff = recipe.get("alpha_cutoff", 0.5)
    log(f"\n=== {name}  <- polyhaven:{source} ({resolution}) ===")

    info = polyhaven.asset_info(source)
    want = []
    for group in recipe["groups"]:
        for material in group["materials"]:
            want.append(material["diffuse"])
            if material.get("alpha"):
                want.append(material["alpha"])
    gltf_path, maps = polyhaven.fetch_gltf(source, resolution, cache_dir, sorted(set(want)), log=log)
    gltf = Gltf(gltf_path)

    mesh_indices = recipe.get("meshes")
    mesh_indices = set(mesh_indices) if mesh_indices is not None else None

    present = {material for mesh_index, _, material, _ in gltf.primitives()
               if mesh_indices is None or mesh_index in mesh_indices}
    claimed = {m["name"] for group in recipe["groups"] for m in group["materials"]}
    dropped = sorted(present - claimed)
    if dropped:
        log(f"  dropped materials (not in recipe): {', '.join(dropped)}")

    collected = []
    for group in recipe["groups"]:
        names = [m["name"] for m in group["materials"]]
        if group["mode"] != "foliage" and len(names) != 1:
            raise SystemExit(f"{name}/{group['submesh']}: mode {group['mode']!r} takes exactly one "
                             f"source material (it writes one texture); got {names}")
        data = gather(gltf, mesh_indices, names)
        if data is None:
            raise SystemExit(f"{name}: no primitives matched materials {names}")
        positions, normals, uvs, tris, tex_index = data
        collected.append({"spec": group, "positions": positions, "normals": normals,
                          "uvs": uvs, "tris": tris, "tex_index": tex_index})

    real_height = normalize(collected)
    log(f"  source height {real_height:.2f} m -> normalized to base-at-origin unit height")

    os.makedirs(os.path.join(out_root, name, "Textures"), exist_ok=True)
    obj_groups, mtl_entries, report, modifications, billboard_parts = [], [], [], [], []
    source_tri_total = sum(len(g["tris"]) for g in collected)

    for entry in collected:
        spec = entry["spec"]
        mode = spec["mode"]
        submesh = spec["submesh"]
        material_name = f"{name}_{submesh}"
        texture_size = spec.get("texture_size", 1024)
        texture_rel = os.path.join("Textures", f"{name}_{submesh}.png")
        texture_abs = os.path.join(out_root, name, texture_rel)
        before = len(entry["tris"])

        if mode == "foliage":
            # One (diffuse, alpha) pair per source material, in the order gather()
            # numbered them, because a canopy group can span materials with
            # different maps - a broadleaf's leaves and the twigs they hang off.
            textures = []
            for material in spec["materials"]:
                diffuse = np.asarray(load_map(maps[material["diffuse"]]).convert("RGB"), np.float32) / 255.0
                mask = None
                if material.get("alpha"):
                    mask = np.asarray(load_map(maps[material["alpha"]]).convert("L"), np.float32) / 255.0
                textures.append((diffuse, mask))
            centroids = entry["positions"][entry["tris"]].mean(axis=1)
            labels, cell = cards.cluster_grid(centroids, spec["cards"])
            log(f"  {submesh}: {before:,} source tris -> {labels.max() + 1:,} clusters "
                f"(grid {cell * real_height:.2f} m)")
            atlas, rects, _ = cards.bake_atlas(
                entry["positions"], entry["uvs"], entry["tris"], entry["tex_index"], textures,
                labels, spec.get("tiles", 4), spec.get("tile_size", 512),
                metre_scale=real_height, alpha_threshold=cutoff, log=log)
            positions, normals, uvs, tris = cards.build_cards(
                entry["positions"], entry["tris"], labels, rects,
                tile_pixels=spec.get("tile_size", 512), seed=recipe.get("seed", 1337))
            Image.fromarray((np.clip(atlas, 0, 1) * 255).astype(np.uint8), "RGBA").resize(
                (texture_size, texture_size), Image.LANCZOS).save(texture_abs, optimize=True)
            has_alpha = True
            modifications.append(
                f"`{submesh}`: the scanned canopy ({before:,} triangles of individually modelled "
                f"foliage) was clustered into {len(tris) // 4:,} groups and rebuilt as cross-cards "
                f"({len(tris):,} triangles). The card atlas was baked from the scan itself by "
                f"orthographic projection of each cluster, so its transparency is that cluster's "
                f"real geometry coverage.")
        elif mode == "solid":
            positions, normals, uvs, tris, cell, buckets = decimate.decimate_to_budget(
                entry["positions"], entry["normals"], entry["uvs"], entry["tris"], spec["target_tris"])
            log(f"  {submesh}: {before:,} -> {len(tris):,} tris (target {spec['target_tris']:,}; grid "
                f"vertex clustering, cell {cell * real_height:.3f} m, {buckets} uv buckets)")
            source_maps = spec["materials"][0]
            build_albedo(maps[source_maps["diffuse"]], maps.get(source_maps.get("alpha")),
                         texture_size).save(texture_abs, optimize=True)
            has_alpha = bool(source_maps.get("alpha"))
            modifications.append(
                f"`{submesh}`: decimated from {before:,} to {len(tris):,} triangles by grid vertex "
                f"clustering, seam-preserving (see tools/vegetation-import/olo_veg/decimate.py).")
        elif mode == "direct":
            positions, normals, uvs, tris = (entry["positions"], entry["normals"],
                                             entry["uvs"], entry["tris"])
            log(f"  {submesh}: {len(tris):,} tris taken as-is")
            source_maps = spec["materials"][0]
            build_albedo(maps[source_maps["diffuse"]], maps.get(source_maps.get("alpha")),
                         texture_size).save(texture_abs, optimize=True)
            has_alpha = bool(source_maps.get("alpha"))
            modifications.append(
                f"`{submesh}`: geometry used as scanned ({len(tris):,} triangles); only the "
                f"mesh selection, recentring and unit-height rescale below were applied.")
        else:
            raise SystemExit(f"{name}: unknown group mode {mode!r}")

        alpha_channel = np.asarray(Image.open(texture_abs).convert("RGBA"), np.float32)[..., 3] / 255.0  # RGB reads as 255
        coverage = measure_coverage(positions, uvs, tris, alpha_channel, cutoff)
        report.append((f"{submesh} triangles", f"{len(tris):,} (from {before:,})"))
        report.append((f"{submesh} coverage at cutoff {cutoff}", f"{coverage:.1f}%"))
        log(f"      albedo {os.path.basename(texture_abs)} "
            f"{os.path.getsize(texture_abs) / 1024:.0f} KiB, coverage at cutoff {cutoff}: {coverage:.1f}%")

        obj_groups.append((material_name, positions, normals, uvs, tris))
        mtl_entries.append((material_name, texture_rel, has_alpha))
        billboard_parts.append((positions, uvs, tris, texture_abs))

    card_rel = os.path.join("Textures", f"{name}_card.png")
    card_abs = os.path.join(out_root, name, card_rel)
    card_size = recipe.get("card_texture_size", 512)
    card_image, card_coverage, plant_width, card_squeeze = bake_billboard(billboard_parts, card_size, cutoff)
    card_image.save(card_abs, optimize=True)
    log(f"  card: {os.path.basename(card_abs)} {os.path.getsize(card_abs) / 1024:.0f} KiB, "
        f"{card_coverage:.1f}% coverage, plant is {plant_width:.2f} x 1.00 of the card quad")
    if card_squeeze < 1.0:
        log(f"      NOTE: wider than the unit card quad, so the billboard is squeezed to "
            f"{card_squeeze:.2f}x in x to keep the whole plant. The mesh and impostor rungs are unaffected.")
    report.append(("flat-card coverage", f"{card_coverage:.1f}%"))
    modifications.append(
        f"Baked `Textures/{name}_card.png`, the flat billboard the layer draws beyond "
        f"MeshViewDistance, by projecting the finished plant over the card quad's own frame"
        + (f" (squeezed to {card_squeeze:.2f}x in x: the plant spreads {plant_width:.2f} times "
           f"its height, and the quad is square)." if card_squeeze < 1.0 else "."))

    obj_path = os.path.join(out_root, name, f"{name}.obj")
    write_obj(obj_path, name, obj_groups, mtl_name=f"{name}.mtl")
    write_mtl(os.path.join(out_root, name, f"{name}.mtl"), mtl_entries)

    total_tris = sum(len(g[4]) for g in obj_groups)
    modifications.append(
        f"Recentred on the trunk and rescaled to the foliage authoring frame (base at the origin, "
        f"unit height). The plant is **{real_height:.2f} m** tall in the original, which is the "
        f"figure a layer's MinHeight/MaxHeight should reflect.")
    if dropped:
        modifications.append(f"Dropped source materials not needed by the game asset: {', '.join(dropped)}.")
    modifications.append(
        f"Textures rebuilt as RGBA png from the source diffuse plus its separate alpha map "
        f"(the glTF package ships jpg, which cannot carry a cutout).")

    files = polyhaven.asset_files(source)
    gltf_url, includes = polyhaven.gltf_package(files, resolution)
    urls = [gltf_url] + [url for url, _ in includes.values()]
    urls += [polyhaven.map_url(files, m, resolution) for m in sorted(set(want))]
    licence.write_licence(os.path.join(out_root, name), source, info, urls, modifications, resolution)

    committed = 0
    for root, _, names in os.walk(os.path.join(out_root, name)):
        committed += sum(os.path.getsize(os.path.join(root, f)) for f in names)
    report.insert(0, ("total triangles", f"{total_tris:,} (from {source_tri_total:,} scanned)"))
    report.insert(1, ("committed size", f"{committed / 1e6:.2f} MB"))
    report.insert(2, ("real-world height", f"{real_height:.2f} m"))
    licence.write_readme(os.path.join(out_root, name), name, info,
                         {"total_tris": total_tris, "report": report})

    log(f"  -> {total_tris:,} triangles, {committed / 1e6:.2f} MB committed")
    return {"name": name, "source": source, "tris": total_tris, "bytes": committed,
            "height_m": real_height, "report": report}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("species", nargs="*", help="species to import (default: all in recipes.json)")
    parser.add_argument("--recipes", default=os.path.join(HERE, "recipes.json"))
    parser.add_argument("--out", default=DEFAULT_OUT, help="output root")
    parser.add_argument("--cache", default=DEFAULT_CACHE, help="where raw scans are kept (git-ignored)")
    parser.add_argument("--list", action="store_true", help="list the recipes and exit")
    args = parser.parse_args()

    with open(args.recipes, "r", encoding="utf-8") as handle:
        recipes = [r for r in json.load(handle)["species"]]

    if args.list:
        for recipe in recipes:
            print(f"  {recipe['name']:12} <- polyhaven:{recipe['source']:18} {recipe.get('note', '')}")
        return 0

    wanted = set(args.species) if args.species else None
    selected = [r for r in recipes if wanted is None or r["name"] in wanted]
    if wanted:
        missing = wanted - {r["name"] for r in selected}
        if missing:
            raise SystemExit(f"no recipe named: {', '.join(sorted(missing))}")

    results = [import_species(recipe, args.out, args.cache) for recipe in selected]

    print("\n" + "=" * 72)
    print(f"{'species':12}{'triangles':>12}{'committed':>12}{'height':>10}")
    for result in results:
        print(f"{result['name']:12}{result['tris']:>12,}{result['bytes'] / 1e6:>10.2f} MB"
              f"{result['height_m']:>9.2f} m")
    print(f"{'TOTAL':12}{sum(r['tris'] for r in results):>12,}"
          f"{sum(r['bytes'] for r in results) / 1e6:>10.2f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
