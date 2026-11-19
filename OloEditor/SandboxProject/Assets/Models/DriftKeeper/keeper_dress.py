"""The work clothes, hands and storm lantern. Distances in metres."""

from math import cos, exp, pi, sin
import numpy as np

PALETTE = {
    "KeeperSkin": ((0.50, 0.245, 0.163), 0.54, 0),
    "KeeperEar": ((0.36, 0.116, 0.080), 0.62, 0),
    "KeeperLip": ((0.37, 0.134, 0.106), 0.42, 0),
    "KeeperGum": ((0.32, 0.085, 0.065), 0.57, 0),
    "KeeperTongue": ((0.46, 0.145, 0.121), 0.39, 0),
    "KeeperTeeth": ((0.76, 0.65, 0.45), 0.32, 0),
    "KeeperNostril": ((0.045, 0.010, 0.009), 0.83, 0),
    "KeeperNail": ((0.54, 0.29, 0.21), 0.38, 0),
    "KeeperOilskin": ((0.42, 0.215, 0.049), 0.48, 0),
    "KeeperSeam": ((0.245, 0.118, 0.029), 0.52, 0),
    "KeeperGansey": ((0.035, 0.074, 0.087), 0.93, 0),
    "KeeperTwill": ((0.062, 0.072, 0.077), 0.88, 0),
    "KeeperCap": ((0.023, 0.047, 0.065), 0.82, 0),
    "KeeperLeather": ((0.047, 0.031, 0.022), 0.46, 0),
    "KeeperSole": ((0.021, 0.019, 0.018), 0.66, 0),
    "KeeperBrass": ((0.51, 0.31, 0.097), 0.38, 0.76),
    "KeeperGlass": ((0.55, 0.33, 0.089), 0.16, 0.15),
    "KeeperWick": ((0.95, 0.58, 0.19), 0.33, 0),
    "KeeperSclera": ((0.77, 0.73, 0.65), 0.14, 0),
}


def mix_weights(value, low, high, first, second):
    t = float(np.clip((value - low) / (high - low), 0, 1))
    return {first: 1 - t, second: t}


def add_hand(s, sign, suffix):
    # Hands hang with palms facing the thighs. Longitudinal differences and
    # bent distal phalanges keep the fingertips from forming a single edge.
    hx = sign * 0.252
    s.loft(
        "KeeperSkin",
        [
            (0.852, hx, 0.055, 0.017, 0.038),
            (0.869, hx, 0.056, 0.023, 0.044),
            (0.904, hx, 0.052, 0.025, 0.043),
            (0.932, hx, 0.050, 0.020, 0.030),
            (0.952, hx, 0.052, 0.020, 0.027),
        ],
        "hand_" + suffix,
        26,
    )
    for f, (offset, length) in enumerate([(-0.028, 0.060), (-0.009, 0.072), (0.011, 0.076), (0.030, 0.066)]):
        z = 0.054 + offset
        curl = 0.012 if suffix == "R" else 0.028
        base = 0.867 if f in (1, 2) else 0.864
        path = [
            (hx, base, z),
            (hx + sign * 0.002, base - 0.34 * length, z),
            (hx - sign * curl * 0.35, base - 0.68 * length, z + 0.002),
            (hx - sign * curl, base - length, z + 0.004),
            (hx - sign * (curl + 0.003), base - length - 0.002, z + 0.005),
        ]
        s.tube(
            "KeeperSkin",
            path,
            [(0.009, 0.009), (0.009, 0.009), (0.008, 0.008), (0.006, 0.007), (0.002, 0.003)],
            "hand_" + suffix,
            14,
        )
        s.oval(
            "KeeperNail",
            (path[-2][0] + sign * 0.005, path[-2][1] + 0.003, path[-2][2]),
            (0.002, 0.005, 0.004),
            "hand_" + suffix,
            12,
            8,
        )
    s.tube(
        "KeeperSkin",
        [
            (hx, 0.907, 0.079),
            (hx - sign * 0.009, 0.892, 0.099),
            (hx - sign * 0.019, 0.874, 0.106),
            (hx - sign * 0.027, 0.860, 0.105),
        ],
        [(0.012, 0.015), (0.012, 0.013), (0.010, 0.010), (0.006, 0.007)],
        "hand_" + suffix,
        16,
    )


