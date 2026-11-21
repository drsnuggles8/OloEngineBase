"""Software previews, so a recipe can be judged without building or launching the engine.

`top_down` is a shaded map. `from_sea` is a perspective view from a boat: a column-marching
height-field renderer (one ray fan per screen column, front to back, filling upward as the
terrain rises), lit by one sun with the baked albedo. Neither is the engine's image; they answer
"is this the shape I meant" in two seconds.
"""
import numpy as np
from PIL import Image

from . import fields as f
from .materials import mean_albedo

WATER = np.array((0.035, 0.11, 0.16))
SKY_TOP = np.array((0.30, 0.47, 0.72))
SKY_LOW = np.array((0.72, 0.80, 0.88))


def _srgb(lin):
    lin = np.clip(lin, 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)


def _lit(result, sun=(0.45, 0.62, -0.64)):
    h, cell = result["h"], result["cell"]
    albedo = mean_albedo(result["weights"], result["macro"])
    gz, gx = np.gradient(h, cell)
    nrm = np.stack([-gx, np.ones_like(h), -gz], axis=-1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    s = np.array(sun) / np.linalg.norm(sun)
    ndl = np.clip(nrm @ s, 0.0, 1.0)
    sky = 0.5 + 0.5 * nrm[..., 1]
    colour = albedo * (2.6 * ndl[..., None] * np.array((1.0, 0.95, 0.86)) + 0.55 * sky[..., None] * np.array((0.6, 0.75, 1.0)))
    depth = np.clip(-h, 0.0, None)[..., None]
    under = f.lerp(colour * np.array((0.55, 0.8, 0.85)), WATER, 1.0 - np.exp(-depth / 3.0))
    return np.where(h[..., None] > 0.0, colour, under)


def top_down(result, pixels=768):
    img = _srgb(_lit(result) * 0.9)
    out = Image.fromarray((img[::-1] * 255).astype(np.uint8))  # north up
    return out.resize((pixels, pixels), Image.LANCZOS)


def from_sea(result, bearing_deg, offshore=150.0, eye_height=3.5, fov_deg=45.0, width=960, height=540,
             centre=None):
    """Perspective view from `offshore` metres outside the coast along a bearing from the centre."""
    h, cell, size = result["h"], result["cell"], result["size"]
    n = h.shape[0]
    colour = _lit(result)
    cx, cz = centre if centre else (size * 0.5, size * 0.5)
    b = np.radians(bearing_deg)
    dirx, dirz = np.cos(b), np.sin(b)
    # Find the coast along the bearing.
    steps = np.arange(size * 0.75, 0.0, -cell)
    px = np.clip(((cx + dirx * steps) / cell).astype(int), 0, n - 1)
    pz = np.clip(((cz + dirz * steps) / cell).astype(int), 0, n - 1)
    hit = np.nonzero(h[pz, px] > 0.0)[0]
    shore = steps[hit[0]] if hit.size else size * 0.3
    ex, ez = cx + dirx * (shore + offshore), cz + dirz * (shore + offshore)
    yaw = np.arctan2(cz - ez, cx - ex)
    focal = 0.5 * width / np.tan(np.radians(fov_deg) * 0.5)
    horizon = int(height * 0.46)
    img = np.zeros((height, width, 3))
    rows = np.arange(height)[:, None]
    grad = np.clip((horizon - rows) / horizon, 0.0, 1.0)
    img[:] = f.lerp(SKY_LOW, SKY_TOP, grad[..., None] ** 0.7)
    img[horizon:] = f.lerp(np.array((0.20, 0.36, 0.46)), WATER * 2.2, np.clip((rows[horizon:] - horizon) / (height - horizon), 0, 1)[..., None] ** 0.5)
    ybuf = np.full(width, height, dtype=np.int64)
    cols = np.arange(width)
    ang = yaw + np.arctan((cols - width * 0.5) / focal)
    cosd = np.cos(ang - yaw)
    dist = 2.0
    far = shore + offshore + size
    while dist < far:
        wx_, wz_ = ex + np.cos(ang) * dist / cosd, ez + np.sin(ang) * dist / cosd
        ix = (wx_ / cell).astype(int)
        iz = (wz_ / cell).astype(int)
        ok = (ix >= 0) & (ix < n) & (iz >= 0) & (iz < n)
        hh = np.where(ok, h[np.clip(iz, 0, n - 1), np.clip(ix, 0, n - 1)], -99.0)
        ground = np.maximum(hh, 0.0)
        sy = (horizon - (ground - eye_height) * focal / dist).astype(np.int64)
        sy = np.clip(sy, 0, height)
        fog = 1.0 - np.exp(-dist * 0.0007)
        col = colour[np.clip(iz, 0, n - 1), np.clip(ix, 0, n - 1)]
        col = f.lerp(col, SKY_LOW, fog)
        # The sea occludes too: a water sample advances the column's horizon without drawing,
        # so land behind it only fills what rises above it.
        covers = (sy < ybuf) & ((hh > 0.0) | (dist < far))
        draw = ok & (hh > 0.0) & covers
        for c in np.nonzero(draw)[0]:
            img[sy[c]:ybuf[c], c] = col[c]
        ybuf = np.where(covers, sy, ybuf)
        dist += max(cell * 0.6, dist * 0.004)
    return Image.fromarray((_srgb(img * 0.9) * 255).astype(np.uint8))
