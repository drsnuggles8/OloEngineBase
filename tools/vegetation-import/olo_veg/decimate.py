"""Vertex-cluster decimation for the SOLID parts of a plant — trunk, bark, branches.

Solid surfaces are the only part of a scanned plant that decimates sensibly. The
canopy does not (see raster.py), so nothing here is ever pointed at foliage.

The method is grid vertex clustering: quantize positions into cells, replace each
cell with one averaged vertex, rewrite the triangles and drop the ones that
collapse. It is O(n), needs no priority queue, and degrades predictably — which
matters more here than the last few percent of shape fidelity a quadric-error
collapse would buy, because a tree trunk is a near-cylinder and these meshes are
100k triangles that have to reach a few thousand.

THE UV SEAM. Averaging positions across a cell is harmless; averaging TEXTURE
COORDINATES across one is not. Two vertices can sit on top of each other in space
and be on opposite sides of a uv seam (u = 0.99 and u = 0.01 on a cylindrical
trunk wrap). Merging them produces a triangle whose u spans the whole atlas, and
it renders as a smear of the entire texture across one face. The cluster key
therefore carries a COARSE uv alongside the cell, so seam neighbours land in
different clusters and the seam survives.
"""

import numpy as np


def _cluster_keys(positions, uvs, cell, uv_buckets):
    cells = np.floor(positions / cell).astype(np.int64)
    uv_cells = np.floor(np.nan_to_num(uvs) * uv_buckets).astype(np.int64)
    key = np.concatenate([cells, uv_cells], axis=1)
    _, labels = np.unique(key, axis=0, return_inverse=True)
    return labels


def decimate(positions, normals, uvs, tris, cell, uv_buckets=6):
    """Collapse every vertex in a grid cell to one. Returns (pos, nrm, uv, tris)."""
    labels = _cluster_keys(positions, uvs, cell, uv_buckets)
    count = labels.max() + 1

    def average(values):
        out = np.zeros((count, values.shape[1]), np.float64)
        np.add.at(out, labels, values.astype(np.float64))
        hits = np.bincount(labels, minlength=count).astype(np.float64)[:, None]
        return (out / np.maximum(hits, 1.0)).astype(np.float32)

    new_positions = average(positions)
    new_uvs = average(uvs)
    new_normals = average(normals)
    lengths = np.linalg.norm(new_normals, axis=1, keepdims=True)
    new_normals = np.where(lengths > 1e-8, new_normals / np.maximum(lengths, 1e-8), 0.0).astype(np.float32)

    new_tris = labels[tris]
    keep = (new_tris[:, 0] != new_tris[:, 1]) & (new_tris[:, 1] != new_tris[:, 2]) & (new_tris[:, 0] != new_tris[:, 2])

    # Drop triangles that survived with ZERO UV AREA. Collapsing distinct
    # vertices onto one uv is a second kind of degeneracy the index test above
    # cannot see: the triangle still has three indices and real geometric area,
    # but no texture gradient, so it shades with the geometric normal and the
    # engine logs it (MeshOptimization: "98 of 3279 triangles are degenerate").
    # They carry no detail and cost draw work, so they go.
    uv0 = new_uvs[new_tris[:, 0]]
    uv1 = new_uvs[new_tris[:, 1]]
    uv2 = new_uvs[new_tris[:, 2]]
    uv_area = np.abs((uv1[:, 0] - uv0[:, 0]) * (uv2[:, 1] - uv0[:, 1])
                     - (uv2[:, 0] - uv0[:, 0]) * (uv1[:, 1] - uv0[:, 1]))
    keep &= uv_area > 1e-9

    new_tris = new_tris[keep].astype(np.uint32)
    return compact(new_positions, new_normals, new_uvs, new_tris)


def decimate_to_budget(positions, normals, uvs, tris, target_tris, uv_buckets=6, tolerance=0.08, max_steps=20):
    """Binary-search the cell size that lands nearest `target_tris`.

    Returns (pos, nrm, uv, tris, cell). A budget is authored per species in
    recipes.json; searching for the cell that meets it keeps the recipe in the
    units an artist thinks in (triangles) rather than in grid metres, which mean
    nothing without knowing how big the plant is.

    THE UV BUCKET IS A FLOOR, so it has to be able to give way. Splitting the
    cluster key by coarse uv is what saves the seam, but it also means no cell
    size, however large, can merge two vertices in different uv buckets: the
    search hits a floor and silently returns a mesh well over budget. Observed on
    pine_tree_01's bark, which stalled at 11,354 triangles against a budget of
    3,500 because its branches each carry their own uv island. So when the floor
    is above budget the bucket count is coarsened and the search repeats - seam
    fidelity is traded away only when the budget cannot otherwise be met, and the
    caller is told which bucket count was used. A species whose budget forces a
    coarse bucket count is telling you the budget is too tight for its topology,
    so the count is reported rather than silently applied.
    """
    extent = float(np.max(positions.max(axis=0) - positions.min(axis=0)))
    if len(tris) <= target_tris:
        return (*compact(positions, normals, uvs, tris.astype(np.uint32)), 0.0, uv_buckets)

    for buckets in _bucket_ladder(uv_buckets):
        low, high = extent * 1e-4, extent * 4.0
        best, best_cell = None, 0.0
        for _ in range(max_steps):
            cell = np.sqrt(low * high)
            result = decimate(positions, normals, uvs, tris, cell, buckets)
            produced = len(result[3])
            if best is None or abs(produced - target_tris) < abs(len(best[3]) - target_tris):
                best, best_cell = result, cell
            if abs(produced - target_tris) <= tolerance * target_tris:
                return (*best, best_cell, buckets)
            if produced > target_tris:
                low = cell      # cells too small -> too much detail survived
            else:
                high = cell
        if len(best[3]) <= target_tris * (1.0 + tolerance):
            return (*best, best_cell, buckets)
    return (*best, best_cell, buckets)


def _bucket_ladder(start):
    ladder = [start, 4, 3, 2, 1]
    seen, out = set(), []
    for value in ladder:
        if value <= start and value not in seen:
            seen.add(value)
            out.append(value)
    return out


def compact(positions, normals, uvs, tris):
    """Drop vertices no triangle references and renumber."""
    if len(tris) == 0:
        empty = np.zeros((0, 3), np.float32)
        return empty, empty, np.zeros((0, 2), np.float32), np.zeros((0, 3), np.uint32)
    used = np.unique(tris)
    remap = np.zeros(int(used.max()) + 1, np.int64)
    remap[used] = np.arange(len(used))
    return positions[used], normals[used], uvs[used], remap[tris].astype(np.uint32)
