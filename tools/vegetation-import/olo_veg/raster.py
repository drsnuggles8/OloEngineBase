"""Orthographic software rasterizer used to bake foliage cards from scan geometry.

This is the step that makes the imported trees honest. A scanned conifer models
every needle — pine_tree_01 carries 6.8 million triangles of them, and 91.7% of
that surface passes its own alpha test, because the alpha map only trims a needle
EDGE. Such a canopy cannot be decimated (collapsing thin needle strips produces
slivers, not needles) and cannot be committed (949 MB). So the canopy is rebuilt
from cards, and the card texture is BAKED FROM THE SCAN ITSELF: project a cluster
of real needles onto its own dominant plane and record what is there.

The transparency in the result is therefore real geometry coverage, not a painted
guess — a texel is clear because the scan has no surface at that point. That is
the difference between this and the defect issue #1398 describes, where every
species shared one 17.9%-coverage grass cutout.

Depth is resolved with a z-buffer along the projection axis so overlapping
needles composite in the right order.
"""

import numpy as np


def sample(texture, u, v):
    """Nearest-neighbour sample in glTF UV convention: v runs DOWNWARD from the top."""
    height, width = texture.shape[:2]
    x = np.clip((u % 1.0) * (width - 1), 0, width - 1).astype(np.int32)
    y = np.clip((v % 1.0) * (height - 1), 0, height - 1).astype(np.int32)
    return texture[y, x]


def rasterize(corners, uvs, depths, tex_index, textures, size, alpha_threshold=0.5):
    """Rasterize triangles into an RGBA tile.

    corners    (T, 3, 2) float pixel coordinates in [0, size]
    uvs        (T, 3, 2) source texture coordinates, glTF convention
    depths     (T, 3)    view-axis depth; larger is nearer
    tex_index  (T,)      which entry of `textures` each triangle samples
    textures   list of (diffuse HxWx3 float, alpha HxW float or None)
    Returns (rgb float32 HxWx3, alpha float32 HxW).
    """
    rgb = np.zeros((size, size, 3), np.float32)
    coverage = np.zeros((size, size), np.float32)
    zbuffer = np.full((size, size), -np.inf, np.float32)

    for t in range(len(corners)):
        p = corners[t]
        x0 = max(int(np.floor(p[:, 0].min())), 0)
        x1 = min(int(np.ceil(p[:, 0].max())) + 1, size)
        y0 = max(int(np.floor(p[:, 1].min())), 0)
        y1 = min(int(np.ceil(p[:, 1].max())) + 1, size)
        if x1 <= x0 or y1 <= y0:
            continue

        denom = (p[1, 1] - p[2, 1]) * (p[0, 0] - p[2, 0]) + (p[2, 0] - p[1, 0]) * (p[0, 1] - p[2, 1])
        if abs(denom) < 1e-12:
            continue

        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        b0 = ((p[1, 1] - p[2, 1]) * (gx - p[2, 0]) + (p[2, 0] - p[1, 0]) * (gy - p[2, 1])) / denom
        b1 = ((p[2, 1] - p[0, 1]) * (gx - p[2, 0]) + (p[0, 0] - p[2, 0]) * (gy - p[2, 1])) / denom
        b2 = 1.0 - b0 - b1
        inside = (b0 >= 0.0) & (b1 >= 0.0) & (b2 >= 0.0)
        if not inside.any():
            continue

        z = b0 * depths[t, 0] + b1 * depths[t, 1] + b2 * depths[t, 2]
        window = zbuffer[y0:y1, x0:x1]
        inside &= z > window
        if not inside.any():
            continue

        u = b0 * uvs[t, 0, 0] + b1 * uvs[t, 1, 0] + b2 * uvs[t, 2, 0]
        v = b0 * uvs[t, 0, 1] + b1 * uvs[t, 1, 1] + b2 * uvs[t, 2, 1]
        diffuse, alpha = textures[int(tex_index[t])]
        if alpha is not None:
            # The scan's own alpha mask trims the needle/leaf edge. Applying it
            # HERE rather than into the output alpha is deliberate: a texel the
            # source calls transparent must not claim geometry coverage, or the
            # card grows a rectangular halo around every leaf.
            inside &= sample(alpha, u, v) >= alpha_threshold
            if not inside.any():
                continue

        window[inside] = z[inside]
        rgb[y0:y1, x0:x1][inside] = sample(diffuse, u[inside], v[inside])
        coverage[y0:y1, x0:x1][inside] = 1.0

    return rgb, coverage


def dilate_rgb(rgb, alpha, passes=4):
    """Bleed colour outward into clear texels.

    A card is sampled with bilinear filtering and mipmapped; without this, texels
    just outside the needle carry black, and every needle edge picks up a dark
    fringe as it minifies. Alpha is untouched — only the colour spreads.
    """
    out = rgb.copy()
    filled = alpha > 0.0
    for _ in range(passes):
        if filled.all():
            break
        total = np.zeros_like(out)
        count = np.zeros(filled.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            shifted_rgb = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            shifted_hit = np.roll(np.roll(filled, dy, axis=0), dx, axis=1)
            total += shifted_rgb * shifted_hit[..., None]
            count += shifted_hit
        grow = (~filled) & (count > 0)
        out[grow] = total[grow] / count[grow][..., None]
        filled = filled | grow
    return out
