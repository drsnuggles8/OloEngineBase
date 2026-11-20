"""Cranial planes, open sockets, eyelids and the articulated oral aperture."""

from math import atan2, cos, exp, log1p, pi, sin, sqrt
import numpy as np

EYE_Y = 1.634
EYE_Z = 0.0795
EYE_X = 0.032
EYE_R = 0.0126
MOUTH_Y = 1.561
# Width, anterior depth and posterior depth at anatomical stations from menton
# to vertex. The mandible projects forward beneath the mouth.
STATIONS = np.array(
    [
        (1.518, 0.016, 0.061, -0.009),
        (1.526, 0.040, 0.082, 0.026),
        (1.540, 0.059, 0.090, 0.051),
        (1.563, 0.069, 0.088, 0.077),
        (1.586, 0.075, 0.086, 0.093),
        (1.612, 0.080, 0.086, 0.104),
        (1.634, 0.079, 0.084, 0.111),
        (1.650, 0.080, 0.091, 0.113),
        (1.680, 0.081, 0.094, 0.110),
        (1.703, 0.077, 0.087, 0.102),
        (1.726, 0.064, 0.068, 0.084),
        (1.742, 0.041, 0.042, 0.056),
        (1.750, 0.002, 0.002, 0.003),
    ]
)


def bump(x, y, cx, cy, sx, sy):
    return exp(-(((x - cx) / sx) ** 2 + ((y - cy) / sy) ** 2))


SLOPES = np.gradient(STATIONS[:, 1:], STATIONS[:, 0], axis=0)


def skull_section(y):
    i = int(np.clip(np.searchsorted(STATIONS[:, 0], y) - 1, 0, len(STATIONS) - 2))
    span = STATIONS[i + 1, 0] - STATIONS[i, 0]
    t = (y - STATIONS[i, 0]) / span
    return (
        (1 + 2 * t) * (1 - t) ** 2 * STATIONS[i, 1:]
        + t * (1 - t) ** 2 * span * SLOPES[i]
        + t * t * (3 - 2 * t) * STATIONS[i + 1, 1:]
        + t * t * (t - 1) * span * SLOPES[i + 1]
    )


def head_point(angle, y):
    w, front, back = skull_section(y)
    c = cos(angle)
    x = w * sin(angle)
    ax = abs(x)
    z = -0.008 + (front + 0.008 if c > 0 else back - 0.008) * c
    if c > 0:
        # Brow shelf, hollow temples and orbits, zygoma, nose bridge and alae.
        relief = (
            0.009 * bump(ax, y, 0.031, 1.651 - 0.08 * ax, 0.029, 0.009)
            - 0.010 * bump(ax, y, 0.032, EYE_Y, 0.019, 0.011)
            + 0.009 * bump(ax, y, 0.042, 1.613, 0.023, 0.013)
            - 0.005 * bump(ax, y, 0.058, 1.577, 0.015, 0.023)
            + 0.022 * bump(x, y, 0, 1.619, 0.0095, 0.034)
            + 0.029 * bump(x, y, 0, 1.596, 0.015, 0.011)
            + 0.015 * bump(ax, y, 0.014, 1.590, 0.0067, 0.007)
            - 0.004 * bump(ax, y, 0.010, 1.585, 0.0035, 0.003)
            + 0.004 * bump(ax, y, 0.004, 1.575, 0.0028, 0.007)
            - 0.003 * bump(ax, y, 0.024 + (1.585 - y) * 0.30, 1.572, 0.003, 0.019)
            + 0.004 * bump(x, y, 0, 1.535, 0.023, 0.007)
        )
        # Crow's feet, transverse forehead lines and lower orbital crease.
        for level, tilt in [(1.668, 0.12), (1.675, 0.10), (1.683, 0.09)]:
            relief -= 0.0013 * bump(ax, y, 0.022, level + tilt * ax, 0.036, 0.0010)
        relief -= 0.0018 * bump(ax, y, 0.034, 1.623 + 4 * (ax - 0.031) ** 2, 0.013, 0.0014)
        # Vermilion rolls round a shallow arched aperture. Evaluate the volume
        # in facial coordinates so its highlight crosses the outer mesh seam.
        mouth_line = MOUTH_Y + 0.001 * (ax / 0.026) ** 2
        bow = 0.0016 * exp(-(((ax - 0.007) / 0.0045) ** 2))
        relief += 0.0065 * bump(x, y, 0, mouth_line + 0.0027 + bow, 0.023, 0.0033)
        relief += 0.0078 * bump(x, y, 0, mouth_line - 0.0034, 0.022, 0.0036)
        relief -= 0.0017 * bump(ax, y, 0.025, mouth_line, 0.004, 0.003)
        z += relief * min(1, 2 * c)
        # The ridge continues round the orbit into the malar plane.
        # Without that volume a flat annulus reads as a rectangular eye patch.
        z += 0.010 * bump(ax, y, EYE_X, EYE_Y, 0.024, 0.016)
        # Continuous lid and orbital fat-pad field across the patch seam.
        # Continue the spherical shell tangentially before its vertical rim.
        rho = sqrt((ax - EYE_X) ** 2 + (y - EYE_Y) ** 2)
        radius = EYE_R + 0.00046
        limit = 0.89 * radius
        height = sqrt(radius**2 - min(rho, limit) ** 2)
        globe = EYE_Z + height - max(0, rho - limit) * limit / height
        z += 0.0025 * log1p(exp(min(40, (globe - z) / 0.0025)))
        z += 0.0027 * bump(ax, y, 0.033, 1.623 + 0.10 * (ax - 0.031), 0.020, 0.0048)
        z -= 0.0011 * bump(ax, y, 0.030, 1.617 + 0.16 * (ax - 0.031), 0.017, 0.0015)
        for offset in (-0.004, 0.000, 0.004):
            z -= 0.0013 * bump(ax, y, 0.053, 1.633 + offset + (ax - 0.048) * offset * 72, 0.009, 0.0011)
    return np.array((x, y, z))


