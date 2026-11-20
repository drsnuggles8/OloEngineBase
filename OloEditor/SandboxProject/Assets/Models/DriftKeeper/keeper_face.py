"""Cranial planes, open sockets, eyelids and the articulated oral aperture."""

from math import atan2, cos, exp, log1p, pi, sin, sqrt
import numpy as np

EYE_Y = 1.634
EYE_Z = 0.0765
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


# Cross-sections from the philtrum to the glabella. Each transverse station
# records the relief at the septum, dome, soft triangle, ala and cheek. The
# underside doubles back in height beneath the tip before joining the lip.
NOSE_U = np.array([0, 0.19, 0.38, 0.54, 0.72, 1.0])
NOSE_Y = np.array([1.577, 1.581, 1.584, 1.588, 1.592, 1.598, 1.605, 1.617, 1.630, 1.644, 1.657])
NOSE_DEPTH = 0.001 * np.array(
    [
        [0, 0, 0, 0, 0, 0],
        [5, 3, 1, 2, 0, 0],
        [13, 10, 5, 7, 2, 0],
        [22, 19, 13, 14, 5, 0],
        [30, 28, 23, 18, 7, 0],
        [32, 30, 24, 14, 5, 0],
        [28, 24, 16, 8, 2, 0],
        [22, 17, 9, 3, 0, 0],
        [16, 12, 5, 1, 0, 0],
        [7, 5, 2, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
    ]
)
NOSE_DROP = 0.001 * np.array(
    [
        [0, 0, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
        [-1, 0, 2, 1, 0, 0],
        [-2.7, -1.7, 1, 0, 0, 0],
        [-5.2, -3.6, -0.65, 0, 0, 0],
        [-4.8, -3.5, -0.8, 0, 0, 0],
        [-2, -1, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
        [0, 0, 0, 0, 0, 0],
    ]
)


def cubic_section(nodes, values, slopes, t):
    i = min(len(nodes) - 2, max(0, int(np.searchsorted(nodes, t)) - 1))
    span = nodes[i + 1] - nodes[i]
    u = (t - nodes[i]) / span
    return (
        (1 + 2 * u) * (1 - u) ** 2 * values[i]
        + u * (1 - u) ** 2 * span * slopes[i]
        + u * u * (3 - 2 * u) * values[i + 1]
        + u * u * (u - 1) * span * slopes[i + 1]
    )


def field_slopes(values, nodes):
    slopes = np.gradient(values, nodes, axis=0)
    slopes[[0, -1]] = 0
    return slopes


NASAL_SECTION = np.stack((NOSE_DEPTH, NOSE_DROP), axis=-1)
NASAL_Y_SLOPES = field_slopes(NASAL_SECTION, NOSE_Y)
NASAL_X_SLOPES = np.swapaxes(field_slopes(np.swapaxes(NASAL_SECTION, 0, 1), NOSE_U), 0, 1)
NASAL_CROSS_SLOPES = field_slopes(NASAL_X_SLOPES, NOSE_Y)


def nose_section(x, y):
    u = abs(x - 0.0015 * exp(-(((y - 1.596) / 0.025) ** 2))) / 0.034
    if u >= 1 or not NOSE_Y[0] < y < NOSE_Y[-1]:
        return 0.0, 0.0
    values = cubic_section(NOSE_Y, NASAL_SECTION, NASAL_Y_SLOPES, y)
    slopes = cubic_section(NOSE_Y, NASAL_X_SLOPES, NASAL_CROSS_SLOPES, y)
    return cubic_section(NOSE_U, values, slopes, u)


def lid_margin(side, a, pad=0):
    """Upper lid hangs over the iris. Both canthi meet on the same sloping fissure."""
    u, v = cos(a), sin(a)
    dx = 0.0120 * u
    slant = 0.00105 * side * u - 0.00065
    if v > 0:
        arch = (0.00315 + 0.00030 * side) * (1 - 0.16 * side * u) * v**1.30
    else:
        arch = -0.00330 * (-v) ** 1.40 * (1 + 0.13 * side * u)
    dy = slant + arch + pad * max(v, 0)
    radius = EYE_R + 0.00048
    return np.array(
        (side * EYE_X + dx, EYE_Y + dy, EYE_Z + sqrt(max(0, radius * radius - dx * dx - dy * dy)))
    )


def mouth_seam(x):
    """The upper central tubercle overlaps the lower lip at rest."""
    return (
        MOUTH_Y
        - 0.00125 * np.exp(-((x / 0.0058) ** 2))
        + 0.00062 * np.exp(-(((abs(x) - 0.010) / 0.0052) ** 2))
        - 0.00195 * (x / 0.026) ** 2
        + 0.00066 * x / 0.026
    )


def head_point(angle, y):
    w, front, back = skull_section(y)
    c = cos(angle)
    x = w * sin(angle)
    ax = abs(x)
    z = -0.008 + (front + 0.008 if c > 0 else back - 0.008) * c
    # The gonion sits above and behind the chin. Raising it gives the lower
    # edge of the jaw a diagonal in profile even before the beard grows.
    hinge = float(np.clip((abs(angle) - 0.36) / 1.65, 0, 1))
    hinge = hinge * hinge * (3 - 2 * hinge)
    height = y + 0.030 * hinge * exp(-(((y - 1.541) / 0.042) ** 2))
    if c > 0:
        z += 0.012 * c * (1 - c * c)
        asym = 0.0014 * np.tanh(x / 0.016)
        brow = 1.650 - 0.065 * ax + asym
        relief = (
            0.009 * bump(ax, y, 0.025, brow, 0.027, 0.011)
            + 0.004 * bump(ax, y, 0.051, brow - 0.003, 0.020, 0.014)
            - 0.0035 * bump(ax, y, 0.033, EYE_Y, 0.026, 0.014)
            + 0.0068 * bump(ax, y, 0.041, 1.608 + 0.29 * (ax - 0.034), 0.033, 0.022)
            - 0.0037 * bump(ax, y, 0.055, 1.579 + 0.48 * (ax - 0.037), 0.026, 0.023)
            + 0.004 * bump(ax, y, 0.056, 1.554, 0.017, 0.020)
            + 0.005 * bump(ax, y, 0.012, 1.569, 0.022, 0.018)
            - 0.002 * bump(ax, y, 0.021 + 0.25 * (1.587 - y), 1.575, 0.005, 0.017)
            + 0.003 * bump(ax, y, 0.004, 1.574, 0.003, 0.006)
            + 0.006 * bump(x, y, 0, 1.534, 0.026, 0.011)
            - 0.0025 * bump(x, y, 0, 1.548, 0.021, 0.004)
        )
        # The tear trough descends outwards from the inner canthus. Its fold
        # dissolves into the cheek instead of encircling both eyes as a rim.
        trough = 1.625 - 0.26 * (ax - 0.018) - 0.0012 * sin(95 * x)
        relief += 0.0021 * bump(ax, y, 0.029, trough + 0.002, 0.019, 0.005)
        relief -= 0.0011 * bump(ax, y, 0.029, trough - 0.003, 0.017, 0.003)
        # A second, shorter fold catches the cheek below the lateral lid.
        # Its slope and length differ on the two sides of the face.
        outer_fold = 1.616 + 0.27 * (ax - 0.041) + 0.0012 * sin(82 * x + 0.4)
        relief -= 0.00085 * bump(ax, y, 0.044, outer_fold, 0.014, 0.0018)
        relief += 0.00065 * bump(ax, y, 0.045, outer_fold + 0.0024, 0.012, 0.0028)
        for level in (1.666, 1.674, 1.684):
            relief -= 0.00065 * bump(ax, y, 0.027, level + 0.12 * ax + 0.0015 * sin(x * 63), 0.033, 0.00135)
        for offset in (-0.004, 0, 0.0035):
            relief -= 0.00065 * bump(ax, y, 0.052, 1.632 + offset + (ax - 0.048) * offset * 70, 0.009, 0.0013)
        mouth = mouth_seam(x)
        bow = 0.0021 * exp(-(((ax - 0.0064) / 0.0052) ** 2))
        upper = 0.0043 * bump(x, y, 0, mouth + 0.0022 + bow, 0.025, 0.0038)
        lower = 0.0048 * bump(ax, y, 0.008, mouth - 0.0032, 0.015, 0.0045)
        relief += upper + lower - 0.0023 * bump(ax, y, 0.025, mouth - 0.001, 0.005, 0.005)
        # The lateral cheek breaks towards the modiolus and jowl in a long
        # oblique fold. Its fading width makes the transition soft on the neck.
        fold_x = 0.021 + 0.41 * (1.589 - y)
        relief -= 0.0026 * bump(ax, y, fold_x, 1.573, 0.0038, 0.018)
        relief += 0.0044 * bump(ax, y, fold_x + 0.008, 1.576, 0.011, 0.020)
        relief -= 0.0018 * bump(ax, y, 0.030 + 0.13 * (1.557 - y), 1.549, 0.004, 0.012)
        # Slightly fuller right cheek and off-centre chin break the bilateral
        # highlight without moving the sockets or their optical axes.
        relief += 0.0021 * bump(x, y, -0.041, 1.603, 0.028, 0.023) - 0.0013 * bump(
            x, y, 0.043, 1.579, 0.023, 0.020
        )
        x += 0.0015 * bump(x, y, 0, 1.535, 0.043, 0.013) * min(1, 2 * c)
        z += relief * min(1, 2 * c)
        nasal, dy = nose_section(x, y)
        z += nasal
        height += dy
        rho = sqrt((ax - EYE_X) ** 2 + (y - EYE_Y) ** 2)
        radius = EYE_R + 0.0005
        limit = 0.86 * radius
        depth = sqrt(radius**2 - min(rho, limit) ** 2)
        shell = EYE_Z + depth - max(0, rho - limit) * limit / depth
        # This envelope supports the lids on the globe. Extending its tangent
        # behind the outer orbit put a 30 mm step through the temple and hair.
        orbital = float(np.clip((0.032 - rho) / 0.010, 0, 1))
        z += orbital**2 * (3 - 2 * orbital) * 0.0018 * log1p(exp(min(40, (shell - z) / 0.0018)))
    seat = exp(-(((abs(angle) - 1.71) / 0.22) ** 4) - ((y - 1.625) / 0.023) ** 4)
    x -= np.sign(x) * 0.005 * seat
    return np.array((x, height, z))


def face_motion_fade(depth):
    """Keep frontal expressions unchanged and fade them behind the jaw hinge."""
    t = float(np.clip((depth + 0.033) / 0.025, 0, 1))
    return t * t * (3 - 2 * t)


def smile_displacement(point):
    """The mouth corner lifts back into the cheek. Whiskers follow the same skin."""
    x, y, z = point
    ax = abs(x)
    corner = bump(ax, y, 0.026, MOUTH_Y, 0.016, 0.013)
    cheek = bump(ax, y, 0.040, 1.589 + 0.44 * (ax - 0.036), 0.023, 0.024)
    fade = face_motion_fade(z) * float(np.clip((1.628 - y) / 0.014, 0, 1))
    strength = 0.93 + 0.07 * np.tanh(x / 0.015)
    return fade * strength * np.array((
        np.sign(x) * (0.0042 * corner + 0.0017 * cheek),
        0.0058 * corner + 0.0024 * cheek,
        -0.0017 * corner + 0.0020 * cheek,
    ))


def lower_waterline(surface, side, lod):
    # A wet ridge pools between the globe and lower lid. Taper its ends
    # into the canthi and carry the entire cross-section through a blink.
    cols = (38, 22, 12)[lod]
    for j in range(4):
        for i in range(cols + 1):
            phase = i / cols
            angle = pi + 0.09 + (pi - 0.18) * phase
            margin = lid_margin(side, angle)
            half_width = 0.00028 * (0.10 + 0.90 * sin(pi * phase) ** 0.7)
            dx, dy = margin[0] - side * EYE_X, margin[1] - EYE_Y
            closed_y = -0.0015 + 0.00105 * side * cos(angle)
            r = EYE_R + 0.00048
            depth_delta = max(0, EYE_Z + sqrt(max(0, r * r - dx * dx - closed_y * closed_y)) - margin[2])
            margin[1] += half_width * (2 * j / 3 - 1)
            margin[2] += half_width * (0.3 + 0.7 * sin(pi * j / 3))
            index = surface.vertex(margin, (phase, 1 - j / 3), "head")
            move = np.array([0, closed_y - dy, depth_delta])
            surface.shapes["BlinkL" if side > 0 else "BlinkR"][index] = move
            surface.shapes["Squint"][index] = 0.55 * move
            if i and j:
                surface.quad("KeeperWetLid", index - cols - 2, index - cols - 1, index, index - 1)


def jaw_influence(point):
    x, y, z = point
    # Grip the chin and its whiskers below the lower lip. Their falloff
    # starts at the commissures and hinges, not in front of the incisors.
    vertical = float(np.clip((mouth_seam(min(0.026, max(-0.026, x))) + 0.008 - y) / 0.011, 0, 1))
    lateral = float(np.clip((0.079 - abs(x)) / 0.048, 0, 1))
    return face_motion_fade(z) * vertical**2 * (3 - 2 * vertical) * lateral**2 * (3 - 2 * lateral)


def auricle(surface, side, lod):
    """One folded skin shell from the auditory opening through the helix to the skull."""
    cols = (70, 36, 20)[lod]
    rows = (24, 14, 9)[lod]
    grid = []
    for j in range(rows + 1):
        ring = []
        r = j / rows
        for i in range(cols):
            a = 2 * pi * i / cols
            h = r * cos(a)
            f = r * sin(a)
            # Breadth reaches its maximum in the upper half. The small lobe
            # turns forward and the upper helix leans back over the concha.
            y = 1.624 + 0.0318 * h - 0.0019 * r * max(-h, 0)
            z = -0.019 + 0.0164 * f * (1 + 0.24 * h) - 0.0072 * h - 0.0016 * h * h
            w, front, back = skull_section(y)
            c = (z + 0.008) / (front + 0.008 if z > -0.008 else back - 0.008)
            root = w * sqrt(max(0, 1 - c * c)) - 0.0028
            helix = 0.0048 * exp(-(((r - 0.86) / 0.135) ** 2)) * (0.70 + 0.30 * cos(a - 0.8))
            anti = 0.0039 * exp(-(((f + 0.18 + 0.22 * h) / 0.19) ** 2) - ((h + 0.03) / 0.49) ** 2)
            fork = 0.0027 * exp(-(((f - 0.42 * h) / 0.15) ** 2) - ((h - 0.39) / 0.20) ** 2)
            tragus = 0.0054 * exp(-(((f - 0.46) / 0.21) ** 2) - ((h + 0.11) / 0.22) ** 2)
            cup = -0.0049 * exp(-(((f - 0.06) / 0.33) ** 2) - ((h + 0.04) / 0.33) ** 2)
            lobe = 0.0032 * exp(-(((h + 0.76) / 0.23) ** 2) - (f / 0.39) ** 2)
            # The rear half stands off the skull, but the front root and the
            # earlobe blend back into it as the rim rolls through its edge.
            stand = 0.0066 - 0.0039 * f + 0.0015 * h
            x = side * (root + stand + helix + anti + fork + tragus + cup + lobe)
            ring.append(surface.vertex((x, y, z), (0.12 + 0.052 * f, 0.52 - 0.16 * h), "head"))
        grid.append(ring)
    last = grid[-1]
    for t in (0.35, 0.70, 1.0):
        backring = []
        for i, idx in enumerate(last):
            x, y, z = surface.points[idx]
            a = 2 * pi * i / cols
            fall = 0.0093 - 0.0050 * sin(a) + 0.0014 * cos(a)
            # Unroll the return behind the helix into an annulus around the
            # face of the ear. Its chart width follows the local return depth.
            chart_radius = 1 + t * fall / 0.030
            uv = (0.12 + 0.052 * chart_radius * sin(a), 0.52 - 0.16 * chart_radius * cos(a))
            backring.append(
                surface.vertex((x - side * fall * t, y - 0.0018 * t, z - 0.0029 * t), uv, "head")
            )
        grid.append(backring)
    for j in range(1, len(grid)):
        for i in range(cols):
            n = (i + 1) % cols
            a, b, c, e = grid[j - 1][i], grid[j - 1][n], grid[j][n], grid[j][i]
            if side > 0:
                b, e = e, b
            # Darken only the floor of the recessed meatus.
            mat = "KeeperEar" if (j - 0.5) / rows < 0.22 else "KeeperSkin"
            surface.quad(mat, a, b, c, e)


def dental_arch(surface, upper, lod):
    # Central incisor through first molar. Depth recedes around the corners
    # of the jaw, and the lower incisors sit just inside the upper arch.
    centres = [
        (0.0040, 0.0835), (0.0111, 0.0822), (0.0171, 0.0792),
        (0.0212, 0.0738), (0.0240, 0.0674), (0.0260, 0.0592),
    ]
    widths = (0.0081, 0.0064, 0.0068, 0.0069, 0.0073, 0.0093)
    sizes = np.array(centres) * [1.0 if upper else 0.91, 1.0]
    if not upper:
        sizes[:, 1] -= np.linspace(0.0042, 0.0020, len(sizes))
    bone = "head" if upper else "jaw"
    direction = 1 if upper else -1
    pieces = []
    sides = (16, 10, 6)[lod]
    heights = (
        (0, 0.025, 0.09, 0.24, 0.45, 0.70, 0.91, 0.975, 1),
        (0, 0.035, 0.19, 0.63, 0.94, 1),
        (0, 0.06, 0.34, 0.88, 1),
    )[lod]
    rear = sizes[-1] + 0.70 * (sizes[-1] - sizes[-2])
    gum_side = np.vstack((sizes, rear))
    arch = np.vstack((gum_side[::-1] * [-1, 1], gum_side))
    tangents = np.gradient(arch, axis=0)
    gum_height = MOUTH_Y + direction * 0.0084

    def gum(a, v):
        station = v * (len(arch) - 1)
        idx = min(int(station), len(arch) - 2)
        fraction = station - idx
        x, z = (1 - fraction) * arch[idx] + fraction * arch[idx + 1]
        tangent = (1 - fraction) * tangents[idx] + fraction * tangents[idx + 1]
        normal = np.array([-tangent[1], tangent[0]]) / np.linalg.norm(tangent)
        radius = 0.0031 * min(1, station, len(arch) - 1 - station)
        x, z = [x, z] + normal * (radius * cos(a) - 0.0011)
        return x, gum_height + radius * sin(a), z

    pieces.extend(surface.grid("KeeperGum", sides, 2 * (len(arch) - 1), gum, bone, invert=True))
    for idx, (x, z) in enumerate(sizes):
        tangent = sizes[min(idx + 1, len(sizes) - 1)] - sizes[max(0, idx - 1)]
        tangent /= np.linalg.norm(tangent)
        for side in (-1, 1):
            across = np.array([tangent[0], side * tangent[1]])
            front = np.array([-across[1], across[0]])

            def crown(a, v):
                # The worn edge is broad on the incisors and comes to a
                # single offset cusp on the canine. The neck tapers into gum.
                t = heights[round((v if upper else 1 - v) * (len(heights) - 1))]
                span = np.interp(t, [0, 0.025, 0.19, 0.72, 0.95, 1], [0, 0.86, 1, 0.96, 0.71, 0])
                c, s = cos(a), sin(a)
                width = 0.5 * widths[idx] * (1 if upper else 0.91)
                flank = np.sign(s) * abs(s) ** 0.63
                depth = (0.0019 if idx < 3 else 0.0034) * np.sign(c) * abs(c) ** 0.68
                position = np.array([side * x, z]) + span * (width * flank * across + depth * front)
                cusp = 0.0018 * (1 - abs(flank)) if idx == 2 else 0.00042 * cos(a + 0.6 * side)
                # The incisors overlap in height with horizontal clearance.
                # Behind the canine the opposing biting surfaces face one
                # another, so their cusps stop short of the occlusal plane.
                edge = ((-0.0028 if upper else -0.0023) + 0.00065 * idx) if idx < 2 else 0.0025
                y = MOUTH_Y + direction * ((1 - t) * edge + t * 0.0092 - span * cusp * (1 - t) ** 3)
                return position[0], y + 0.00022 * sin(3 * idx + side), position[1]

            pieces.extend(surface.grid("KeeperTeeth", sides, len(heights) - 1, crown, bone))
    return pieces


def face(surface, lod):
    skin_start = len(surface.points)
    # Patch boundaries coincide with grid lines at every resolution. The wound
    # edges are bridged into concentric skin loops surrounding each aperture.
    holes = [(-0.72, -0.18, 1.615, 1.652, -1), (0.18, 0.72, 1.615, 1.652, 1), (-0.48, 0.48, 1.545, 1.579, 0)]
    for side in (-1, 1):
        limits = sorted([side * 0.068, side * 0.236])
        holes.append((*limits, 1.5815, 1.5895, 2 * side))
    # Concentrate vertices along the alar rim, commissures and eyelids.
    # Uniform latitude rings left some near-view cartilage only one quad wide.
    angles = set()
    for lo, hi, steps in [
        (0, 0.32, (42, 14, 7)),
        (0.32, 0.82, (33, 15, 7)),
        (0.82, 1.27, (14, 6, 3)),
        (1.27, 2.05, (19, 8, 4)),
        (2.05, pi, (21, 10, 5)),
    ]:
        for a in np.linspace(lo, hi, steps[lod] + 1):
            angles.update((a, -a))
    angles = sorted(angles | {a for lo, hi, _, _, _ in holes for a in (lo, hi)})
    ys = set()
    for lo, hi, steps in [
        (1.518, 1.545, (13, 5, 3)),
        (1.545, 1.578, (32, 13, 7)),
        (1.578, 1.615, (47, 17, 9)),
        (1.615, 1.655, (39, 14, 7)),
        (1.655, 1.700, (19, 8, 4)),
        (1.700, 1.750, (19, 8, 5)),
    ]:
        ys.update(np.linspace(lo, hi, steps[lod] + 1))
    ys = sorted(ys | {y for _, _, lo, hi, _ in holes for y in (lo, hi)})
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
                if abs(side) == 2:
                    sign = side // 2
                    # Cut an oblique opening in the underside of each wing.
                    tip_x = 0.001 + sign * 0.0109 + 0.0037 * u
                    tip_y = 1.5856 + 0.0015 * v + 0.00085 * u * sign
                    tip_a = np.arcsin(tip_x / skull_section(tip_y)[0])
                    aa = angles[k] * (1 - f) + tip_a * f
                    yy = ys[j] * (1 - f) + tip_y * f
                    p = head_point(aa, yy)
                    p[2] += 0.00070 * sin(pi * f) ** 2
                    i = surface.vertex(p, ((aa + pi) / (2 * pi), (1.75 - yy) / 0.232), "head")
                elif side:
                    radius = EYE_R + 0.00048
                    centre = np.array((side * EYE_X, EYE_Y, EYE_Z))
                    edge = lid_margin(side, a)
                    p = outside * (1 - f) + edge * f
                    # Wrap the entire band over the sphere, folding the superior
                    # lid into a crease just below the brow.
                    base_z = head_point(np.arcsin(np.clip(p[0] / skull_section(p[1])[0], -1, 1)), p[1])[2]
                    # The outer loops use the same cranial field as the face.
                    # Only the final millimetre rolls into the wet lid margin.
                    edge_z = head_point(np.arcsin(edge[0] / skull_section(edge[1])[0]), edge[1])[2]
                    distance_xy = np.linalg.norm(p[:2] - edge[:2])
                    p[2] = base_z + (edge[2] - edge_z) * f * f * (3 - 2 * f) * exp(
                        -((distance_xy / 0.007) ** 2)
                    )
                    distance = abs(p[1] - edge[1])
                    roll = (0.0017 if v > 0 else 0.00065) * exp(-(((distance - 0.0025) / 0.0024) ** 2))
                    crease = (0.00135 if v > 0 else 0.00052) * exp(-(((distance - 0.0066) / 0.0019) ** 2))
                    p[2] += (roll - crease) * abs(v) * sin(pi * f) ** 0.8
                    # Keep the tarsal fold outside the globe, including the
                    # strip between rings when the upper lid is hooded.
                    r2 = (radius + 0.00025) ** 2 - (p[0] - centre[0]) ** 2 - (p[1] - centre[1]) ** 2
                    if r2 > 0 and f < 1:
                        p[2] = max(p[2], centre[2] + sqrt(r2))
                    i = surface.vertex(
                        p,
                        ((np.arcsin(p[0] / skull_section(p[1])[0]) + pi) / (2 * pi), (1.75 - p[1]) / 0.232),
                        "head",
                    )
                    closed_y = EYE_Y - 0.0015 + 0.00105 * side * u
                    drop = (closed_y - edge[1]) * f
                    dxp, dyp = p[0] - centre[0], p[1] + drop - centre[1]
                    cap = radius**2 - dxp**2 - dyp**2
                    dz = max(0, centre[2] + sqrt(cap) - p[2]) if cap > 0 else 0
                    surface.shapes["BlinkL" if side == 1 else "BlinkR"][i] = (0, drop, dz)
                    surface.shapes["Squint"][i] = (0, 0.55 * drop, 0.55 * dz)
                    surface.shapes["BrowRaise"][i] = (0, 0.004 * (1 - f) * max(v, 0), 0)
                else:
                    dx = 0.026 * u
                    dy = (0.00026 if v > 0 else 0.00020) * v + mouth_seam(dx) - MOUTH_Y
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

        if abs(side) == 2:
            # The dark interior is concave and recessed behind the rim.
            centre = np.mean([surface.points[i] for i in previous], axis=0)
            for depth in (0.0014, 0.0045):
                ring = []
                for (k, j), i in zip(border, previous):
                    p = np.array(surface.points[i])
                    p += (centre - p) * (0.12 if depth < 0.002 else 0.46)
                    p[1] += depth * 0.52
                    p[2] -= depth
                    ring.append(surface.vertex(p, surface.uvs[i], "head"))
                for h in range(len(ring)):
                    n = (h + 1) % len(ring)
                    surface.quad("KeeperNostril", previous[h], previous[n], ring[n], ring[h])
                previous = ring
            centre = np.mean([surface.points[i] for i in previous], axis=0)
            centre[2] -= 0.001
            cap = surface.vertex(centre, (0.5, 0.5), "head")
            surface.faces["KeeperNostril"].extend(
                (i, previous[(h + 1) % len(previous)], cap) for h, i in enumerate(previous)
            )

    for side in (-1, 1):

        def caruncle(angle, t):
            # Moving radially out from one central point fills the nasal corner
            # with a slender warm pad, widest beside the canthus.
            u = 0.5 + 0.5 * t * cos(angle)
            dx = -side * (0.0108 + 0.00135 * (2 * u - 1))
            dy = -0.00160 + 0.00070 * t * sin(angle)
            z = EYE_Z + sqrt(max(0, (EYE_R + 0.00065) ** 2 - dx * dx - dy * dy))
            return (side * EYE_X + dx, EYE_Y + dy, z)

        surface.grid("KeeperEar", (24, 16, 10)[lod], 3, caruncle, "head", invert=side < 0)

        auricle(surface, side, lod)
        lower_waterline(surface, side, lod)
    for i in range(skin_start, len(surface.points)):
        move = smile_displacement(surface.points[i])
        if np.dot(move, move) > 1e-12:
            surface.shapes["Smile"][i] = move
    # Dental arcades and tongue lie behind the open lip loops. Lower tissue
    # follows the same aperture targets as the lower lip.
    for upper in (False, True):
        y = MOUTH_Y + (0.0047 if upper else -0.0040)
        pieces = dental_arch(surface, upper, lod)
        if not upper:
            pieces.extend(
                surface.oval("KeeperTongue", (0, y - 0.002, 0.076), (0.019, 0.0038, 0.010), "jaw", 24, 10)
            )
            for i in pieces:
                for name, gap in (("JawOpen", 0.014), ("AA", 0.009), ("OO", 0.006), ("EE", 0.001)):
                    surface.shapes[name][i] = (0, -gap, -0.001)
    surface.oval("KeeperNostril", (0, 1.557, 0.061), (0.029, 0.019, 0.009), "head")
