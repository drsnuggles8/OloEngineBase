"""The shared terrain layer library: eight CC0 Poly Haven scans, processed into engine layers.

Every island uses these same eight textures; an island's own colours come from its macro map
(materials.py). Sources download on demand into `.island-source/` at the repo root (git-ignored)
and are checked against the MD5 recorded in `sources.json`.

Per layer the engine reads three files (Assets/Terrain/DriftIslands/Layers/):

    <Layer>_Color.jpg    sRGB albedo, its mean LINEAR colour normalised to materials.NEUTRAL
    <Layer>_Normal.jpg   tangent-space normal, OpenGL convention (+Y up)
    <Layer>_ARM.png      R = ambient occlusion, G = roughness, B = metallic (0), A = height

The albedo is normalised so that the macro map's tint (island colour / neutral) stays near 1:
the macro map can only brighten by about x2, and a white coral sand cannot be reached from a
brown one.
"""
import hashlib
import json
import os
import urllib.request

import numpy as np
from PIL import Image

from .materials import LAYERS, NEUTRAL

RESOLUTION = 1024
MAPS = {"diff": "Diffuse", "nor_gl": "nor_gl", "arm": "arm", "disp": "Displacement"}

# layer -> (Poly Haven id, metres one repeat covers in the scene)
LIBRARY = {
    "Seabed": ("coast_sand_04", 4.0),
    "Sand": ("sandy_gravel_02", 2.6),
    "Shingle": ("ganges_river_pebbles", 2.2),
    "Turf": ("sparse_grass", 2.4),
    "Heath": ("brown_mud_leaves_01", 2.0),
    "Rock": ("rock_face_03", 3.0),
    "RockBedded": ("cliff_side", 3.4),
    "Scree": ("gray_rocks", 2.2),
}

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES_JSON = os.path.join(os.path.dirname(HERE), "sources.json")
UA = {"User-Agent": "asset-fetch/1.0"}


def _get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120).read()


def refresh_sources():
    """Query Poly Haven for each layer's 1k PNG maps and credits; writes sources.json."""
    out = {}
    for layer, (asset, _) in LIBRARY.items():
        files = json.loads(_get(f"https://api.polyhaven.com/files/{asset}"))
        info = json.loads(_get(f"https://api.polyhaven.com/info/{asset}"))
        entry = {"asset": asset, "page": f"https://polyhaven.com/a/{asset}", "name": info.get("name"),
                 "authors": info.get("authors"), "licence": "CC0 1.0 Universal",
                 "scan_size_mm": info.get("dimensions"), "maps": {}}
        for short, key in MAPS.items():
            f = files[key]["1k"]["png"]
            entry["maps"][short] = {"url": f["url"], "md5": f["md5"], "bytes": f["size"]}
        out[layer] = entry
    with open(SOURCES_JSON, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=1, sort_keys=True)
        fh.write("\n")
    return out


def fetch(cache_dir):
    """Download whatever is missing; verify every file. Returns layer -> {map: path}."""
    with open(SOURCES_JSON, "r", encoding="utf-8") as fh:
        sources = json.load(fh)
    paths = {}
    for layer in LAYERS:
        entry = sources[layer]
        paths[layer] = {}
        for short, f in entry["maps"].items():
            dest = os.path.join(cache_dir, entry["asset"], os.path.basename(f["url"]))
            if not os.path.exists(dest):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                data = _get(f["url"])
                with open(dest + ".part", "wb") as fh:
                    fh.write(data)
                os.replace(dest + ".part", dest)
            with open(dest, "rb") as fh:
                digest = hashlib.md5(fh.read()).hexdigest()
            if digest != f["md5"]:
                raise RuntimeError(f"{dest}: md5 {digest} does not match sources.json ({f['md5']}); "
                                   "delete it to re-download, or run --refresh-sources if the scan was republished")
            paths[layer][short] = dest
    return paths


def _to_linear(srgb):
    return np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)


def _to_srgb(lin):
    lin = np.clip(lin, 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)


def _load(path, mode):
    img = Image.open(path)
    if img.mode in ("I;16", "I"):
        arr = np.asarray(img, dtype=np.float64) / 65535.0
        img = Image.fromarray((arr * 255.0 + 0.5).astype(np.uint8))
    img = img.convert(mode)
    if img.size != (RESOLUTION, RESOLUTION):
        img = img.resize((RESOLUTION, RESOLUTION), Image.LANCZOS)
    return np.asarray(img, dtype=np.float64) / 255.0


def process(layer, maps, out_dir):
    """Write the three engine files for one layer. Returns its measured mean linear albedo."""
    index = LAYERS.index(layer)
    lin = _to_linear(_load(maps["diff"], "RGB"))
    mean = lin.reshape(-1, 3).mean(axis=0)
    # Per-channel gain to the neutral mean. Variation inside the scan (and so its texture) is
    # kept; only the average moves.
    lin = lin * (np.array(NEUTRAL[index]) / mean)
    colour = Image.fromarray((_to_srgb(lin) * 255.0 + 0.5).astype(np.uint8))
    colour.save(os.path.join(out_dir, f"{layer}_Color.jpg"), quality=93, subsampling=0, optimize=True)

    normal = _load(maps["nor_gl"], "RGB")
    Image.fromarray((normal * 255.0 + 0.5).astype(np.uint8)).save(
        os.path.join(out_dir, f"{layer}_Normal.jpg"), quality=95, subsampling=0, optimize=True)

    arm = _load(maps["arm"], "RGB")
    height = _load(maps["disp"], "L")
    lo, hi = np.percentile(height, (0.5, 99.5))
    height = np.clip((height - lo) / max(hi - lo, 1e-6), 0.0, 1.0)  # use the whole range for height blending
    packed = np.dstack([arm[..., 0], arm[..., 1], np.zeros_like(height), height])
    Image.fromarray((packed * 255.0 + 0.5).astype(np.uint8)).save(
        os.path.join(out_dir, f"{layer}_ARM.png"), optimize=True)
    return _to_linear(np.asarray(colour, dtype=np.float64) / 255.0).reshape(-1, 3).mean(axis=0)


def build(out_dir, cache_dir):
    os.makedirs(out_dir, exist_ok=True)
    paths = fetch(cache_dir)
    for layer in LAYERS:
        mean = process(layer, paths[layer], out_dir)
        want = NEUTRAL[LAYERS.index(layer)]
        print(f"{layer:11s} mean linear albedo {mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f}  (neutral {want[0]:.3f} {want[1]:.3f} {want[2]:.3f})")
