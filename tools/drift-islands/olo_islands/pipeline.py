"""One island, end to end: landform -> erosion -> shore -> sites -> border -> material maps."""
import numpy as np

from . import erosion, fields as f, materials, recipes, shaping as s

RESOLUTION = 1024  # height samples per edge; 16 x 16 chunks of the engine's 64-sample chunk


def generate(name, n=RESOLUTION, verbose=True):
    size, height_scale, seed, _, _ = recipes.TILES[name]
    landform, sites = recipes.RECIPES[name]
    cell = f.cell(n, size)
    x, z = f.xz(n, size)
    aux = landform(x, z, seed)
    h = aux["h"].astype(np.float64)

    per_cell, capacity, repose, thermal_iters = recipes.EROSION[name]
    hard = np.clip(aux["hard"], 0.0, 1.0)
    # Rock stands at any angle the recipe drew; soft ground slumps to its angle of repose.
    talus_map = f.lerp(repose, 88.0, f.smoothstep(0.5, 0.9, hard))
    flow = np.zeros_like(h)
    talus = np.zeros_like(h)
    if per_cell > 0.0:
        land_cells = int((h > 0.0).sum())
        for _ in range(2):  # carve, slump, carve again: gullies with talus in them, not pits
            fl, _, _ = erosion.hydraulic(h, cell, seed, int(land_cells * per_cell * 0.5), hardness=hard * 0.9,
                                         capacity=capacity, batch=max(2000, land_cells // 12))
            flow += fl
            talus += erosion.thermal(h, cell, talus_map, max(thermal_iters // 2, 1))
            seed += 1
    else:
        talus += erosion.thermal(h, cell, talus_map, thermal_iters)

    h = s.apply_beaches(h, cell, aux["beach"], seed + 11, x, z, aux.get("beach_inland", 18.0))
    h = s.apply_rock_platform(h, cell, aux["rock_shore"], seed + 12, x, z)
    if sites is not None:
        h = sites(h, x, z)
    h = s.apply_border(h, x, z, size, cell)

    top = float(h.max())
    if top > height_scale + s.BASE:
        raise ValueError(f"{name}: peak {top:.1f} m does not fit HeightScale {height_scale} above a {s.BASE} m base")
    height01 = np.clip((h - s.BASE) / height_scale, 0.0, 1.0)
    splat0, splat1, macro, weights = materials.bake(name, h, cell, x, z, aux, flow, talus, seed)
    if verbose:
        land = h > 0.0
        d = f.signed_shore_distance(h, cell)
        slope = f.slope_degrees(h, cell)
        ring = (d > 0.0) & (d < 6.0)
        print(f"{name}: peak {top:.1f} m, land {100.0 * land.mean():.1f}% of tile, "
              f"median slope within 6 m of the water {np.median(slope[ring]):.1f} deg, "
              f"coast radius {mean_coast_radius(h, size):.1f} m")
    return {"name": name, "h": h, "height01": height01, "splat0": splat0, "splat1": splat1, "macro": macro,
            "weights": weights, "size": size, "height_scale": height_scale, "cell": cell, "aux": aux,
            "flow": flow}


def mean_coast_radius(h, size):
    """Mean distance from the tile centre to the outermost land along 180 bearings.

    DriftWeatherDirector.lua derives each island's landfall and departure rings from
    (IslandFalloffRadius + 0.5) * 0.5 * WorldSizeX, so the scene restates this number as the
    falloff radius that formula turns back into it.
    """
    n = h.shape[0]
    c = (n - 1) * 0.5
    radii = []
    steps = np.arange(0.0, n * 0.5 - 1.0, 0.5)
    for a in np.radians(np.arange(0.0, 360.0, 2.0)):
        px = np.clip((c + steps * np.cos(a)).astype(int), 0, n - 1)
        pz = np.clip((c + steps * np.sin(a)).astype(int), 0, n - 1)
        on_land = np.nonzero(h[pz, px] > 0.0)[0]
        if on_land.size:
            radii.append(steps[on_land[-1]] * f.cell(n, size))
    return float(np.mean(radii)) if radii else 0.0
