"""Operators that turn a landform into an island that meets the sea: beaches, rock platforms,
the tile border, and the fixed sites the landmarks stand on.

Heights are metres relative to the sea surface (y = 0). `BASE` is the tile's own floor: every
border texel of the tile lands on exactly this height, because the scene's seafloor plane sits
2 m below it and a tile edge that stood any higher would be a wall in the water
(docs/agent-rules/terrain-tile-meets-ocean.md).
"""
import numpy as np

from .fields import blur, fbm, lerp, signed_shore_distance, smoothstep

BASE = -24.0


def submarine(depth_metres, offshore, scale):
    """Seabed height at `offshore` metres outside a coast: falls toward BASE over about `scale`."""
    return -depth_metres * (1.0 - np.exp(-np.maximum(offshore, 0.0) / scale))


def beach_profile(d):
    """Equilibrium beach height at signed shore distance d (metres, + inland).

    A 7 deg foreshore up to a 1.6 m berm, a nearly level backshore, and offshore a 5 deg
    nearshore shelf to -2 m that steepens to 10 deg. Measured against what replaced it: the old
    islands met the sea at 45-60 deg.
    """
    inland = np.minimum(d * 0.1228, 1.6 + np.maximum(d - 13.0, 0.0) * 0.026)
    off = -np.minimum(-d * 0.0875, 2.0 + np.maximum(-d - 23.0, 0.0) * 0.176)
    return np.where(d >= 0.0, inland, off)


def apply_beaches(h, cell, beach, seed, x, z, inland=18.0):
    """Pull the shore zone toward the beach profile where `beach` (0..1) says there is sand.

    `inland` is how far behind the waterline the profile holds before the land takes over again:
    a narrow cay needs a few metres, or the whole cay is flattened into its own beach.
    """
    if beach is None or beach.max() <= 0.0:
        return h
    d = signed_shore_distance(h, cell)
    band = smoothstep(-90.0, -40.0, d) * (1.0 - smoothstep(inland, inland * 2.3, d))
    target = beach_profile(d) + 0.12 * fbm(x, z, seed, 9.0, 3) * smoothstep(0.5, 3.0, d)
    return lerp(h, target, np.clip(beach, 0.0, 1.0) * band)


def apply_rock_platform(h, cell, rock_shore, seed, x, z, width=8.0, level=0.6):
    """Cut a wave platform into a rock coast: a ledge just above the water, a step behind it.

    The ledge width follows a noise field so the shore is not a uniform kerb, and it only ever
    LOWERS the land (a platform is cut, never built).
    """
    if rock_shore is None or rock_shore.max() <= 0.0:
        return h
    d = signed_shore_distance(h, cell)
    # From nothing (the cliff foot stands in the sea) to twice the nominal width (a skerry apron).
    w = width * 2.0 * smoothstep(-0.25, 0.75, fbm(x, z, seed, 58.0, 3)) + 0.4
    ledge = level + 0.05 * np.maximum(d, 0.0) + 0.25 * fbm(x, z, seed + 3, 5.0, 3)
    release = smoothstep(w, w + 2.2, d)  # past the ledge the cap lifts and the cliff stands
    cap = lerp(ledge, 1.0e4, release)
    cut = np.minimum(h, cap)
    return lerp(h, cut, np.clip(rock_shore, 0.0, 1.0) * (d > 0.0))


def apply_border(h, x, z, size, cell, ramp=26.0):
    """Drive the tile's rim to BASE exactly: the outermost texel centres get weight 0."""
    edge = np.minimum(np.minimum(x, size - x), np.minimum(z, size - z))
    m = smoothstep(0.5 * cell, ramp, edge)
    out = lerp(BASE, np.maximum(h, BASE), m)
    out[0, :] = out[-1, :] = BASE
    out[:, 0] = out[:, -1] = BASE
    return out


def stamp(h, x, z, cx, cz, target, flat, blend, mode="set"):
    """Blend the ground toward `target` inside `flat` metres of (cx, cz), fading out by `blend`.

    `target` may be a scalar or an array. mode "min" only lowers (a channel), "max" only raises.
    """
    w = 1.0 - smoothstep(flat, blend, np.hypot(x - cx, z - cz))
    t = target
    if mode == "min":
        t = np.minimum(h, target)
    elif mode == "max":
        t = np.maximum(h, target)
    return lerp(h, t, w)


def box_weight(x, z, x0, x1, z0, z1, feather):
    """1 inside the rectangle, 0 `feather` metres outside it."""
    dx = np.maximum(np.maximum(x0 - x, x - x1), 0.0)
    dz = np.maximum(np.maximum(z0 - z, z - z1), 0.0)
    return 1.0 - smoothstep(0.0, feather, np.hypot(dx, dz))


def channel(h, x, z, ax, az, bx, bz, half_width, depth, feather=6.0):
    """Keep at least `depth` of water along the segment a->b (a boat's track)."""
    dx, dz = bx - ax, bz - az
    t = np.clip(((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz), 0.0, 1.0)
    dist = np.hypot(x - (ax + t * dx), z - (az + t * dz))
    w = 1.0 - smoothstep(half_width, half_width + feather, dist)
    return lerp(h, np.minimum(h, -depth), w)


def soften(h, sigma_cells, keep):
    """Blur where `keep` is 0, leave alone where it is 1 (rounds a modelled crest, not a cliff)."""
    return lerp(blur(h, sigma_cells), h, np.clip(keep, 0.0, 1.0))
