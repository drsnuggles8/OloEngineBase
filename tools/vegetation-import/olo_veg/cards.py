"""Rebuild a scanned canopy as textured cross-cards.

Why cards and not decimation is argued in raster.py. This module does the two
halves of the rebuild:

  bake_atlas()  picks a few representative clusters of real foliage and bakes
                each into an atlas tile, so the card texture IS the scan.
  build_cards() replaces every cluster in the canopy with a cross of two quads
                sized and oriented to that cluster, pointed at one of the tiles.

A cross - two quads sharing their long axis, ninety degrees apart - is what stops
the canopy collapsing when the camera moves. A single quad per cluster is
invisible edge-on and the tree flickers bald as you orbit it.

NORMALS ARE AUTHORED, NOT INHERITED. A card's geometric normal is its own flat
face, and a canopy lit that way reads as a heap of flat tiles. Instead each card
vertex gets a normal pointing outward from the trunk axis, tilted toward world up.
That is the standard "spherical normals" trick: it makes the canopy light like one
soft volume, which is also what the engine's foliage transmission path (#1236)
assumes it is shading.
"""

import numpy as np

from . import raster

# How far the authored normal leans toward world up. Pure radial normals light a
# conifer from the side only and the crown goes flat under a high sun.
_UP_BIAS = 0.35

# Cards smaller than this fraction of the median are canopy stragglers - a few
# stray needles that would each cost a whole cross-card to draw.
_MIN_CLUSTER_FRACTION = 0.05


def cluster_grid(centroids, target_count, max_steps=28):
    """Label points by uniform grid cell, sized so ~target_count cells are non-empty.

    Returns (labels, cell_size). A uniform grid rather than k-means because the
    canopy has no natural cluster count and k-means over a million points is slow
    for a result that a grid gives directly.
    """
    extent = centroids.max(axis=0) - centroids.min(axis=0)
    low, high = float(np.max(extent)) * 1e-3, float(np.max(extent))
    best = None
    origin = centroids.min(axis=0)
    for _ in range(max_steps):
        cell = np.sqrt(low * high)
        keys = np.floor((centroids - origin) / cell).astype(np.int64)
        unique, labels = np.unique(keys, axis=0, return_inverse=True)
        produced = len(unique)
        if best is None or abs(produced - target_count) < abs(best[2] - target_count):
            best = (labels, cell, produced)
        if abs(produced - target_count) <= 0.05 * target_count:
            break
        if produced > target_count:
            low = cell
        else:
            high = cell
    return best[0], best[1]


def _frame(points):
    """Centroid and principal axes (major, mid, minor) of a point set."""
    centre = points.mean(axis=0)
    centred = points - centre
    if len(points) < 3:
        return centre, np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])
    _, _, basis = np.linalg.svd(centred, full_matrices=False)
    return centre, basis[0], basis[1], basis[2]


def _half_extents(points, centre, axis_a, axis_b, percentile=96.0):
    local = points - centre
    a = float(np.percentile(np.abs(local @ axis_a), percentile))
    b = float(np.percentile(np.abs(local @ axis_b), percentile))
    return max(a, 1e-4), max(b, 1e-4)


def bake_atlas(positions, uvs, tris, tex_index, textures, labels, tile_count, tile_size,
               metre_scale=1.0, alpha_threshold=0.5, log=print):
    """Bake `tile_count` foliage tiles into one square atlas.

    Tiles are the `tile_count` DENSEST clusters, not a spread across the
    population. Spreading looked fairer and was not: sampling down towards the
    median gave pine_tree_01 tiles at 60%, 18%, 7% and 12% coverage, so three
    cards in four were nearly empty and the canopy they built was moth-eaten
    wherever those tiles were reused. Every card in the rebuilt canopy is the
    same size, so every tile has to be equally well filled, and the density
    distribution has a long enough tail that even the top 2% was too wide a net.

    Returns (atlas RGBA float, [(u0, v0, u1, v1)], [(half_a, half_b)]).
    """
    counts = np.bincount(labels)
    populated = np.nonzero(counts > 0)[0]
    if len(populated) == 0:
        raise ValueError("bake_atlas: no populated clusters to bake from")
    picks = populated[np.argsort(-counts[populated])][:tile_count]

    columns = int(np.ceil(np.sqrt(tile_count)))
    rows = int(np.ceil(tile_count / columns))
    atlas = np.zeros((rows * tile_size, columns * tile_size, 4), np.float32)
    rects, extents = [], []

    for slot, cluster in enumerate(picks):
        member = labels == cluster
        cluster_tris = tris[member]
        verts = np.unique(cluster_tris)
        centre, axis_a, axis_b, axis_n = _frame(positions[verts].astype(np.float64))
        half_a, half_b = _half_extents(positions[verts].astype(np.float64), centre, axis_a, axis_b)
        radius = max(half_a, half_b) * 1.02

        local = positions[cluster_tris.reshape(-1)].astype(np.float64) - centre
        x = (local @ axis_a) / radius * 0.5 + 0.5
        y = (local @ axis_b) / radius * 0.5 + 0.5
        corners = np.stack([x * tile_size, (1.0 - y) * tile_size], axis=-1).reshape(-1, 3, 2)
        depths = (local @ axis_n).reshape(-1, 3)
        rgb, alpha = raster.rasterize(
            corners, uvs[cluster_tris.reshape(-1)].astype(np.float64).reshape(-1, 3, 2),
            depths, tex_index[member], textures, tile_size, alpha_threshold)
        rgb = raster.dilate_rgb(rgb, alpha)

        row, column = divmod(slot, columns)
        y0, x0 = row * tile_size, column * tile_size
        atlas[y0 : y0 + tile_size, x0 : x0 + tile_size, :3] = rgb
        atlas[y0 : y0 + tile_size, x0 : x0 + tile_size, 3] = alpha
        rects.append((x0 / atlas.shape[1], y0 / atlas.shape[0],
                      (x0 + tile_size) / atlas.shape[1], (y0 + tile_size) / atlas.shape[0]))
        extents.append((radius, radius))
        log(f"    tile {slot}: {len(cluster_tris):,} source tris, "
            f"{2 * radius * metre_scale:.2f} m across, {100 * alpha.mean():.1f}% coverage")

    return atlas, rects, extents


