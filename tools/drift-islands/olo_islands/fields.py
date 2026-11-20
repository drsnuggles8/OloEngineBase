"""Scalar fields on a square grid: seeded gradient noise and the few operators the recipes share.

Everything works in METRES. A grid of n x n samples covers a tile of `size` metres; `xz(n, size)`
returns the sample coordinates, x along columns and z along rows, which is also how the engine
reads a heightmap (row 0 is the tile's z = 0 edge).
"""
import numpy as np
from scipy import ndimage


def xz(n, size):
    c = (np.arange(n, dtype=np.float64) + 0.5) * (size / n)
    return np.meshgrid(c, c)


def _hash(ix, iz, seed):
    with np.errstate(over="ignore"):  # 64-bit wrap-around is the hash
        h = (ix.astype(np.uint64) * np.uint64(0x9E3779B185EBCA87)) ^ (iz.astype(np.uint64) * np.uint64(0xC2B2AE3D27D4EB4F))
        h ^= np.uint64(seed % (1 << 63)) * np.uint64(0x165667B19E3779F9)
        h ^= h >> np.uint64(29)
        h *= np.uint64(0xBF58476D1CE4E5B9)
        h ^= h >> np.uint64(32)
    return h


def gradient_noise(x, z, seed):
    """Perlin gradient noise in about [-1, 1], one octave, lattice spacing 1."""
    x0 = np.floor(x)
    z0 = np.floor(z)
    fx = x - x0
    fz = z - z0
    ix = x0.astype(np.int64)
    iz = z0.astype(np.int64)

    def corner(dx, dz):
        h = _hash(ix + dx, iz + dz, seed)
        ang = (h >> np.uint64(40)).astype(np.float64) * (2.0 * np.pi / float(1 << 24))
        return np.cos(ang) * (fx - dx) + np.sin(ang) * (fz - dz)

    u = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    v = fz * fz * fz * (fz * (fz * 6.0 - 15.0) + 10.0)
    a = corner(0, 0) * (1.0 - u) + corner(1, 0) * u
    b = corner(0, 1) * (1.0 - u) + corner(1, 1) * u
    return (a * (1.0 - v) + b * v) * 1.41421356


def fbm(x, z, seed, wavelength, octaves=5, gain=0.5, lacunarity=2.03):
    """Fractal sum, roughly [-1, 1]. `wavelength` is the first octave's, in metres."""
    total = np.zeros_like(x, dtype=np.float64)
    amp = 1.0
    norm = 0.0
    f = 1.0 / wavelength
    for o in range(octaves):
        total += amp * gradient_noise(x * f + 17.3 * o, z * f - 9.1 * o, seed + 101 * o)
        norm += amp
        amp *= gain
        f *= lacunarity
    return total / norm


def ridged(x, z, seed, wavelength, octaves=5, gain=0.5, lacunarity=2.03):
    """Ridged multifractal in [0, 1]: sharp crests where the noise crosses zero."""
    total = np.zeros_like(x, dtype=np.float64)
    amp = 1.0
    norm = 0.0
    f = 1.0 / wavelength
    weight = np.ones_like(x, dtype=np.float64)
    for o in range(octaves):
        r = 1.0 - np.abs(gradient_noise(x * f + 31.7 * o, z * f + 5.9 * o, seed + 211 * o))
        r = r * r * weight
        weight = np.clip(r * 2.0, 0.0, 1.0)
        total += amp * r
        norm += amp
        amp *= gain
        f *= lacunarity
    return total / norm


def warp(x, z, seed, wavelength, amount, octaves=3):
    """Displace coordinates by a vector noise field: what turns a drawn outline into a coast."""
    return (x + amount * fbm(x, z, seed, wavelength, octaves),
            z + amount * fbm(x, z, seed + 7919, wavelength, octaves))


def smoothstep(a, b, v):
    """Hermite step from a to b; the edges may be arrays and may run downhill (a > b)."""
    span = np.where(np.abs(np.asarray(b) - a) < 1e-12, 1e-12, np.asarray(b) - a)
    t = np.clip((v - a) / span, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def blur(a, sigma_cells):
    return ndimage.gaussian_filter(a, sigma_cells, mode="nearest") if sigma_cells > 0 else a


def slope_degrees(h, cell):
    gz, gx = np.gradient(h, cell)
    return np.degrees(np.arctan(np.hypot(gx, gz)))


def signed_shore_distance(h, cell, level=0.0):
    """Metres to the waterline: positive inland, negative offshore."""
    land = h > level
    inside = ndimage.distance_transform_edt(land) * cell
    outside = ndimage.distance_transform_edt(~land) * cell
    return np.where(land, inside - 0.5 * cell, -(outside - 0.5 * cell))


def segment_coords(x, z, ax, az, bx, bz):
    """Along-track fraction t in [0, 1] and signed perpendicular offset s (metres, + to the left of a->b)."""
    dx, dz = bx - ax, bz - az
    length2 = dx * dx + dz * dz
    t = np.clip(((x - ax) * dx + (z - az) * dz) / length2, 0.0, 1.0)
    px, pz = ax + t * dx, az + t * dz
    side = np.sign((x - ax) * dz - (z - az) * dx)
    dist = np.hypot(x - px, z - pz)
    return t, -side * dist, dist


def polyline_field(x, z, pts, value_key=2):
    """Nearest-segment evaluation over a polyline of (x, z, value, ...) control points.

    Returns (distance, signed offset, interpolated value arrays for every extra column).
    """
    best = None
    for a, b in zip(pts[:-1], pts[1:]):
        t, s, d = segment_coords(x, z, a[0], a[1], b[0], b[1])
        vals = [a[k] + (b[k] - a[k]) * t for k in range(value_key, len(a))]
        if best is None:
            best = [d, s] + vals
        else:
            closer = d < best[0]
            best[0] = np.where(closer, d, best[0])
            best[1] = np.where(closer, s, best[1])
            for i, v in enumerate(vals):
                best[2 + i] = np.where(closer, v, best[2 + i])
    return best


def bump(x, z, cx, cz, radius, power=2.0):
    """A smooth radial bump: 1 at the centre, 0 at `radius`."""
    r = np.hypot(x - cx, z - cz) / radius
    return np.clip(1.0 - r ** power, 0.0, 1.0)


def plug(x, z, cx, cz, radius, wall, seed=0, wobble=0.0):
    """A flat-topped column mask: 1 inside `radius`, falling to 0 across `wall` metres."""
    r = np.hypot(x - cx, z - cz)
    if wobble > 0.0:
        r = r + wobble * fbm(x, z, seed, radius * 1.3, 3)
    return 1.0 - smoothstep(radius, radius + wall, r)