def stagger_stance(s):
    for i, point in enumerate(s.points):
        x, y, z = point
        if y < 0.90 and x > 0:
            shift = 0.035 * min(1, max(0, (0.90 - y) / 0.40))
            s.points[i] = (x, y, z + shift)


def body(s, lod, anatomy=False):
    n = (80, 36, 18)[lod]

    def trunk(p):
        if p[1] < 1.16:
            return mix_weights(p[1], 0.96, 1.16, "pelvis", "spine")
        return mix_weights(p[1], 1.16, 1.34, "spine", "chest")

    def torso(mat, rows, weights=trunk, fluting=None):
        s.loft(mat, [(y, 0, z, w, d) for y, w, d, z in rows], weights, n, fluting)

    if anatomy:
        from keeper_anatomy import underlying_body

        underlying_body(s, trunk, mix_weights)
        for sign, suffix in ((1, "L"), (-1, "R")):
            add_hand(s, sign, suffix)
        stagger_stance(s)
        return

    torso(
        "KeeperSkin",
        [
            (0.86, 0.095, 0.086, 0),
            (1.00, 0.145, 0.113, 0),
            (1.16, 0.135, 0.100, 0),
            (1.34, 0.181, 0.121, 0),
            (1.42, 0.16, 0.098, 0),
            (1.47, 0.082, 0.068, 0),
        ],
    )
    torso(
        "KeeperSkin",
        [
            (1.43, 0.068, 0.061, -0.003),
            (1.48, 0.065, 0.062, -0.005),
            (1.54, 0.050, 0.062, -0.010),
            (1.57, 0.044, 0.056, -0.009),
        ],
        "neck",
    )

    def folds(a, y):
        hem = max(0, (1.21 - y) / 0.45)
        side = abs(sin(a)) ** 2
        drape = (0.002 + 0.007 * hem) * cos(9 * a + 7 * y) + 0.002 * sin(17 * a - 9 * y)
        crease = y - 1.12 - 0.055 * cos(a) - 0.028 * sin(2 * a)
        return drape + side * (
            0.008 * exp(-((crease / 0.011) ** 2)) - 0.005 * exp(-(((crease + 0.015) / 0.012) ** 2))
        )

    # Heavy oilcloth hangs from a broad shoulder yoke and opens over the knit.
    coat_rows = [
        (0.765, 0.215, 0.146, 0.0),
        (0.771, 0.217, 0.147, 0),
        (0.783, 0.215, 0.143, 0),
        (0.856, 0.209, 0.143, -0.001),
        (0.927, 0.194, 0.135, -0.003),
        (0.993, 0.162, 0.123, -0.002),
        (1.047, 0.157, 0.120, 0),
        (1.086, 0.155, 0.119, 0.002),
        (1.11, 0.163, 0.122, 0.003),
        (1.128, 0.158, 0.123, 0.004),
        (1.171, 0.161, 0.129, 0.004),
        (1.240, 0.177, 0.142, 0.003),
        (1.305, 0.195, 0.143, 0.002),
        (1.366, 0.208, 0.130, 0),
        (1.407, 0.205, 0.114, -0.002),
        (1.438, 0.178, 0.098, -0.003),
        (1.463, 0.101, 0.078, -0.002),
        (1.468, 0.078, 0.067, 0),
    ]
    torso("KeeperOilskin", coat_rows, trunk, folds)
    # Collar stands open at the throat; the tip rolls outward to shed water.
    torso(
        "KeeperGansey",
        [
            (1.436, 0.083, 0.072, 0),
            (1.455, 0.079, 0.071, 0),
            (1.482, 0.070, 0.065, 0),
            (1.490, 0.069, 0.064, 0),
            (1.491, 0.054, 0.053, -0.003),
        ],
        "chest",
        lambda a, y: 0.0015 * cos(38 * a),
    )

    def collar(a, t):
        gap = 0.28 + 0.60 * t
        ang = gap + (2 * pi - 2 * gap) * a / (2 * pi)
        reach = 1 - 0.65 * t
        return (
            (0.082 + 0.058 * reach) * sin(ang),
            1.442 + 0.054 * t - 0.029 * cos(ang) * reach,
            (0.073 + 0.032 * reach) * cos(ang),
        )

    s.grid("KeeperOilskin", n, 6, collar, "chest")

    def hem_line(angle):
        return [(w * sin(angle), y, z + d * cos(angle) + 0.006) for y, w, d, z in coat_rows[:-2]]

    path = hem_line(-0.115)
    s.tube("KeeperSeam", path, [(0.004, 0.002)] * len(path), trunk, 8)
    for y in (0.84, 0.937, 1.04, 1.15, 1.26, 1.365):
        h, w, d, z = min(coat_rows, key=lambda r: abs(r[0] - y))
        s.oval("KeeperBrass", (0.006, y, z + d + 0.007), (0.007, 0.007, 0.003), trunk, 12, 7)

    # Shoulder cape sheds spray past the sleeve heads. The hem dips on the
    # back; the offset between the two lower rings catches a narrow shadow.
    def cape(a, t):
        width = 0.207 - 0.125 * t
        depth = 0.142 - 0.068 * t
        y = 1.330 + 0.137 * t + 0.024 * cos(a) * (1 - t)
        return (width * sin(a), y, (depth + 0.003 * sin(6 * a)) * cos(a))

    s.grid("KeeperOilskin", n, 7, cape, "chest")

    def cape_edge(a, t):
        x, y, z = cape(a, 0)
        return (x * (1 + 0.008 * t), y + 0.004 * t, z * (1 + 0.009 * t))

    s.grid("KeeperSeam", n, 2, cape_edge, "chest")
    for sign, suffix in ((1, "L"), (-1, "R")):

        def part(name):
            return name + "_" + suffix

        x = sign * 0.099

        def leg(p):
            return mix_weights(p[1], 0.42, 0.54, part("shin"), part("thigh"))

        trouser = [
            (0.30, 0.044, 0.048, 0.002),
            (0.36, 0.052, 0.052, 0),
            (0.38, 0.055, 0.056, 0.002),
            (0.40, 0.060, 0.058, -0.003),
            (0.422, 0.078, 0.075, 0),
            (0.452, 0.081, 0.084, 0.007),
            (0.478, 0.077, 0.079, 0.011),
            (0.504, 0.076, 0.078, 0.007),
            (0.528, 0.085, 0.081, 0.001),
            (0.559, 0.083, 0.082, 0),
            (0.642, 0.090, 0.085, -0.007),
            (0.750, 0.087, 0.091, -0.006),
            (0.86, 0.079, 0.084, -0.009),
            (0.92, 0.073, 0.080, 0),
        ]
        s.loft(
            "KeeperTwill",
            [(y, x + sign * 0.016 * (0.92 - y), z, w, d) for y, w, d, z in trouser],
            leg,
            n // 2,
            lambda a, y: 0.0035 * cos(5 * a + 12 * y) + 0.0015 * sin(9 * a - 35 * y),
        )
        footx = x + sign * 0.012
        for mat, rows in [
            (
                "KeeperSole",
                [
                    (0.001, 0.002, 0.003, 0.040),
                    (0.002, 0.060, 0.134, 0.040),
                    (0.009, 0.063, 0.138, 0.040),
                    (0.024, 0.062, 0.138, 0.041),
                ],
            ),
            (
                "KeeperLeather",
                [
                    (0.023, 0.061, 0.135, 0.040),
                    (0.036, 0.064, 0.132, 0.043),
                    (0.053, 0.061, 0.119, 0.043),
                    (0.069, 0.055, 0.102, 0.038),
                    (0.099, 0.046, 0.074, 0.015),
                    (0.132, 0.048, 0.062, -0.004),
                    (0.145, 0.049, 0.062, -0.005),
                    (0.155, 0.046, 0.059, -0.005),
                    (0.166, 0.050, 0.060, -0.004),
                    (0.182, 0.047, 0.060, -0.006),
                    (0.215, 0.049, 0.061, -0.007),
                    (0.287, 0.058, 0.066, -0.005),
                    (0.351, 0.062, 0.072, -0.005),
                    (0.385, 0.065, 0.075, -0.005),
                    (0.393, 0.070, 0.078, -0.005),
                    (0.402, 0.070, 0.078, -0.005),
                    (0.405, 0.063, 0.069, -0.005),
                ],
            ),
        ]:
            s.loft(
                mat,
                [(y, footx, z, w, d) for y, w, d, z in rows],
                part("boot")
                if mat == "KeeperSole"
                else lambda p: mix_weights(p[1], 0.095, 0.175, part("boot"), part("shin")),
                n // 2,
            )
        # Side pockets with rolled welts follow the slope of the oilcloth.
        pocket = [(sign * 0.081, 0.956, 0.108), (sign * 0.118, 0.989, 0.095), (sign * 0.143, 1.018, 0.071)]
        s.tube("KeeperSeam", pocket, [(0.004, 0.004)] * 3, trunk)
        # Sleeves narrow at the elbow, wrinkle on its inside and break at the wrist.
        rows = [
            (1.437, 0.155, -0.006, 0.004, 0.008),
            (1.431, 0.162, -0.006, 0.042, 0.057),
            (1.405, 0.180, -0.004, 0.076, 0.082),
            (1.359, 0.204, 0, 0.078, 0.078),
            (1.291, 0.218, 0.006, 0.067, 0.070),
            (1.235, 0.231, 0.013, 0.059, 0.061),
            (1.180, 0.241, 0.019, 0.054, 0.059),
            (1.156, 0.242, 0.020, 0.062, 0.064),
            (1.141, 0.244, 0.021, 0.053, 0.056),
            (1.126, 0.246, 0.025, 0.059, 0.061),
            (1.102, 0.248, 0.026, 0.055, 0.057),
            (1.044, 0.250, 0.041, 0.052, 0.053),
            (0.992, 0.252, 0.050, 0.047, 0.047),
            (0.963, 0.251, 0.053, 0.046, 0.048),
            (0.947, 0.251, 0.053, 0.044, 0.046),
        ]

        def sleeve(p):
            if p[1] > 1.35:
                return mix_weights(p[1] - 0.48 * (abs(p[0]) - 0.16), 1.34, 1.48, part("upperarm"), "chest")
            return mix_weights(p[1], 1.10, 1.19, part("forearm"), part("upperarm"))

        s.loft(
            "KeeperOilskin",
            [(y, sign * xx, z, w, d) for y, xx, z, w, d in reversed(rows)],
            sleeve,
            n // 2,
            lambda a, y: 0.0025 * cos(6 * a + 10 * y),
        )
        s.loft(
            "KeeperSeam",
            [
                (0.945, sign * 0.251, 0.053, 0.044, 0.046),
                (0.948, sign * 0.251, 0.053, 0.046, 0.048),
                (0.958, sign * 0.251, 0.052, 0.046, 0.048),
            ],
            part("forearm"),
            n // 2,
        )
        add_hand(s, sign, suffix)
    stagger_stance(s)
    # Cap band, canted crown with an overhanging welt, peaked leather visor.
    torso(
        "KeeperCap",
        [
            (1.677, 0.061, 0.076, -0.007),
            (1.678, 0.080, 0.106, -0.007),
            (1.684, 0.082, 0.107, -0.007),
            (1.704, 0.084, 0.105, -0.006),
            (1.712, 0.091, 0.117, 0.002),
            (1.732, 0.097, 0.122, 0.006),
            (1.743, 0.097, 0.119, 0.007),
            (1.750, 0.092, 0.112, 0.005),
            (1.754, 0.078, 0.091, 0.001),
            (1.758, 0.003, 0.004, 0),
        ],
        "head",
    )
    torso(
        "KeeperLeather",
        [
            (1.680, 0.081, 0.107, -0.007),
            (1.682, 0.083, 0.109, -0.007),
            (1.691, 0.084, 0.109, -0.007),
            (1.694, 0.082, 0.107, -0.007),
        ],
        "head",
    )

    def visor(a, t):
        # Thin closed crescent in front of the cap.
        angle = (a / (2 * pi) - 0.5) * 2.85
        h = cos(angle / 1.425 * pi / 2)
        return (
            0.083 * sin(angle) * (1 + 0.10 * t),
            1.686 - 0.010 * t * h - 0.004 * sin(angle) ** 2,
            -0.006 + 0.107 * cos(angle) + 0.037 * t * h,
        )

    s.grid("KeeperLeather", n, 4, visor, "head", invert=True)

    def visor_under(a, t):
        x, y, z = visor(a, t)
        return (x, y - 0.0022, z)

    s.grid("KeeperLeather", n, 4, visor_under, "head")

    def visor_edge(a, t):
        x, y, z = visor(a, 1)
        return (x, y - 0.0022 * t, z)

    s.grid("KeeperLeather", n, 1, visor_edge, "head", invert=True)
    cord = [
        (0.085 * sin(t), 1.696 - 0.008 * cos(t) ** 2, 0.109 * cos(t) - 0.003)
        for t in np.linspace(-1.50, 1.50, 36)
    ]
    s.tube("KeeperSeam", cord, [(0.0018, 0.0018)] * len(cord), "head", 8)
    s.oval("KeeperBrass", (0, 1.711, 0.112), (0.010, 0.010, 0.002), "head", 18, 12)
    # Crown over a shanked anchor in low relief on the cap device.
    for path in [
        [(0, 1.716, 0.116), (0, 1.704, 0.116)],
        [(-0.004, 1.712, 0.116), (0.004, 1.712, 0.116)],
        [
            (-0.005, 1.708, 0.116),
            (-0.004, 1.705, 0.116),
            (0, 1.703, 0.116),
            (0.004, 1.705, 0.116),
            (0.005, 1.708, 0.116),
        ],
    ]:
        s.tube("KeeperBrass", path, [(0.0008, 0.0008)] * len(path), "head", 8)
    for sign in (-1, 1):
        s.oval("KeeperBrass", (sign * 0.081, 1.694, 0.019), (0.003, 0.005, 0.005), "head")
    # The bail arcs up through the curled left fingers; the frame surrounds
    # the smaller glowing burner so the side rods stay black against the light.
    lx, lz = 0.229, 0.058

    def lamp(mat, rows):
        s.loft(mat, [(y, lx, lz, r, r) for y, r in rows], "lantern", max(14, n // 2))

    lamp(
        "KeeperBrass",
        [
            (0.581, 0.003),
            (0.582, 0.040),
            (0.588, 0.045),
            (0.600, 0.043),
            (0.609, 0.037),
            (0.619, 0.038),
            (0.625, 0.030),
            (0.631, 0.029),
        ],
    )
    lamp("KeeperBrass", [(0.630, 0.013), (0.638, 0.014), (0.639, 0.009)])
    lamp("KeeperWick", [(0.638, 0.003), (0.644, 0.005), (0.650, 0.004), (0.659, 0.0006)])
    lamp("KeeperGlass", [(0.626, 0.025), (0.635, 0.032), (0.652, 0.035), (0.687, 0.031), (0.707, 0.024)])
    lamp(
        "KeeperBrass",
        [
            (0.705, 0.028),
            (0.710, 0.038),
            (0.716, 0.040),
            (0.720, 0.031),
            (0.732, 0.024),
            (0.739, 0.022),
            (0.741, 0.002),
        ],
    )
    for a in np.linspace(0.25 * pi, 2.25 * pi, 4, endpoint=False):
        s.tube(
            "KeeperBrass",
            [
                (lx + 0.038 * sin(a), 0.60, lz + 0.038 * cos(a)),
                (lx + 0.042 * sin(a), 0.69, lz + 0.042 * cos(a)),
                (lx + 0.036 * sin(a), 0.716, lz + 0.036 * cos(a)),
            ],
            [(0.0025, 0.0025)] * 3,
            "lantern",
        )
    path = [(lx + 0.042 * cos(t), 0.711 + 0.115 * sin(t), lz) for t in np.linspace(0, pi, 24)]
    s.tube("KeeperBrass", path, [(0.0027, 0.0027)] * len(path), "lantern")