def face_motion_fade(depth):
    """Keep frontal expressions unchanged and fade them behind the jaw hinge."""
    t = float(np.clip((depth + 0.033) / 0.025, 0, 1))
    return t * t * (3 - 2 * t)


def jaw_influence(point):
    x, y, z = point
    return face_motion_fade(z) * bump(x, y, 0, 1.535, 0.065, 0.018)


def face(surface, lod):
    # Patch boundaries coincide with grid lines at every resolution. The wound
    # edges are bridged into concentric skin loops surrounding each aperture.
    holes = [(-0.72, -0.18, 1.615, 1.652, -1), (0.18, 0.72, 1.615, 1.652, 1), (-0.48, 0.48, 1.545, 1.579, 0)]
    angles = sorted(
        set(
            np.linspace(-pi, pi, (193, 77, 35)[lod]).tolist()
            + [a for lo, hi, _, _, _ in holes for a in (lo, hi)]
        )
    )
    ys = sorted(
        set(
            np.linspace(1.518, 1.750, (131, 49, 27)[lod]).tolist()
            + [y for _, _, lo, hi, _ in holes for y in (lo, hi)]
        )
    )
    grid = {}

    def head_vertex(a, y):
        p = head_point(a, y)
        i = surface.vertex(p, ((a + pi) / (2 * pi), (1.75 - y) / 0.232), "head")
        # Carry the front's deformation through the temple and jaw hinge.
        # A hard front/back cut produced a 9 mm step in an open jaw.
        fade = face_motion_fade(p[2])
        if fade > 0:
            surface.shapes["BrowRaise"][i] = (
                0,
                fade * 0.005 * bump(abs(p[0]), y, 0.032, 1.653, 0.025, 0.012),
                0,
            )
            lower = jaw_influence(p)
            for name, size in (("JawOpen", 0.018), ("AA", 0.010), ("OO", 0.004)):
                surface.shapes[name][i] = (0, -size * lower, -size * 0.22 * lower)
        return i

    for j, y in enumerate(ys):
        for k, a in enumerate(angles):
            grid[k, j] = head_vertex(a, y)
            if (
                j
                and k
                and not any(
                    lo < (a + angles[k - 1]) / 2 < hi and bot < (y + ys[j - 1]) / 2 < top
                    for lo, hi, bot, top, _ in holes
                )
            ):
                surface.quad("KeeperSkin", grid[k - 1, j - 1], grid[k, j - 1], grid[k, j], grid[k - 1, j])
    for lo, hi, bot, top, side in holes:
        left, right = angles.index(lo), angles.index(hi)
        low, high = ys.index(bot), ys.index(top)
        border = (
            [(k, low) for k in range(left, right)]
            + [(right, j) for j in range(low, high)]
            + [(k, high) for k in range(right, left, -1)]
            + [(left, j) for j in range(high, low, -1)]
        )
        previous = [grid[key] for key in border]
        rim = np.array([surface.points[i] for i in previous])
        steps = (12, 7, 4)[lod]
        for step in range(1, steps + 1):
            f = step / steps
            ring = []
            for (k, j), outside in zip(border, rim):
                a = atan2(2 * (ys[j] - bot) / (top - bot) - 1, 2 * (angles[k] - lo) / (hi - lo) - 1)
                u, v = cos(a), sin(a)
                if side:
                    dx = 0.0119 * u
                    dy = (0.0049 if v > 0 else 0.0035) * v + 0.0006 * side * u
                    radius = EYE_R + 0.00045
                    centre = np.array((side * EYE_X, EYE_Y, EYE_Z))
                    edge = centre + (dx, dy, sqrt(max(0, radius**2 - dx**2 - dy**2)))
                    p = outside * (1 - f) + edge * f
                    # Wrap the entire band over the sphere, folding the superior
                    # lid into a crease just below the brow.
                    base_z = head_point(np.arcsin(np.clip(p[0] / skull_section(p[1])[0], -1, 1)), p[1])[2]
                    # The outer loops use the same cranial field as the face.
                    # Only the final millimetre rolls into the wet lid margin.
                    p[2] = base_z + (edge[2] - base_z) * f**6
                    p[2] += 0.0007 * sin(pi * f) ** 2 * max(v, 0)
                    i = surface.vertex(
                        p,
                        ((np.arcsin(p[0] / skull_section(p[1])[0]) + pi) / (2 * pi), (1.75 - p[1]) / 0.232),
                        "head",
                    )
                    closed_y = EYE_Y - 0.0005 + side * 0.0006 * u
                    drop = (closed_y - edge[1]) * f
                    dxp, dyp = p[0] - centre[0], p[1] + drop - centre[1]
                    cap = radius**2 - dxp**2 - dyp**2
                    dz = max(0, centre[2] + sqrt(cap) - p[2]) if cap > 0 else 0
                    surface.shapes["BlinkL" if side == 1 else "BlinkR"][i] = (0, drop, dz)
                    surface.shapes["Squint"][i] = (0, 0.55 * drop, 0.55 * dz)
                    surface.shapes["BrowRaise"][i] = (0, 0.004 * (1 - f) * max(v, 0), 0)
                else:
                    dx = 0.026 * u
                    cupid = 0.0011 * exp(-(((abs(u) - 0.27) / 0.20) ** 2))
                    dy = (0.0008 if v > 0 else 0.0005) * v + 0.0009 * u * u + cupid * max(v, 0)
                    edge = np.array((dx, MOUTH_Y + dy, 0.091 - 0.007 * u * u))
                    p = outside * (1 - f) + edge * f
                    base_z = head_point(np.arcsin(p[0] / skull_section(p[1])[0]), p[1])[2]
                    p[2] = base_z
                    chart = (
                        (np.arcsin(p[0] / skull_section(p[1])[0]) + pi) / (2 * pi),
                        (1.75 - p[1]) / 0.232,
                    )
                    i = surface.vertex(p, chart, "head")
                    lower = max(0, -v) ** 0.65
                    for name, width, gap, push in [
                        ("JawOpen", 0, 0.017, -0.003),
                        ("AA", 0.002, 0.011, -0.001),
                        ("EE", 0.005, 0.002, 0),
                        ("OO", -0.013, 0.008, 0.006),
                    ]:
                        outer_weight = jaw_influence(outside)
                        jaw = (
                            {"JawOpen": 0.018, "AA": 0.010, "OO": 0.004}.get(name, 0) * outer_weight * (1 - f)
                        )
                        surface.shapes[name][i] = (
                            width * u * f,
                            -gap * lower * f - jaw,
                            push * f - 0.22 * jaw,
                        )
                    surface.shapes["Smile"][i] = (0.004 * u * f, 0.006 * abs(u) ** 2 * f, 0)
                ring.append(i)
            for h in range(len(ring)):
                n = (h + 1) % len(ring)
                surface.quad(
                    "KeeperLip" if not side and f >= 0.65 else "KeeperSkin",
                    previous[h],
                    previous[n],
                    ring[n],
                    ring[h],
                )
            previous = ring

    for side in (-1, 1):
        # The pinna is one cupped shell. Its outer rim curls back toward the
        # skull and the antihelix rises inside the bowl, splitting above it.
        def pinna(a, r):
            h = r * cos(a)
            fore = r * sin(a)
            y = 1.623 + 0.032 * h
            z = -0.017 + 0.016 * fore * (1 + 0.18 * h) - 0.006 * h
            helix = 0.007 * exp(-(((r - 0.85) / 0.13) ** 2))
            anti = 0.005 * exp(-(((fore + 0.22 + 0.25 * h) / 0.15) ** 2) - ((h + 0.01) / 0.53) ** 2)
            fork = 0.003 * exp(-(((fore - 0.20 * h) / 0.14) ** 2) - ((h - 0.41) / 0.24) ** 2)
            bowl = -0.005 * exp(-(((fore - 0.15) / 0.36) ** 2) - (h / 0.42) ** 2)
            return (side * (0.081 + 0.005 * h + 0.005 * (1 - r * r) + helix + anti + fork + bowl), y, z)

        s_count = (56, 32, 18)[lod]
        surface.grid("KeeperSkin", s_count, (18, 12, 7)[lod], pinna, "head", invert=side > 0)

        def pinna_back(a, t):
            x, y, z = pinna(a, 1)
            return (x - side * 0.009 * t, y - 0.002 * t, z - 0.001 * t)

        surface.grid("KeeperSkin", s_count, 2, pinna_back, "head", invert=side > 0)
        surface.oval("KeeperEar", (side * 0.082, 1.621, -0.014), (0.0018, 0.005, 0.0035), "head", 16, 9)
        surface.oval("KeeperSkin", (side * 0.087, 1.620, -0.008), (0.003, 0.006, 0.004), "head", 16, 10)
        nostril = head_point(side * 0.14, 1.586)
        surface.oval("KeeperNostril", nostril + np.array((0, 0, 0.0004)), (0.0032, 0.00125, 0.0007), "head")
    # Dental arcades and tongue lie behind the open lip loops. Lower tissue
    # follows the same aperture targets as the lower lip.
    for upper in (False, True):
        y = MOUTH_Y + (0.0047 if upper else -0.0040)
        pieces = list(
            surface.oval("KeeperGum", (0, y, 0.073), (0.025, 0.005, 0.012), "jaw" if not upper else "head")
        )
        tooth_count = (12, 8, 4)[lod]
        for tooth in range(tooth_count):
            t = 2 * (tooth + 0.5) / tooth_count - 1
            pieces.extend(
                surface.oval(
                    "KeeperTeeth",
                    (t * 0.023, y + (-0.003 if upper else 0.0025), 0.083 - 0.011 * t * t),
                    (0.027 / tooth_count, 0.0034, 0.0024),
                    "head" if upper else "jaw",
                    12,
                    8,
                )
            )
        if not upper:
            pieces.extend(
                surface.oval("KeeperTongue", (0, y - 0.002, 0.076), (0.019, 0.0038, 0.010), "jaw", 24, 10)
            )
            for i in pieces:
                for name, gap in (("JawOpen", 0.014), ("AA", 0.009), ("OO", 0.006), ("EE", 0.001)):
                    surface.shapes[name][i] = (0, -gap, -0.001)
    surface.oval("KeeperNostril", (0, 1.557, 0.061), (0.029, 0.019, 0.009), "head")