def build_cards(positions, tris, labels, rects, tile_pixels=512, seed=1337, size_clamp=(0.55, 1.6)):
    """Emit cross-cards, one pair of quads per cluster.

    A card is sized from ITS OWN cluster, clamped to a band around the median,
    and NOT from the extent of the cluster its tile was baked from. Those are
    different clusters, so the needles on a card are drawn at a slightly
    different world scale than they were baked at. The uniform grid keeps the
    spread small (the clamp bounds it to 0.55-1.6x the median), and sizing cards
    to the canopy is what preserves its shape - a crown that thins towards the
    top has smaller clusters there and should have smaller cards.

    Returns (positions, normals, uvs, triangles) for the rebuilt canopy.
    """
    counts = np.bincount(labels)
    median = np.median(counts[counts > 0])
    keep = np.nonzero(counts >= max(median * _MIN_CLUSTER_FRACTION, 1))[0]

    # Trunk axis: the horizontal centre of the canopy. Normals point away from it.
    all_verts = np.unique(tris)
    canopy_centre = positions[all_verts].mean(axis=0)

    rng = np.random.default_rng(seed)
    frames = []
    for cluster in keep:
        member = labels == cluster
        verts = np.unique(tris[member])
        if len(verts) < 4:
            continue
        points = positions[verts].astype(np.float64)
        centre, axis_a, axis_b, axis_n = _frame(points)
        half_a, half_b = _half_extents(points, centre, axis_a, axis_b)
        frames.append((centre, axis_a, axis_b, axis_n, half_a, half_b))

    if not frames:
        empty = np.zeros((0, 3), np.float32)
        return empty, empty, np.zeros((0, 2), np.float32), np.zeros((0, 3), np.uint32)

    sizes = np.array([[f[4], f[5]] for f in frames])
    median_size = np.median(sizes, axis=0)
    low, high = size_clamp

    out_p, out_n, out_uv, out_t = [], [], [], []
    vertex_total = 0
    for centre, axis_a, axis_b, axis_n, half_a, half_b in frames:
        half_a = float(np.clip(half_a, median_size[0] * low, median_size[0] * high))
        half_b = float(np.clip(half_b, median_size[1] * low, median_size[1] * high))

        radial = centre - np.array([canopy_centre[0], centre[1], canopy_centre[2]])
        length = np.linalg.norm(radial)
        radial = radial / length if length > 1e-5 else np.array([0.0, 1.0, 0.0])
        normal = radial + _UP_BIAS * np.array([0.0, 1.0, 0.0])
        normal = normal / np.linalg.norm(normal)

        slot = int(rng.integers(0, len(rects)))
        u0, v0, u1, v1 = rects[slot]
        # Half a texel in, derived from the tile's own size in uv rather than
        # from a constant that happens to match today's atlas: a bilinear tap at
        # the boundary otherwise blends in the neighbouring tile.
        inset = 0.5 * min(abs(u1 - u0), abs(v1 - v0)) / max(tile_pixels, 1)
        u0, v0, u1, v1 = u0 + inset, v0 + inset, u1 - inset, v1 - inset
        if rng.random() < 0.5:
            u0, u1 = u1, u0        # mirrored copies, for free variety

        # Quad A spans (axis_a, axis_b); quad B shares axis_a and uses the third
        # axis, so the pair is never simultaneously edge-on to the camera.
        for across in (axis_b, axis_n):
            corners = [centre - half_a * axis_a - half_b * across,
                       centre + half_a * axis_a - half_b * across,
                       centre + half_a * axis_a + half_b * across,
                       centre - half_a * axis_a + half_b * across]
            out_p.append(np.array(corners, np.float32))
            out_n.append(np.tile(normal.astype(np.float32), (4, 1)))
            out_uv.append(np.array([[u0, v1], [u1, v1], [u1, v0], [u0, v0]], np.float32))
            base = vertex_total
            out_t.append(np.array([[base, base + 1, base + 2], [base, base + 2, base + 3]], np.uint32))
            vertex_total += 4

    return (np.concatenate(out_p), np.concatenate(out_n),
            np.concatenate(out_uv), np.concatenate(out_t))
