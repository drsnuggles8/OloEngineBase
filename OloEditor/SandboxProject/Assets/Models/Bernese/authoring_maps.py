"""Deterministic Bernese coat and iris map authoring."""
import numpy as np

def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def _srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

COAT_BLACK = (0.019, 0.019, 0.020)

def raster_uv(uv, col, size, reach=1.5, chunk=1500000):
    """Per-corner values `col` (T, 3, C) over the UV triangles `uv` (T, 3, 2), rasterised into a
    size x size image whose rows run from v = 0 (Blender's pixel order). A texel whose centre lies in a
    triangle takes the value there; one within `reach` texels of a triangle and inside none takes the
    value at the triangle's nearest point. Every bilinear tap a point of a triangle reads is then the
    triangle's own value, however small its island: the smart projection's islands lie 8 texels apart.
    Returns the image and the mask of texels written; fill_empty_texels does the rest.

    The Cycles bake this replaces wrote only the texels whose centres an island covered, and the edge
    texels it trusted least were eroded away two deep -- which emptied every island narrower than five
    texels, the slivers the material contours cut beside the eyes, the lips and the ears. The fill then
    gave them a colour from wherever the nearest island in the atlas was: on the golden's even gold,
    nothing to see; on a Bernese, white and rust flecks in the black (#1558)."""
    P = np.asarray(uv, dtype=np.float64) * size - 0.5  # texel centres on the integers
    col = np.asarray(col, dtype=np.float64)
    lo = np.clip(np.floor(P.min(axis=1) - reach).astype(np.int64), 0, size - 1)
    hi = np.clip(np.ceil(P.max(axis=1) + reach).astype(np.int64), 0, size - 1)
    w = hi[:, 0] - lo[:, 0] + 1
    h = hi[:, 1] - lo[:, 1] + 1
    n = w * h
    ends = np.cumsum(n)
    best = np.full(size * size, np.inf)
    out = np.zeros((size * size, col.shape[2]))
    t0 = 0
    while t0 < len(P):
        t1 = max(int(np.searchsorted(ends, (ends[t0 - 1] if t0 else 0) + chunk, side="right")), t0 + 1)
        ts = np.arange(t0, t1)
        nn = n[ts]
        ti = np.repeat(ts, nn)
        k = np.arange(int(nn.sum())) - np.repeat(np.cumsum(nn) - nn, nn)
        cx = lo[ti, 0] + k % w[ti]
        cy = lo[ti, 1] + k // w[ti]
        q = np.stack([cx, cy], axis=1).astype(np.float64)
        a, b, c = P[ti, 0], P[ti, 1], P[ti, 2]
        v0, v1, v2 = b - a, c - a, q - a
        d00 = np.einsum("ij,ij->i", v0, v0)
        d01 = np.einsum("ij,ij->i", v0, v1)
        d11 = np.einsum("ij,ij->i", v1, v1)
        d20 = np.einsum("ij,ij->i", v2, v0)
        d21 = np.einsum("ij,ij->i", v2, v1)
        den = d00 * d11 - d01 * d01
        ok = np.abs(den) > 1e-18
        den = np.where(ok, den, 1.0)
        l1 = (d11 * d20 - d01 * d21) / den
        l2 = (d00 * d21 - d01 * d20) / den
        bary = np.stack([1.0 - l1 - l2, l1, l2], axis=1)
        inside = ok & np.all(bary >= 0.0, axis=1)
        dist = np.zeros(len(q))
        out_i = np.nonzero(~inside)[0]
        if len(out_i):
            qa = q[out_i]
            corners = (a[out_i], b[out_i], c[out_i])
            bd = np.full(len(out_i), np.inf)
            bb = np.zeros((len(out_i), 3))
            for i, j in ((0, 1), (1, 2), (2, 0)):
                e = corners[j] - corners[i]
                t = np.clip(np.einsum("ij,ij->i", qa - corners[i], e) / np.maximum(np.einsum("ij,ij->i", e, e), 1e-18),
                            0.0, 1.0)
                dd = np.linalg.norm(qa - (corners[i] + t[:, None] * e), axis=1)
                nb = np.zeros((len(out_i), 3))
                nb[:, i] = 1.0 - t
                nb[:, j] = t
                closer = dd < bd
                bd = np.where(closer, dd, bd)
                bb = np.where(closer[:, None], nb, bb)
            dist[out_i] = bd
            bary[out_i] = bb
        keep = np.nonzero(dist <= reach)[0]
        idx = (cy * size + cx)[keep]
        dk = dist[keep]
        order = np.lexsort((dk, idx))
        idx, dk, keep = idx[order], dk[order], keep[order]
        first = np.ones(len(idx), dtype=bool)
        first[1:] = idx[1:] != idx[:-1]
        idx, dk, keep = idx[first], dk[first], keep[first]
        better = dk < best[idx]
        idx, keep = idx[better], keep[better]
        best[idx] = dk[better]
        out[idx] = np.einsum("ij,ijk->ik", bary[keep], col[ti[keep]])
        t0 = t1
    return out.reshape(size, size, -1), np.isfinite(best).reshape(size, size)

