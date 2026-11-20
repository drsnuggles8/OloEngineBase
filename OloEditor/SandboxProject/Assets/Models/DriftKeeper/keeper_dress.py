"""The work clothes, hands and storm lantern. Distances in metres."""

from math import cos, exp, pi, sin

import numpy as np

from keeper_mesh import TEXTURE_REPEAT_METRES

PALETTE = {
    "KeeperSkin": ((0.50, 0.245, 0.163), 0.54, 0),
    "KeeperEar": ((0.36, 0.116, 0.080), 0.62, 0),
    "KeeperLip": ((0.37, 0.134, 0.106), 0.42, 0),
    "KeeperGum": ((0.32, 0.085, 0.065), 0.57, 0),
    "KeeperTongue": ((0.46, 0.145, 0.121), 0.39, 0),
    "KeeperTeeth": ((0.76, 0.65, 0.45), 0.32, 0),
    "KeeperNostril": ((0.045, 0.010, 0.009), 0.83, 0),
    "KeeperNail": ((0.54, 0.29, 0.21), 0.38, 0),
    "KeeperOilskin": ((0.29, 0.153, 0.049), 0.50, 0),
    "KeeperSeam": ((0.155, 0.081, 0.028), 0.55, 0),
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
        if suffix == "L":
            # The bail crosses the palm in the finger-width direction. Each
            # finger hooks under it; the middle pair straddle its pivot.
            knuckle = 0.865
            dip = 0.816 + 0.007 * abs(f - 1.5)
            path = [
                (hx, knuckle, z),
                (0.254, 0.849, z),
                (0.249, dip + 0.006, z + 0.001),
                (0.239, dip, z + 0.002),
                (0.229, dip + 0.001, z + 0.003),
                (0.222, dip + 0.010, z + 0.004),
            ]
        radii = [(0.009, 0.009), (0.009, 0.009), (0.008, 0.008), (0.006, 0.007), (0.002, 0.003)]
        if suffix == "L":
            radii = [
                (0.009, 0.009),
                (0.009, 0.009),
                (0.008, 0.008),
                (0.007, 0.007),
                (0.006, 0.006),
                (0.003, 0.004),
            ]
        s.tube(
            "KeeperSkin",
            path,
            radii,
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


# The coat is cut as front and back panels with two oval scyes (armholes).
# The sleeve cap shares the edge of each hole. A raised arm can therefore
# pull the shoulder up without exposing two intersecting closed tubes.
COAT = np.array(
    [
        # station, half-width, half-depth, front/back balance
        (0.77, 0.210, 0.149, -0.007),
        (0.84, 0.206, 0.149, -0.006),
        (0.94, 0.191, 0.142, -0.004),
        (1.04, 0.173, 0.135, -0.001),
        (1.12, 0.174, 0.132, 0.002),
        (1.20, 0.184, 0.141, 0.003),
        (1.29, 0.196, 0.144, 0.002),
        (1.35, 0.199, 0.135, -0.001),
        (1.39, 0.194, 0.114, -0.004),
        (1.416, 0.178, 0.101, -0.005),
        (1.44, 0.131, 0.087, -0.005),
        (1.461, 0.089, 0.074, -0.002),
        (1.474, 0.067, 0.065, -0.002),
    ]
)
ARMHOLE = 0.73


def between(v, lo, hi):
    return max(0.0, min(1.0, (v - lo) / (hi - lo)))


def bell(x):
    return exp(-x * x)


def roll(distance, width):
    """A raised break with a narrower compressed trough under the ridge."""
    return bell(distance / width) - 0.64 * bell((distance + 1.45 * width) / (0.74 * width))


def front_edge(sign, y):
    # Lower tails swing apart below the bottom fastening. The starboard panel
    # crosses the left at the waist, then both fall open across the chest.
    bottom = (0.19 + 0.045 * sign) * (1 - between(y, 0.77, 1.00)) ** 1.4
    breast = (0.72 + 0.065 * sign) * between(y, 1.135 - 0.016 * sign, 1.455) ** 0.83
    return sign * (-0.10 + bottom + breast) - 0.055


def coat_point(a, y, extra=0):
    w, d, centre = [np.interp(y, COAT[:, 0], COAT[:, k]) for k in (1, 2, 3)]
    # Compressive creases running obliquely from the bent arm and from the
    # pocket mouth, shorter on the buttoned side. The lower coat hangs in
    # heavy, uneven flutes with their apexes at the hip rather than the hem.
    front, side = cos(a), sin(a)
    aa = (a + pi) % (2 * pi) - pi
    folds = 0.0
    for sign in (-1, 1):
        t = aa * sign
        folds += 0.015 * bell((t - 0.78) / 0.60) * roll(y - (1.215 + 0.082 * t - 0.024 * sign), 0.021)
        folds += 0.011 * bell((t - 0.76) / 0.53) * roll(y - (1.155 + 0.047 * t + 0.014 * sign), 0.014)
        folds += 0.010 * bell((t - 0.80) / 0.56) * roll(y - (0.958 + 0.068 * t - 0.013 * sign), 0.016)
        folds += 0.008 * bell((t - 2.27) / 0.66) * roll(y - (1.18 + 0.072 * (t - 2.27)), 0.027)
        for centre_a, amplitude in ((0.47, 0.011), (1.28, 0.015), (2.22, 0.013), (2.77, 0.006)):
            axis = centre_a + (0.98 - y) * (0.9 - 0.35 * sign) + 0.055 * sign
            folds += (1 - between(y, 0.78, 1.16)) * amplitude * roll(t - axis, 0.115 + 0.035 * sign)
    hem_weight = (1 - between(y, 0.77, 0.98)) ** 2
    hem = hem_weight * (0.018 * front - 0.007 * side + 0.007 * sin(3 * a + 0.8) + 0.008 * cos(5 * a - 0.3))
    radius = folds + extra
    return (float((w + radius) * side), y + hem, float(centre + (d + radius) * front))


def hole(sign, q):
    # From the forward point around the sleeve cap. The inner edge passes
    # well below the upper arm pivot; its front sits above its back.
    a = sign * (pi / 2 - ARMHOLE * cos(q))
    y = 1.328 + 0.009 * cos(q) + sign * 0.080 * sin(q)
    return a, y


def arm_weight(p, sign, suffix):
    x, y, _ = p
    pivot = sign * x
    # The cap pivots with the humerus. The neck side of the scye stays with
    # the chest; the low armpit forms a soft gusset between them.
    cap = between(pivot, 0.100, 0.202) * between(y, 1.19, 1.34)
    return {"upperarm_" + suffix: 0.72 * cap, "chest": 1 - 0.72 * cap}


def neck_height(angle):
    return 0.130 + 0.013 * (1 - cos(angle))


def neck_point(angle, y):
    c = cos(angle)
    t = between(y, 1.430, 1.430 + neck_height(angle))
    w = np.interp(
        y, [1.430, 1.460, 1.486, 1.515, 1.549, 1.587], [0.068, 0.063, 0.057, 0.055, 0.061, 0.057]
    )
    depth = np.interp(y, [1.430, 1.468, 1.500, 1.545, 1.587], [0.060, 0.060, 0.055, 0.063, 0.067])
    centre = -0.005 - 0.020 * t
    a = abs((angle + pi) % (2 * pi) - pi)
    scm = 0.0048 * exp(-(((a - (0.36 + 1.02 * t)) / 0.18) ** 2)) * sin(pi * t) ** 0.6
    throat = 0.0038 * exp(-((a / 0.39) ** 2) - ((y - 1.496) / 0.011) ** 2)
    return (sin(angle) * (w + scm), y, centre + c * (depth + scm + throat))


def dress_coat(s, lod, trunk):
    n = (84, 44, 24)[lod]
    sides = (22, 14, 8)[lod]

    def coat_weight(p):
        x, y, _ = p
        if y > 1.20 and abs(x) > 0.100:
            return arm_weight(p, 1 if x > 0 else -1, "L" if x > 0 else "R")
        return trunk(p)

    # The edges of underarm and crown are sampled by the same q grid as the
    # sleeve's top. Adjacent body panels meet at the front/back scye points.
    def overlap(sign, a, y):
        return 0.003 * sign * bell((a - front_edge(sign, y)) / 0.36) * bell((y - 1.025) / 0.13)

    def segment(a_at, mid_at, cols, front=0):
        for upper, rows in [(False, (46, 27, 16)[lod]), (True, (18, 11, 6)[lod])]:

            def sample(u, v, upper=upper):
                u /= 2 * pi
                mid = mid_at(u, upper)
                y = mid + (1.474 - mid) * v if upper else 0.77 + (mid - 0.77) * v
                a = a_at(u, y)
                return coat_point(a, y, overlap(front, a, y) if front else 0)

            s.grid("KeeperOilskin", cols, rows, sample, coat_weight)

    def side_panel(sign):
        a_near = sign * (pi / 2 - ARMHOLE)

        def a_front(u, y):
            t = u if sign > 0 else 1 - u
            return (1 - t) * front_edge(sign, y) + t * a_near

        segment(a_front, lambda u, upper: 1.337, n // 6, sign)

        def a_hole(u, y):
            q = pi * (u if sign > 0 else 1 - u)
            return hole(sign, q)[0]

        def mid_hole(u, upper):
            q = pi * (u if sign > 0 else 1 - u)
            return hole(sign, sign * q if upper else -sign * q)[1]

        segment(a_hole, mid_hole, sides)

    for sign in (1, -1):
        side_panel(sign)
    back_start = pi / 2 + ARMHOLE
    segment(lambda u, y: back_start + u * (2 * pi - 2 * back_start), lambda u, upper: 1.319, n // 3)

    # The fold exposes the darker facing between the roll and its free edge.
    # This quadrant is a separate piece of cloth following the opening;
    # a shallow return closes its thickness at the outer edge.
    def front_details(sign):
        start = 1.122 + 0.012 * sign

        def lapel(a, v):
            u = a / (2 * pi) if sign > 0 else 1 - a / (2 * pi)
            y = start + (1.466 - start) * v
            breadth = np.interp(v, [0, 0.12, 0.65, 0.77, 0.87, 1], [0.008, 0.12, 0.40, 0.45, 0.22, 0.10])
            ang = front_edge(sign, y) + sign * float(breadth) * u
            relief = 0.003 + 0.006 * bell((u - 0.18) / 0.20) + 0.007 * u * u
            return coat_point(ang, y, overlap(sign, ang, y) + relief * min(1, 0.08 + 5 * v))

        s.grid("KeeperOilskin", max(6, n // 8), 22, lapel, coat_weight)

        def placket(a, v):
            y = 0.77 + (start - 0.77) * v
            u = a / (2 * pi) if sign > 0 else 1 - a / (2 * pi)
            ang = front_edge(sign, y) + sign * u * 0.083
            return coat_point(ang, y, overlap(sign, ang, y) + 0.002 + 0.002 * sin(pi * u))

        s.grid("KeeperOilskin", 4, 20, placket, trunk)

        # Turn the front edge back to the shell. The hem exposes the shadow
        # of this doubled facing on the open tails.
        def return_edge(a, t):
            y = 0.77 + (1.466 - 0.77) * a / (2 * pi)
            angle = front_edge(sign, y)
            delta = 0.004 * sign * t
            return coat_point(angle + delta, y, overlap(sign, angle, y) + 0.003 - 0.006 * t)

        s.grid("KeeperSeam", max(20, n // 2), 2, return_edge, coat_weight, invert=sign < 0)
        # Side-entry slash pocket. Both lips follow the same nonlinear coat
        # surface, with a recessed dark mouth visible under the upper welt.
        for mat, v0, v1, height in [
            ("KeeperSeam", -0.013, 0.006, 0.0045),
            ("KeeperOilskin", -0.014, -0.002, 0.0085),
        ]:

            def pocket(a, v, v0=v0, v1=v1, height=height):
                u = a / (2 * pi) if sign > 0 else 1 - a / (2 * pi)
                ang = sign * (0.42 + 0.56 * u)
                taper = min(1, 0.42 + 6 * min(u, 1 - u))
                y = 0.956 + 0.081 * u + 0.009 * sign + taper * (v0 + (v1 - v0) * v)
                lift = (0.30 + 0.70 * sin(pi * u) ** 0.4) * (0.55 + 0.45 * sin(0.82 * pi * v))
                return coat_point(ang, y, height * lift + 0.002)

            s.grid(mat, 16, 4, pocket, trunk)

    for sign in (-1, 1):
        front_details(sign)

    # Folded collar. Back seam stands clear of the nape. Each collar point
    # extends onto the rolled lapel and the front opening stays clear.
    def collar(a, v):
        gap = 0.51 + 0.14 * v
        ang = gap + (2 * pi - 2 * gap) * a / (2 * pi)
        tip = bell((min(ang, 2 * pi - ang) - 0.84) / 0.27)
        low = 1.432 - 0.046 * tip + 0.009 * cos(ang)
        y = low + (1.493 - low) * v
        cw = np.interp(v, [0, 0.18, 0.64, 1], [0.126, 0.125, 0.091, 0.073])
        cd = np.interp(v, [0, 0.18, 0.64, 1], [0.108, 0.108, 0.087, 0.073])
        return (cw * sin(ang), y - 0.004 * v * sin(ang), -0.002 + cd * cos(ang) + 0.015 * tip * (1 - v))

    s.grid("KeeperOilskin", n, 8, collar, "chest")

    # A rear storm yoke with a doubled, curved lower edge carries water
    # outwards towards the raglan seam.
    def yoke(a, v):
        ang = 1.81 + (2 * pi - 3.62) * a / (2 * pi)
        low = 1.302 + 0.063 * (1 + cos(ang))
        y = low + (1.441 - low) * v
        lift = 0.0007 + 0.004 * (1 - v) ** 2
        return coat_point(ang, y, lift)

    s.grid("KeeperOilskin", n // 2, 10, yoke, coat_weight)

    def yoke_fold(a, v):
        x, y, z = yoke(a, 0)
        return (x * (1 - 0.007 * v), y - 0.003 * v, z * (1 - 0.007 * v))

    s.grid("KeeperSeam", n // 2, 1, yoke_fold, coat_weight, invert=True)
    for y in (0.865, 0.965, 1.065, 1.153):
        angle = front_edge(1, y) + 0.105
        x, by, z = coat_point(angle, y, overlap(1, angle, y) + 0.005)
        # Buttonhole binding breaks the straight line of the outer fly.
        s.oval("KeeperSeam", (x - 0.003, by, z), (0.011, 0.0022, 0.0020), trunk, 14, 6)
        s.oval("KeeperBrass", (x, by, z + 0.0028), (0.007, 0.007, 0.0032), trunk, 14, 8)

    for sign, suffix in [(1, "L"), (-1, "R")]:
        dress_sleeve(s, sign, suffix, sides)
    # Bulk of a worsted pullover visible where the unbuttoned oilskin parts.
    knit = [
        (0.788, 0.145, 0.106, 0),
        (0.814, 0.153, 0.112, 0.001),
        (0.98, 0.154, 0.115, 0.002),
        (1.13, 0.151, 0.116, 0),
        (1.25, 0.172, 0.128, 0.002),
        (1.34, 0.176, 0.129, 0),
        (1.398, 0.162, 0.110, -0.003),
        (1.448, 0.101, 0.079, -0.003),
    ]

    def knit_weights(point):
        if point[1] > 1.445:
            return mix_weights(point[1], 1.445, 1.474, "chest", "neck")
        return trunk(point)

    def knit_flute(a, y):
        return 0.0014 * cos(44 * a) + 0.003 * bell((y - 1.25) / 0.18) * cos(6 * a + 8 * y)

    s.loft(
        "KeeperGansey",
        [(y, 0, z, w, d) for y, w, d, z in knit],
        knit_weights,
        n,
        knit_flute,
    )

    # Fit the ribbed neck to the actual nape. The inner lip follows the skin
    # contour while the raised outer edge leaves room for the thickness of wool.
    def neckband(a, t):
        rim_y = 1.491 + 0.005 * (1 - cos(a)) / 2
        stations = [
            (1.448, 0.010),
            (1.463, 0.008),
            (rim_y - 0.012, 0.0055),
            (rim_y - 0.002, 0.0055),
            (rim_y, 0.004),
            (rim_y - 0.001, 0.001),
        ]
        row = round(t * (len(stations) - 1))
        y, ease = stations[row]
        if row == 0:
            d = knit_flute(a, y)
            return ((0.101 + d) * sin(a), y, -0.003 + (0.079 + d) * cos(a))
        x, y, z = neck_point(a, y)
        rib = 0.00055 * cos(44 * a) * sin(pi * t)
        return (x + (ease + rib) * sin(a), y, z + (ease + rib) * cos(a))

    s.grid("KeeperGansey", n, 5, neckband, knit_weights)


def dress_sleeve(s, sign, suffix, half):
    # Ring the wrist and elbow, then turn the sleeve from a horizontal
    # section into the sloped scye. The shoulder surface is one continuous
    # shell even when the wave draws the arm above horizontal.
    sections = np.array(
        [
            # centre x, height, centre z, transverse and depth radii
            (0.251, 0.944, 0.053, 0.038, 0.039),
            (0.252, 0.955, 0.053, 0.043, 0.045),
            (0.253, 0.991, 0.046, 0.047, 0.049),
            (0.249, 1.060, 0.035, 0.051, 0.054),
            (0.244, 1.142, 0.019, 0.056, 0.057),
            (0.240, 1.181, 0.013, 0.055, 0.061),
            (0.232, 1.236, 0.007, 0.062, 0.066),
            (0.224, 1.282, 0.002, 0.067, 0.073),
            (0.215, 1.322, 0, 0.068, 0.076),
            (0.208, 1.349, -0.003, 0.064, 0.072),
        ]
    )

    def sleeve_weight(p):
        x, y, _ = p
        # Carry the forearm around the elbow gradually. At the scye the body
        # and sleeve query the same function, including in the wave pose.
        station = y + 0.50 * (0.215 - sign * x)
        upper = between(station, 1.10, 1.19)
        if y < 1.235:
            return {"upperarm_" + suffix: upper, "forearm_" + suffix: 1 - upper}
        blend = between(0.239 - sign * x, 0.003, 0.068) * between(y, 1.23, 1.35)
        w = arm_weight(p, sign, suffix)["chest"] * blend
        return {"upperarm_" + suffix: 1 - w, "chest": w}

    def shaft(q, y):
        x, z, rx, rz = [np.interp(y, sections[:, 1], sections[:, i]) for i in (0, 2, 3, 4)]
        # Elbow load folds fan out from the inside seam. Their opposing
        # crest/trough and taper keep the extended side relatively taut.
        inside = 0.5 + 0.5 * cos(q + 0.38 * sign)
        d = inside * (
            0.009 * roll(y - 1.119 - 0.030 * sin(q), 0.012) + 0.010 * roll(y - 1.164 + 0.032 * sin(q), 0.016)
        )
        d += 0.008 * bell((y - 0.988) / 0.044) * cos(3 * q + sign * 9 * y) + 0.003 * cos(4 * q + 8 * y)
        d += 0.004 * roll(y - 1.236 - 0.033 * sin(q + 0.6), 0.015)
        return np.array((sign * x + (rx + d) * sin(q), y, z + (rz + d) * cos(q)))

    cols = 2 * half
    rows = max(17, round(37 * s.detail))
    lower_sleeve = s.grid("KeeperOilskin", cols, rows, lambda q, t: shaft(q, 0.944 + 0.248 * t), sleeve_weight)
    elbow_ring = lower_sleeve[-(cols + 1) :]

    # On a dress pattern the armpit side of a sleeve cap is much shorter
    # than its outer crown. A horizontal final ring forces the inner sleeve
    # back down into the body, which collapses when the arm lifts. Bend each
    # cloth meridian upwards from the elbow to its own point on the scye.
    depth = 16 if half > 10 else 8
    for j in range(depth + 1):
        t = j / depth
        for i in range(cols + 1):
            q = 2 * pi * i / cols
            outside = 0.5 + 0.5 * sign * sin(q)
            a, y = hole(sign, q)
            p0 = shaft(q, 1.192)
            p3 = np.array(coat_point(a, y))
            rise = y - p0[1]
            p1 = p0 + np.array((-sign * 0.009, 0.43 * rise, -0.003))
            p2 = p3 + np.array((sign * (0.018 + 0.047 * outside), -0.27 * rise, 0.008 * cos(q)))
            point = (1 - t) ** 3 * p0 + 3 * t * (1 - t) ** 2 * p1 + 3 * t * t * (1 - t) * p2 + t**3 * p3
            # Diagonal drag lines form where the sleeve catches against the
            # bicep. Their amplitudes vanish at both sewn boundaries.
            fan = sin(pi * t) ** 1.5 * (
                0.006 * roll(t - 0.45 - 0.19 * sin(q - 0.4 * sign), 0.115)
                + 0.004 * roll(t - 0.68 + 0.11 * sin(q + 0.7), 0.095)
            )
            point += (fan * sin(q), 0, fan * cos(q))
            # Share the shaft's chart and influence at the sewn elbow. Both
            # copies of each vertex must meet after settling and animating.
            elbow = elbow_ring[i]
            blend = t * t * (3 - 2 * t)
            forearm = (1 - blend) * s.bones[elbow]["forearm_" + suffix]
            chest = blend * arm_weight(p3, sign, suffix)["chest"]
            weights = {"forearm_" + suffix: forearm, "chest": chest, "upperarm_" + suffix: 1 - chest - forearm}
            u, v = s.uvs[elbow]
            if j:
                previous = len(s.points) - (cols + 1)
                length = np.linalg.norm(point - s.points[previous])
                v = s.uvs[previous][1] - length / TEXTURE_REPEAT_METRES["KeeperOilskin"]
            idx = s.vertex(point, (u, v), weights)
            if i and j:
                s.quad("KeeperOilskin", idx - cols - 2, idx - cols - 1, idx, idx - 1)

    # Returned oilcloth cuff and ribbed gansey wrist leave a narrow band of
    # knit between sleeve and hand. The cuff opening has a visible inner rim.
    def cuff(a, t):
        rows = [
            (0.937, 0.034, 0.035),
            (0.938, 0.043, 0.046),
            (0.943, 0.046, 0.048),
            (0.958, 0.045, 0.048),
            (0.962, 0.043, 0.046),
        ]
        y, w, d = rows[round(t * 4)]
        return (sign * 0.251 + w * sin(a), y + 0.003 * cos(a - 0.5), 0.053 + d * cos(a))

    s.grid("KeeperOilskin", cols, 4, cuff, "forearm_" + suffix)

    # Short oilcloth strap keeps water out of the hanging sleeve. Its end
    # crosses the inner seam and fastens on the outer wrist.
    def strap(a, t):
        phi = sign * (0.24 + 1.78 * (a / (2 * pi) if sign > 0 else 1 - a / (2 * pi)))
        y = 0.963 + 0.019 * t + 0.005 * cos(phi - 0.6 * sign)
        x, _, z = shaft(phi, y)
        return x + 0.004 * sin(phi), y, z + 0.004 * cos(phi)

    s.grid("KeeperOilskin", max(8, half), 3, strap, "forearm_" + suffix)
    x, y, z = strap((1.75 if sign > 0 else 0.25) * pi, 0.5)
    s.oval("KeeperBrass", (x + sign * 0.002, y, z), (0.0025, 0.004, 0.004), "forearm_" + suffix, 10, 6)
    s.loft(
        "KeeperGansey",
        [
            (y, sign * 0.251, 0.053, w, d)
            for y, w, d in [
                (0.928, 0.023, 0.032),
                (0.930, 0.029, 0.035),
                (0.945, 0.029, 0.036),
                (0.949, 0.028, 0.034),
            ]
        ],
        "forearm_" + suffix,
        cols,
        lambda a, y: 0.0013 * cos(23 * a),
    )


def deck_cap(s, n):
    # The firm sweatband holds an eight-piece cloth crown. Its forward panel
    # stands up behind the device; the cloth farther back sags towards one ear.
    # Varying the pattern circumference by panel leaves shallow sewn darts.
    def crown(a, t):
        levels = [
            (1.690, 0.078, 0.102, -0.007),
            (1.703, 0.084, 0.109, -0.008),
            (1.715, 0.096, 0.120, -0.013),
            (1.726, 0.100, 0.128, -0.014),
            (1.737, 0.100, 0.127, -0.012),
            (1.747, 0.096, 0.119, -0.010),
            (1.755, 0.086, 0.108, -0.009),
            (1.762, 0.067, 0.085, -0.007),
            (1.765, 0.041, 0.056, -0.004),
            (1.766, 0.0001, 0.0001, -0.002),
        ]
        y, w, d, z = levels[round(t * (len(levels) - 1))]
        fold = sin(pi * t) ** 1.2
        dart = 0.0035 * fold * bell(sin(4 * a + 0.2) / 0.34)
        drop = fold * (
            0.012 * sin(a + 0.5) + 0.007 * cos(a - 2.7) + 0.008 * bell((sin(a - 0.15) + 0.70) / 0.25)
        )
        roll = 0.005 * fold * sin(3 * a + 0.5)
        return (
            (w - dart + roll) * sin(a) - 0.009 * t,
            y - drop - 0.006 * t + 0.014 * fold * cos(a),
            z + (d - dart + roll) * cos(a),
        )

    s.grid("KeeperCap", n, 9, crown, "head")

    def band(a, t):
        y, w, d = [
            (1.666, 0.079, 0.103),
            (1.666, 0.085, 0.110),
            (1.669, 0.0855, 0.1105),
            (1.683, 0.084, 0.108),
            (1.696, 0.085, 0.107),
            (1.700, 0.085, 0.108),
        ][round(5 * t)]
        return (w * sin(a), y + 0.004 * cos(a) - 0.003 * sin(a), -0.008 + d * cos(a))

    s.grid("KeeperCap", n, 5, band, "head")

    # Glazed leather bill, dished across its width with a cut, dark edge.
    def brim(a, t):
        phi = (a / (2 * pi) - 0.5) * 2.86
        front = max(0, cos(phi / 1.43 * pi / 2))
        return (
            0.083 * sin(phi) * (1 + 0.14 * t),
            1.684 - 0.013 * t * front - 0.008 * sin(phi) ** 2 - 0.002 * sin(phi),
            -0.007 + 0.106 * cos(phi) + 0.043 * t * front,
        )

    s.grid("KeeperLeather", n, 6, brim, "head", invert=True)

    def underside(a, t):
        x, y, z = brim(a, t)
        return (x, y - 0.0025, z)

    s.grid("KeeperLeather", n, 6, underside, "head")

    def rim(a, t):
        x, y, z = brim(a, 1)
        return (x, y - 0.0025 * t, z)

    s.grid("KeeperLeather", n, 1, rim, "head", invert=True)

    # Short braided storm cord attached at each temple. It rests against
    # the band, above the peak; the brass anchor stands on a dark wool patch.
    for strand in (-1, 1):
        cord = []
        for a in np.linspace(-1.46, 1.46, 62):
            twist = strand * 0.00085
            cord.append(
                (
                    0.086 * sin(a),
                    1.696 - 0.008 * cos(a) ** 2 - 0.003 * sin(a) + twist * sin(43 * a),
                    -0.006 + 0.109 * cos(a) + twist * cos(43 * a),
                )
            )
        s.tube("KeeperSeam", cord, [(0.00125, 0.00125)] * len(cord), "head", 6)
    for sign in (-1, 1):
        s.oval("KeeperBrass", (sign * 0.086, 1.696 - sign * 0.003, 0.006), (0.0028, 0.0041, 0.0041), "head")
    s.oval("KeeperCap", (-0.001, 1.716, 0.112), (0.014, 0.014, 0.003), "head", 18, 12)
    for path in [
        [(-0.001, 1.727, 0.115), (-0.001, 1.711, 0.117)],
        [(-0.006, 1.721, 0.116), (0.004, 1.721, 0.116)],
        [
            (-0.010, 1.716, 0.116),
            (-0.008, 1.712, 0.117),
            (-0.001, 1.709, 0.118),
            (0.006, 1.713, 0.117),
            (0.008, 1.717, 0.116),
        ],
    ]:
        s.tube("KeeperBrass", path, [(0.0012, 0.0012)] * len(path), "head", 8)


def sea_boot(s, sign, suffix, n):
    x = 0.111 * sign
    boot = "boot_" + suffix

    def weight(p):
        return mix_weights(p[1], 0.098, 0.170, boot, "shin_" + suffix)

    # The welt follows a foot last: broader across the ball than the heel,
    # with toe spring and a shorter, narrow shank over the separate heel.
    def sole(a, t):
        y, w, d = [
            (0.006, 0.054, 0.131),
            (0.007, 0.060, 0.136),
            (0.011, 0.064, 0.139),
            (0.022, 0.064, 0.139),
            (0.027, 0.061, 0.135),
        ][round(4 * t)]
        f = cos(a)
        ball = 0.86 + 0.22 * f
        lift = 0.012 * bell((f + 0.35) / 0.33) * (1 - t) + 0.009 * max(0, f) ** 4
        return (x + w * ball * sin(a) + sign * 0.008 * f * f, y + lift, 0.038 + d * f)

    s.grid("KeeperSole", n, 4, sole, boot)
    heel = [
        (0.001, x, -0.042, 0.042, 0.046),
        (0.005, x, -0.042, 0.047, 0.049),
        (0.021, x, -0.039, 0.050, 0.050),
    ]
    s.loft("KeeperSole", heel, boot, n)
    stations = np.array(
        [
            (0.023, 0.061, 0.135, 0.039),
            (0.034, 0.063, 0.132, 0.040),
            (0.049, 0.062, 0.119, 0.043),
            (0.069, 0.055, 0.100, 0.039),
            (0.098, 0.047, 0.075, 0.016),
            (0.123, 0.046, 0.064, -0.003),
            (0.180, 0.047, 0.058, -0.007),
            (0.231, 0.052, 0.061, -0.006),
            (0.318, 0.062, 0.071, -0.008),
            (0.369, 0.071, 0.078, -0.009),
            (0.426, 0.080, 0.082, -0.010),
            (0.447, 0.080, 0.083, -0.009),
        ]
    )

    def shaft(a, t):
        y = 0.023 + 0.424 * t
        w, d, z = [np.interp(y, stations[:, 0], stations[:, k]) for k in (1, 2, 3)]
        f, side = cos(a), sin(a)
        wr = (
            0.007 * roll(y - (0.142 + 0.013 * side - 0.015 * f), 0.012)
            + 0.005 * roll(y - (0.192 - 0.014 * side), 0.014)
        ) * (0.6 + 0.4 * f)
        wr += 0.004 * (1 - f * f) * roll(y - 0.332 + 0.031 * side, 0.019)
        wr += 0.0035 * between(y, 0.19, 0.37) * cos(5 * a + 13 * y + sign)
        toe = 1 - between(y, 0.026, 0.125)
        return (
            x + (w + wr) * (1 + toe * (-0.14 + 0.22 * f)) * side + sign * 0.008 * toe * f * f,
            y + 0.009 * toe * max(0, f) ** 4,
            z + (d + wr) * f,
        )

    s.grid("KeeperLeather", n, max(18, round(40 * s.detail)), shaft, weight)

    # The turn-down is an open, doubled shaft. Its free edge tilts and
    # droops at the boot's outer calf instead of cutting a horizontal ring.
    def cuff(a, t):
        y, w, d = [
            (0.406, 0.078, 0.082),
            (0.408, 0.084, 0.088),
            (0.419, 0.085, 0.089),
            (0.443, 0.083, 0.087),
            (0.450, 0.082, 0.085),
            (0.454, 0.079, 0.083),
            (0.447, 0.072, 0.076),
        ][round(6 * t)]
        drop = 0.010 * cos(a + 0.9 * sign) + 0.005 * sin(3 * a)
        flute = 0.0025 * cos(5 * a + sign)
        return (x + (w + flute) * sin(a), y + drop, -0.009 + (d + flute) * cos(a))

    s.grid("KeeperLeather", n, 6, cuff, "shin_" + suffix)


def body(s, lod, anatomy=False):
    n = (80, 36, 18)[lod]

    def trunk(p):
        if p[1] < 1.16:
            return mix_weights(p[1], 0.96, 1.16, "pelvis", "spine")
        return mix_weights(p[1], 1.16, 1.34, "spine", "chest")

    if anatomy:
        from keeper_anatomy import underlying_body

        underlying_body(s, trunk, mix_weights)
        for sign, suffix in ((1, "L"), (-1, "R")):
            add_hand(s, sign, suffix)
        return

    s.grid("KeeperSkin", n, (32, 18, 10)[lod], lambda a, t: neck_point(a, 1.430 + neck_height(a) * t), "neck")

    dress_coat(s, lod, trunk)
    for sign, suffix in ((1, "L"), (-1, "R")):

        def part(name, suffix=suffix):
            return name + "_" + suffix

        x = sign * 0.099

        def leg(p):
            return mix_weights(p[1], 0.42, 0.54, part("shin"), part("thigh"))

        trouser = [
            (0.30, 0.044, 0.048, 0.002),
            (0.36, 0.052, 0.052, 0),
            (0.38, 0.055, 0.056, 0.002),
            (0.40, 0.060, 0.058, -0.003),
            (0.422, 0.062, 0.062, 0),
            (0.452, 0.066, 0.069, 0.002),
            (0.478, 0.079, 0.082, 0.006),
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
        sea_boot(s, sign, suffix, n // 2)
        add_hand(s, sign, suffix)
    deck_cap(s, n)
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
    # A wire stirrup with a nearly flat grip. The middle of the grip is the
    # lantern joint, so it stays in the hand when the hanging weight swings.
    path = [
        (lx + 0.008 * cos(t), 0.711 + 0.115 * (1 - abs(cos(t)) ** 3) ** 0.5, lz + 0.041 * cos(t))
        for t in np.linspace(0, pi, 33)
    ]
    s.tube("KeeperBrass", path, [(0.0027, 0.0027)] * len(path), "lantern")