IRIS_TEX_SIZE = 2048

IRIS_LIMBUS = 9.6 / 12.0

IRIS_PUPIL = 3.5 / 12.0

IRIS_SCLERA_LINEAR = (0.78, 0.74, 0.71)

IRIS_PUPILLARY = (0.33, 0.18, 0.070)

IRIS_CILIARY = (0.22, 0.115, 0.044)

IRIS_OUTER = (0.095, 0.048, 0.019)

IRIS_PUPIL_COLOUR = (0.018, 0.014, 0.012)

IRIS_RUFF = (0.09, 0.05, 0.025)

def _periodic_value_noise(rng, ang, rad, n_ang, n_rad):
    """Bilinear value noise on (angle, radius), periodic in angle: n_ang cells around, n_rad out."""
    grid = rng.random((n_rad + 2, n_ang)).astype(np.float32)
    a = (ang / (2.0 * np.pi) % 1.0) * n_ang
    r = np.clip(rad, 0.0, 1.0) * n_rad
    a0 = np.floor(a).astype(np.int64)
    r0 = np.floor(r).astype(np.int64)
    fa = (a - a0).astype(np.float32)
    fr = (r - r0).astype(np.float32)
    fa = fa * fa * (3.0 - 2.0 * fa)
    fr = fr * fr * (3.0 - 2.0 * fr)
    a1 = (a0 + 1) % n_ang
    a0 = a0 % n_ang
    r1 = r0 + 1
    g00, g01 = grid[r0, a0], grid[r0, a1]
    g10, g11 = grid[r1, a0], grid[r1, a1]
    return (g00 + (g01 - g00) * fa) * (1.0 - fr) + (g10 + (g11 - g10) * fa) * fr

def iris_albedo(size=IRIS_TEX_SIZE, seed=1533):
    """The globe's albedo in its planar UV (u, v) = 0.5 + 0.5 (x, y) across the gaze: sRGB, (H, W, 3)."""
    rng = np.random.default_rng(seed)
    t = (np.arange(size, dtype=np.float32) + 0.5) / size * 2.0 - 1.0
    x, y = np.meshgrid(t, t)
    rs = np.hypot(x, y)  # the lateral offset in globe radii: the ocular model's iris-plane coordinate
    ang = np.arctan2(y, x)
    ri = rs / IRIS_LIMBUS  # 0 at the gaze, 1 at the limbus
    pupil = IRIS_PUPIL / IRIS_LIMBUS
    # The pupil's margin wobbles a little, as a real one does.
    margin = pupil * (1.0 + 0.012 * (_periodic_value_noise(rng, ang, np.zeros_like(ri), 24, 1) - 0.5))
    # The collarette: a wavy ring a third of the way from the pupil to the limbus.
    wobble = _periodic_value_noise(rng, ang, np.zeros_like(ri), 40, 1) - 0.5
    collar = pupil + 0.30 * (1.0 - pupil) + 0.03 * wobble
    zone = np.clip((ri - pupil) / np.maximum(1.0 - pupil, 1e-3), 0.0, 1.0)  # 0 pupil edge, 1 limbus
    lin = lambda c: _srgb_to_linear(np.array(c, dtype=np.float64)).astype(np.float32)
    inner, mid, outer = lin(IRIS_PUPILLARY), lin(IRIS_CILIARY), lin(IRIS_OUTER)
    in_pupillary = _smoothstep(collar + 0.035, collar - 0.035, ri)[..., None]
    t_out = _smoothstep(0.30, 1.0, zone)[..., None]
    base = mid[None, None, :] * (1.0 - t_out) + outer[None, None, :] * t_out
    base = base * (1.0 - in_pupillary) + inner[None, None, :] * in_pupillary
    # The fibres: long radial streaks, many around and few out, in three octaves; the finest is what
    # a close-up resolves, the coarse ones are the trabeculae a mid-shot reads.
    fib = (0.45 * _periodic_value_noise(rng, ang, zone, 160, 3) +
           0.35 * _periodic_value_noise(rng, ang + 0.013, zone, 420, 7) +
           0.20 * _periodic_value_noise(rng, ang + 0.029, zone, 1100, 14))
    fibres = 0.45 + 1.10 * fib  # 0.45 .. 1.55
    # A few light trabeculae, gold flecks in the brown, more of them inside the collarette.
    flecks = np.clip(_periodic_value_noise(rng, ang + 0.4, zone, 90, 5) - 0.72, 0.0, 1.0) / 0.28
    fleck_gain = 1.0 + 0.55 * flecks * (0.6 + 0.4 * in_pupillary[..., 0])
    # Contraction furrows: faint dark rings across the outer ciliary zone.
    furrow = 1.0 - 0.18 * np.exp(-((zone - 0.62) / 0.025) ** 2) - 0.14 * np.exp(-((zone - 0.78) / 0.02) ** 2)
    # The collarette's ridge, a little lighter.
    ridge = np.exp(-((ri - collar) / 0.022) ** 2)
    # Crypts: dark lozenges in the ciliary zone, just outside the collarette.
    crypt = np.ones_like(ri)
    for _ in range(34):
        ca = rng.uniform(-np.pi, np.pi)
        cr = rng.uniform(0.04, 0.42) * (1.0 - collar.mean()) + collar.mean() + 0.025
        da = np.angle(np.exp(1j * (ang - ca))) * ri
        dr = ri - cr
        crypt *= 1.0 - 0.55 * np.exp(-(da / 0.02) ** 2 - (dr / 0.045) ** 2)
    iris = base * (fibres * crypt * furrow * fleck_gain)[..., None] * (1.0 + 0.30 * ridge)[..., None]
    # The ruff: a thin dark frill of pigment round the pupil, then the pupil itself, crisp.
    ruff = _smoothstep(margin + 0.035, margin + 0.008, ri)[..., None]
    iris = iris * (1.0 - ruff) + lin(IRIS_RUFF)[None, None, :] * ruff
    in_pupil = _smoothstep(margin + 0.004, margin - 0.004, ri)[..., None]
    iris = iris * (1.0 - in_pupil) + lin(IRIS_PUPIL_COLOUR)[None, None, :] * in_pupil
    iris = np.clip(iris, 0.0, 1.0)
    # The limbus: the iris ends at ri = 1 (the model's own ring darkens the edge band on top).
    in_iris = _smoothstep(1.03, 0.97, ri)[..., None]
    out = iris * in_iris + np.array(IRIS_SCLERA_LINEAR, dtype=np.float32)[None, None, :] * (1.0 - in_iris)
    return np.clip(np.where(out <= 0.0031308, out * 12.92, 1.055 * np.power(out, 1.0 / 2.4) - 0.055), 0.0, 1.0)
