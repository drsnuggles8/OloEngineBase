"""The six Drift islands, as landforms.

Each recipe returns metres above the sea on the tile's own grid (x east, z north, origin at the
tile's corner, which is how `Drift.olo` places a terrain). A recipe draws the island's CHARACTER:
where the spine runs, where the cliffs stand, where the sand lies. Erosion, the shore profile and
the materials are then derived from it by the shared pipeline in `pipeline.py`.

The numbers that tie a recipe to the scene are the `SITES`: the landmarks were surveyed against
the old terrain with Jolt rays, and each site restates that survey as a target the new ground
must meet. `survey.py` re-measures them from the generated height field.

References, one per island: Hebridean gneiss skerries (Ridgeback, Sisters), the basalt stacks of
St Kilda and the Faroes (Stacks), bedded sandstone tables as on the Algarve coast (Mesa), the
dune and machair islands of the Outer Hebrides (Dunes), a Pacific ring atoll (Atoll).
"""
import numpy as np

from . import fields as f
from . import shaping as s

# name -> (tile size m, height scale m, seed, world corner x, world corner z)
TILES = {
    "Ridgeback": (420.0, 112.0, 879, -210.0, 70.0),
    "Sisters": (260.0, 80.0, 3313, -290.0, 530.0),
    "Stacks": (300.0, 128.0, 5501, -480.0, -670.0),
    "Mesa": (360.0, 72.0, 2207, 320.0, -140.0),
    "Dunes": (380.0, 56.0, 1301, -730.0, -130.0),
    "Atoll": (400.0, 32.0, 4409, 130.0, -680.0),
}


def _sea(off, scale=18.0):
    return s.submarine(-s.BASE, off, scale)


def _blob(x, z, cx, cz, rx, rz, rot_deg, seed, warp_len, warp_amt):
    """Normalised distance u from an ellipse centre (1 on its rim), coast-warped; and metres per u."""
    xw, zw = f.warp(x, z, seed, warp_len, warp_amt)
    xw, zw = f.warp(xw, zw, seed + 13, warp_len * 0.31, warp_amt * 0.3)
    c, sn = np.cos(np.radians(rot_deg)), np.sin(np.radians(rot_deg))
    lx = (xw - cx) * c + (zw - cz) * sn
    lz = -(xw - cx) * sn + (zw - cz) * c
    u = np.hypot(lx / rx, lz / rz)
    return u, 0.5 * (rx + rz), np.arctan2(lz, lx)


# ── Ridgeback ───────────────────────────────────────────────────────────────────────────────
def ridgeback(x, z, seed):
    """A whaleback of gneiss: a long spine, sheer on the weather (north) side, grass falling
    south to a rock shore, a shingle cove, and the timber landing in the lee of the west point."""
    xw, zw = f.warp(x, z, seed, 120.0, 13.0)
    xw, zw = f.warp(xw, zw, seed + 5, 38.0, 4.0)
    # (x, z, crest height, half-width south, half-width north)
    spine = [
        (52.0, 136.0, 5.0, 40.0, 28.0),
        (104.0, 164.0, 30.0, 84.0, 48.0),
        (165.0, 200.0, 57.0, 112.0, 54.0),
        (222.0, 224.0, 71.0, 124.0, 54.0),
        (270.0, 236.0, 58.0, 116.0, 50.0),
        (318.0, 240.0, 84.0, 112.0, 48.0),
        (358.0, 230.0, 42.0, 66.0, 40.0),
        (384.0, 214.0, 6.0, 34.0, 26.0),
    ]
    dist, side, crest, w_south, w_north = f.polyline_field(xw, zw, spine)
    north = side > 0.0
    # One spur field shapes BOTH the slope and the coast: a spur stands higher and runs further
    # out to sea as a headland, the gully beside it ends in a cove.
    spur = f.fbm(xw, zw, seed + 21, 92.0, 3)
    geo = f.ridged(xw, zw, seed + 22, 48.0, 3) - 0.5  # buttresses and clefts on the weather face
    w = np.where(north, w_north * (1.0 + 0.32 * geo), w_south * (1.0 + 0.36 * spur))
    u = dist / w
    # South: concave grass slope, steep under the crest and easing to the shore, with a bench
    # two thirds of the way down where the slope pauses.
    bench = 0.10 * np.exp(-((u - 0.62) / 0.13) ** 2) * (0.5 + 0.5 * f.fbm(xw, zw, seed + 23, 140.0, 2))
    south_h = crest * (np.clip(1.0 - u, 0.0, 1.0) ** 1.7 + bench * np.clip(1.0 - u, 0.0, 1.0) ** 0.3)
    # North: the top holds, then the face drops.
    north_h = crest * (1.0 - 0.22 * np.clip(u, 0.0, 1.0) ** 2) * (1.0 - f.smoothstep(0.50, 1.0, u) ** 0.8)
    land = np.where(north, north_h, south_h)
    land = land * (1.0 + 0.26 * spur * (~north))
    # Crags: knuckles of rock standing out of the steeper ground.
    crag = np.clip(f.ridged(x, z, seed + 24, 26.0, 4) - 0.55, 0.0, 1.0)
    land += 7.0 * crag * f.smoothstep(6.0, 22.0, land) * (1.0 - f.smoothstep(0.75, 1.0, u))
    land += 1.8 * f.fbm(x, z, seed + 31, 19.0, 4) * f.smoothstep(2.0, 14.0, land)
    off = np.maximum(u - 1.0, 0.0) * w
    h = np.where(u < 1.0, land, _sea(off, np.where(north, 11.0, 20.0)))
    # Skerries off the west point.
    for cx, cz, top, r in ((70.0, 112.0, 3.2, 9.0), (58.0, 98.0, 1.6, 6.5), (86.0, 102.0, 2.2, 5.0)):
        h = np.maximum(h, top * f.bump(x, z, cx, cz, r, 2.4) + s.BASE * (1 - f.bump(x, z, cx, cz, r * 3.0, 1.2)))
    hard = np.clip(np.where(north, 0.9, 0.25) + 1.2 * crag, 0.0, 0.95)
    rock_shore = np.ones_like(h)
    # The shingle cove east of the landing, and a second pocket under the saddle.
    beach = np.maximum(f.bump(x, z, 172.0, 104.0, 46.0, 3.0), f.bump(x, z, 300.0, 128.0, 30.0, 3.0))
    return {"h": h, "hard": hard, "beach": beach, "rock_shore": rock_shore * (1 - beach),
            "rock_kind": np.zeros_like(h), "wind": (1.0, 0.15)}


def ridgeback_sites(h, x, z):
    """The timber landing and hand crane (Assets/Models/DriftHarbourCrane/README.md).

    Scene roots at world (-106, 1.48, 158.5), i.e. tile-local (104, 88.5); the deck spans local
    x 99.6..108.4, z 86.4..91.6 and its shoreward (north) edge meets ground at 1.31-1.49 m.
    Surveyed bed: -3.21 m under the hook (local z 83.64), -3.52 m on the alongside track.
    """
    lx = 104.0
    # The surveyed profile, as a function of z: the hard standing meets the deck's shoreward
    # edge (z = 91.6) at deck height and climbs inland; under the deck a rock face falls to the
    # berth's bed, -3.21 m under the hook; seaward of the hook the bed deepens at 0.33 m per
    # metre, which puts the alongside track (z = 82.7) at -3.52 m.
    apron = 1.42 + 0.085 * np.maximum(z - 91.6, 0.0) + 0.012 * np.abs(x - lx)
    face = f.lerp(-3.21, 1.42, np.clip((z - 83.64) / (91.6 - 83.64), 0.0, 1.0))
    bed = -3.21 - 0.33 * np.maximum(83.64 - z, 0.0)
    profile = np.where(z >= 91.6, apron, np.where(z >= 83.64, face, bed))
    # Inland the standing only ever RAISES ground, and is carried until it meets the island's
    # own slope, wherever the warped coast put that.
    w = s.box_weight(x, z, lx - 10.0, lx + 10.0, 91.6, 150.0, 9.0)
    h = f.lerp(h, np.maximum(h, profile), w)
    # The alongside track and the approach from the west (boat draft 0.65 m). Before the berth,
    # which then has the last word under the deck: a channel cut afterwards took the rock face
    # down with it.
    h = s.channel(h, x, z, 30.0, 76.0, 150.0, 82.5, 7.0, 3.5, 8.0)
    # The berth is SET, not only cut: the natural bed here is 12-17 m down, and the survey the
    # landing's piles and the hook were built against is a ledge 3 m under the surface.
    w = s.box_weight(x, z, lx - 12.0, lx + 12.0, 74.0, 91.6, 7.0)
    h = f.lerp(h, profile, w)
    return h


# ── Sisters ─────────────────────────────────────────────────────────────────────────────────
def sisters(x, z, seed):
    """Two steep islets of the same rock, joined by a shingle bar that the swell washes over."""
    islets = ((96.0, 154.0, 56.0, 44.0, 28.0, 45.0, seed), (180.0, 102.0, 38.0, 31.0, -20.0, 30.0, seed + 40))
    h = np.full_like(x, s.BASE)
    for cx, cz, rx, rz, rot, peak, sd in islets:
        u, r, theta = _blob(x, z, cx, cz, rx, rz, rot, sd, 60.0, 7.0)
        crag = f.ridged(x, z, sd + 3, 46.0, 4) - 0.4
        # Tilted blocks: the swell has cut a wall on the weather (west) side and left the bedding
        # plane as a slope in the lee, so the summit stands near the western edge, not in the
        # middle. `weather` is 1 facing the swell and 0 in the lee.
        weather = f.smoothstep(-0.45, 0.55, np.cos(theta + np.radians(rot) - np.radians(195.0)))
        uc = np.clip(u, 0.0, 1.0)
        wall = 1.0 - uc ** 7.0                     # holds its height almost to the rim, then drops
        slope = (1.0 - uc) ** 0.85 * (1.0 + 0.5 * uc)  # falls from the summit all the way down
        land = peak * f.lerp(slope, wall, weather) * (1.0 + 0.30 * crag * f.smoothstep(0.05, 0.4, 1.0 - uc))
        # Ledges: the rock breaks along its bedding, in steps a few metres high.
        step = 4.5 + 1.5 * f.fbm(x, z, sd + 11, 50.0, 2)
        stepped = (np.floor(land / step) + f.smoothstep(0.55, 0.95, land / step - np.floor(land / step))) * step
        land = f.lerp(land, stepped, 0.55 * weather + 0.2)
        land += 1.5 * f.fbm(x, z, sd + 9, 14.0, 4) * f.smoothstep(1.0, 8.0, land)
        h = np.maximum(h, np.where(u < 1.0, land, _sea((u - 1.0) * r, 12.0)))
    # The bar: a low shingle ridge between them, bowed toward the swell.
    bar = [(128.0, 132.0, 1.0), (140.0, 119.0, 0.9), (154.0, 111.0, 1.0)]
    dist, _, top = f.polyline_field(x, z, bar)
    ridge = np.where(dist < 9.0, top * (1.0 - (dist / 9.0) ** 2), _sea(dist - 9.0, 9.0) * 0.5)
    h = np.maximum(h, ridge)
    beach = (dist < 26.0).astype(np.float64) * (1.0 - f.smoothstep(14.0, 26.0, dist))
    return {"h": h, "hard": np.full_like(h, 0.85), "beach": beach, "rock_shore": 1.0 - beach,
            "rock_kind": np.zeros_like(h), "wind": (0.9, 0.4)}


def _faceted(lx, lz, sides, seed, irregular=0.2):
    """Distance in units of an irregular convex polygon's inradius: 1 on its faces. The faces are
    the joint planes a stack broke along, so no two are the same length or quite evenly turned."""
    rng = np.random.default_rng(seed)
    angles = (np.arange(sides) + rng.uniform(-0.28, 0.28, sides)) * (2.0 * np.pi / sides)
    reach = 1.0 + rng.uniform(-irregular, irregular, sides)
    d = np.full_like(lx, -1.0e9)
    for a, r in zip(angles, reach):
        d = np.maximum(d, (lx * np.cos(a) + lz * np.sin(a)) / r)
    return np.maximum(d, 0.0)


# ── Stacks ──────────────────────────────────────────────────────────────────────────────────
# (centre x, centre z, radius, top height). The first is the stack Stacks Light stands on.
STACKS = (
    (61.0, 205.0, 12.5, 28.7),
    (104.0, 92.0, 7.5, 47.0),
    (243.0, 92.0, 9.5, 33.0),
    (38.0, 150.0, 6.5, 9.0),
    (262.0, 236.0, 6.0, 16.0),
)


def stacks(x, z, seed):
    """A block of basalt with a tilted grass top, an outlier beside it, and sea stacks on a
    wave-cut platform. Nothing here is a slope: a face stands or it is flat. The faces step back
    in tiers, one per lava flow, so a wall is a colonnade over a ledge over a colonnade."""
    xw, zw = f.warp(x, z, seed, 34.0, 5.0)
    # Joints: an angular noise that notches the plan into buttresses and re-entrants.
    joint = f.ridged(x, z, seed + 3, 21.0, 3) - 0.5
    # Columns: basalt cools into columns a metre or two across, and a height field extrudes any
    # plan detail vertically, so a fine ridged noise on the plan IS a fluted face.
    flute = f.ridged(x, z, seed + 4, 3.4, 2) - 0.5
    h = np.full_like(x, s.BASE)
    solid = np.zeros_like(x)

    def block(cx, cz, rx, rz, rot, top, tiers, sd):
        """`tiers` is a list of (ledge height, setback in metres); the top is the last tier."""
        nonlocal h, solid
        c, sn = np.cos(np.radians(rot)), np.sin(np.radians(rot))
        lx = (xw - cx) * c + (zw - cz) * sn
        lz = -(xw - cx) * sn + (zw - cz) * c
        # A superellipse: flat sides and rounded corners, like a jointed block, not a disc.
        u = (np.abs(lx / rx) ** 3.4 + np.abs(lz / rz) ** 3.4) ** (1.0 / 3.4)
        r = 0.5 * (rx + rz)
        edge = (1.0 - u) * r + 5.5 * joint + 0.7 * flute  # metres inside the outer wall
        level = np.full_like(x, s.BASE)
        for ledge_h, setback in tiers:
            ragged = setback * (0.55 + 0.9 * (0.5 + 0.5 * f.fbm(x, z, sd + int(ledge_h), 26.0, 3)))
            m = f.smoothstep(ragged - 1.3, ragged + 1.3, edge)
            level = f.lerp(level, ledge_h + 0.4 * f.fbm(x, z, sd + 5, 5.0, 3), m)
        setback_top = tiers[-1][1] + 5.0
        m = f.smoothstep(setback_top - 1.5, setback_top + 1.5, edge)
        level = f.lerp(level, top, m)
        solid = np.maximum(solid, f.smoothstep(-1.5, 0.5, edge))
        h = np.maximum(h, level)

    # The Keep: its top tilts from 60 m at the south-west to 96 m at the north-east.
    tilt = 78.0 + 0.19 * ((x - 168.0) * 0.82 + (z - 138.0) * 0.57) + 2.5 * f.fbm(x, z, seed + 1, 30.0, 4)
    block(168.0, 138.0, 62.0, 47.0, 22.0, tilt, [(19.0, 0.0), (47.0, 6.0)], seed)
    # The Anvil: a lower outlier to the north-east.
    block(224.0, 198.0, 26.0, 20.0, -30.0, 44.0 + 1.5 * f.fbm(x, z, seed + 2, 18.0, 3), [(17.0, 0.0)], seed + 1)
    for i, (cx, cz, radius, top) in enumerate(STACKS):
        if i == 0:
            m = f.plug(x, z, cx, cz, radius, 3.0, seed + 60, wobble=0.9)
            solid = np.maximum(solid, m)
            h = np.maximum(h, f.lerp(s.BASE, top, m))
            continue
        # A stack is what is left of a wall: a faceted column of jointed basalt, never a cone.
        # Its plan is an irregular polygon (the joints it broke along), its faces are fluted by
        # the columns, it steps in once or twice on the way up, and its head is not level.
        rot = np.radians(37.0 * i)
        lx = (x - cx) * np.cos(rot) + (z - cz) * np.sin(rot)
        lz = -(x - cx) * np.sin(rot) + (z - cz) * np.cos(rot)
        rr = _faceted(lx / 1.3, lz / 0.82, 5 + (i % 3), seed + 70 + i)  # metres from the axis
        rr = rr + 1.3 * joint + 0.55 * flute + 0.9 * f.fbm(x, z, seed + 70 + i, 9.0, 3)
        rng = np.random.default_rng(seed + 300 + i)
        col = np.full_like(x, s.BASE)
        # (radius fraction, height fraction): a boulder skirt, one or two ledges, then the head.
        tiers = [(1.42, 0.07), (1.08, rng.uniform(0.30, 0.48)), (0.86, rng.uniform(0.62, 0.8))]
        for frac, height in tiers:
            ragged = radius * frac * (1.0 + 0.10 * f.fbm(x, z, seed + 90 + i + int(frac * 10), 11.0, 2))
            col = f.lerp(col, top * height + 0.5 * f.fbm(x, z, seed + 80 + i, 4.0, 3),
                         1.0 - f.smoothstep(ragged - 0.2, ragged + 1.1, rr))
        dip = rng.uniform(0.0, 2.0 * np.pi)
        head_top = top * (1.0 - 0.012 * (lx * np.cos(dip) + lz * np.sin(dip))) + 0.8 * f.fbm(x, z, seed + 80 + i, 6.0, 3)
        col = f.lerp(col, head_top, 1.0 - f.smoothstep(radius * 0.66, radius * 0.66 + 1.0, rr))
        solid = np.maximum(solid, 1.0 - f.smoothstep(radius * 1.08, radius * 1.08 + 1.5, rr))
        h = np.maximum(h, col)

    # The platform: bare rock awash around the block and out to the light, then deep water.
    near = np.zeros_like(x)
    for cx, cz, rad in ((168.0, 138.0, 84.0), (88.0, 182.0, 44.0), (224.0, 198.0, 42.0)):
        near = np.maximum(near, f.bump(xw, zw, cx, cz, rad * (1.0 + 0.22 * f.fbm(x, z, seed + 90, 40.0, 3)), 6.0))
    awash = 0.5 + 0.9 * f.fbm(x, z, seed + 91, 7.0, 3) + 1.1 * np.clip(joint, 0.0, 1.0)
    shelf = f.lerp(_sea(60.0 * (1.0 - near), 9.0), awash, f.smoothstep(0.25, 0.6, near))
    h = np.maximum(h, np.where(solid > 0.02, h, shelf))
    # A geo on the east side with a boulder beach at its head.
    beach = f.bump(x, z, 236.0, 146.0, 20.0, 3.0)
    return {"h": h, "hard": np.full_like(h, 0.97), "beach": beach, "rock_shore": np.zeros_like(h),
            "rock_kind": np.zeros_like(h), "wind": (0.95, 0.3), "solid": solid}


def stacks_sites(h, x, z):
    """Stacks Light (Assets/Models/DriftLighthouse/README.md): tower at world (-419, 29.9, -465),
    tile-local (61, 205). Its stair's bottom tread meets the rock at 28.7 m at local
    (62.26, 200.29); the pier runs 14 m below the entry landing, so the stack's top is the rock
    it is built on."""
    return s.stamp(h, x, z, 61.0, 204.0, 28.68, 8.5, 11.0)


# ── Mesa ────────────────────────────────────────────────────────────────────────────────────
def mesa(x, z, seed):
    """A table of bedded sandstone: a caprock rim, a bench where a softer bed has weathered
    back, a second wall, and a talus apron down to a narrow red shore."""
    u, r, theta = _blob(x, z, 180.0, 182.0, 122.0, 98.0, 25.0, seed, 80.0, 9.0)
    a1 = f.fbm(np.cos(theta) * 2.1 + 5.0, np.sin(theta) * 2.1, seed + 1, 1.0, 3)
    a2 = f.fbm(np.cos(theta) * 3.3, np.sin(theta) * 3.3 + 9.0, seed + 2, 1.0, 3)
    cap_edge = 0.58 + 0.07 * a1
    bench_edge = cap_edge + 0.05 + 0.16 * np.clip(0.5 + 0.9 * a2, 0.0, 1.0)
    talus_top = 9.5 + 2.0 * a2
    wall = 0.02
    cap = 40.0 + 2.2 * f.fbm(x, z, seed + 3, 60.0, 3) + 0.6 * f.fbm(x, z, seed + 4, 9.0, 3)
    bench = 20.0 + 1.5 * f.fbm(x, z, seed + 5, 34.0, 3)
    talus = talus_top * np.clip(1.0 - (u - bench_edge) / np.maximum(1.0 - bench_edge, 0.05), 0.0, 1.0) ** 1.15
    land = f.lerp(cap, bench, f.smoothstep(cap_edge, cap_edge + wall, u))
    land = f.lerp(land, talus, f.smoothstep(bench_edge, bench_edge + wall, u))
    h = np.where(u < 1.0, land, _sea((u - 1.0) * r, 16.0))
    # The Chimney: a detached butte off the east corner.
    m = f.plug(x, z, 300.0, 250.0, 15.0, 3.0, seed + 6, wobble=1.5)
    h = np.maximum(h, f.lerp(s.BASE, 26.0, m))
    apron = f.bump(x, z, 300.0, 250.0, 34.0, 1.6)
    h = np.maximum(h, np.where(m > 0.02, h, f.lerp(s.BASE, 5.0, apron)))
    walls = f.smoothstep(cap_edge - 0.01, cap_edge + wall, u) * (1 - f.smoothstep(cap_edge + wall, cap_edge + 2 * wall, u))
    hard = np.maximum(np.where(u < bench_edge + wall, 0.92, 0.2), 0.95 * (m > 0.02))
    beach = np.clip(0.5 + 1.6 * a1, 0.0, 1.0)
    return {"h": h, "hard": hard, "beach": beach, "rock_shore": 1.0 - beach,
            "rock_kind": np.ones_like(h), "wind": (0.8, -0.5), "cap_edge": cap_edge, "u": u, "walls": walls}


# ── Dunes ───────────────────────────────────────────────────────────────────────────────────
def dunes(x, z, seed):
    """Sand on a low platform: a foredune behind the windward beach, a big parabolic dune,
    machair in its lee, and a spit hooking round a shallow lagoon."""
    u, r, _ = _blob(x, z, 180.0, 205.0, 124.0, 86.0, -14.0, seed, 110.0, 11.0)
    inland = np.maximum(1.0 - u, 0.0) * r
    platform = np.minimum(inland * 0.12, 3.2 + 0.012 * inland)
    wx, wz = 0.989, 0.148  # the scene's wind (Drift.olo WindSettings.Direction)
    along = x * wx + z * wz
    across = -x * wz + z * wx
    # Transverse dunes: a gentle windward ramp and a slip face, crests broken by a slow noise.
    lam = 54.0
    phase = (along + 16.0 * f.fbm(x, z, seed + 2, 90.0, 3)) / lam
    t = phase - np.floor(phase)
    wave = np.where(t < 0.72, t / 0.72, (1.0 - t) / 0.28)
    amp = 5.0 * f.smoothstep(26.0, 70.0, inland) * (0.55 + 0.45 * f.fbm(x, z, seed + 3, 120.0, 2))
    field = amp * wave ** 1.3

    def barchan(cx, cz, height, length, width):
        """One crescentic dune: a long windward back, a short slip face, horns trailing downwind."""
        lx = (x - cx) * wx + (z - cz) * wz
        lz = -(x - cx) * wz + (z - cz) * wx
        lx = lx - 0.011 * lz * lz  # the crest bows: its ends lead the middle downwind
        back = np.where(lx < 0.0, np.exp(-(lx / length) ** 2), np.exp(-(lx / (length * 0.3)) ** 2))
        return height * back * np.exp(-(lz / width) ** 2)

    big = np.zeros_like(x)
    for cx, cz, height, length, width in ((140.0, 222.0, 21.0, 52.0, 50.0), (190.0, 176.0, 13.0, 40.0, 36.0),
                                          (226.0, 236.0, 10.0, 34.0, 30.0), (112.0, 168.0, 8.0, 28.0, 26.0)):
        big = np.maximum(big, barchan(cx, cz, height, length, width))
    big *= f.smoothstep(8.0, 40.0, inland)
    # The foredune: one ridge parallel to the windward shore.
    fore = 5.5 * np.exp(-((inland - 34.0) / 11.0) ** 2) * f.smoothstep(-0.2, 0.5, -(x - 180.0) * wx / 124.0 + 0.2)
    land = platform + np.maximum(field, big) + fore
    h = np.where(u < 1.0, land, _sea((u - 1.0) * r, 30.0))
    # The spit: a low sand ridge from the south-east corner, hooking back west.
    spit = [(262.0, 152.0, 2.0), (298.0, 120.0, 1.8), (306.0, 88.0, 1.6), (286.0, 66.0, 1.4), (258.0, 62.0, 1.0)]
    dist, _, top = f.polyline_field(x, z, spit)
    ridge = np.where(dist < 15.0, top * (1.0 - (dist / 15.0) ** 2), _sea(dist - 15.0, 16.0) * 0.6)
    h = np.maximum(h, ridge)
    # The lagoon it encloses stays shallow.
    lagoon = f.bump(x, z, 250.0, 105.0, 46.0, 2.0)
    h = np.where((h < 0.0), np.maximum(h, f.lerp(h, -1.3, lagoon)), h)
    return {"h": h, "hard": np.zeros_like(h), "beach": np.ones_like(h), "rock_shore": np.zeros_like(h),
            "rock_kind": np.zeros_like(h), "wind": (wx, wz), "sand": np.ones_like(h), "beach_inland": 14.0}


# ── Atoll ───────────────────────────────────────────────────────────────────────────────────
# Motus on the ring: (centre angle deg, half arc deg, crest height m). The pass faces the
# start line (bearing about 145 deg from the atoll's centre).
MOTUS = ((5.0, 66.0, 6.2), (100.0, 17.0, 3.8), (205.0, 21.0, 4.4), (268.0, 24.0, 5.0))
PASS_ANGLE, PASS_HALF = 148.0, 7.5


def atoll(x, z, seed):
    """A ring reef awash, four sand cays on it, a lagoon inside and one pass through."""
    xw, zw = f.warp(x, z, seed, 130.0, 10.0)
    lx, lz = xw - 200.0, zw - 200.0
    theta = np.arctan2(lz, lx)
    ring_r = 116.0 * (1.0 + 0.10 * np.cos(2 * theta + 0.7) + 0.05 * np.cos(3 * theta - 1.1))
    r = np.hypot(lx, lz)
    d = r - ring_r  # metres outside the reef crest (negative = toward the lagoon)
    reef = -0.45 + 0.25 * f.fbm(x, z, seed + 1, 8.0, 3)
    flat_in = 26.0 + 8.0 * f.fbm(x, z, seed + 2, 70.0, 2)
    lagoon = -2.0 - 5.0 * f.smoothstep(0.0, 70.0, -d - flat_in) + 0.5 * f.fbm(x, z, seed + 3, 26.0, 3)
    # Bommies: coral heads rising toward the surface inside the lagoon.
    heads = f.smoothstep(0.62, 0.8, f.fbm(x, z, seed + 4, 22.0, 2) * 0.5 + 0.5) * f.smoothstep(10.0, 30.0, -d - flat_in)
    lagoon = f.lerp(lagoon, -1.0, heads)
    inside = f.lerp(reef, lagoon, f.smoothstep(flat_in * 0.6, flat_in * 1.25, -d))
    outside = reef + s.submarine(-s.BASE + reef, d - 5.0, 13.0)
    h = np.where(d < 0.0, inside, np.where(d < 5.0, reef, outside))
    deg = np.degrees(theta)
    sand = np.zeros_like(h)
    for centre, half, crest in MOTUS:
        da = np.abs((deg - centre + 180.0) % 360.0 - 180.0)
        # A cay is a lens of sand the waves have piled on the reef flat: widest and highest in
        # its middle, drawn out to a point at each end, its outline wandering with the reef. The
        # arc it covers is itself uneven, or every cay would end on a radial line.
        da = da + 0.11 * half * f.fbm(x, z, seed + 8 + int(centre), 30.0, 3)
        along = np.clip(1.0 - (da / half) ** 2, 0.0, 1.0)
        width = 27.0 * (0.55 + 0.75 * (0.5 + 0.5 * f.fbm(x, z, seed + 7, 52.0, 2))) * (0.25 + 0.75 * along ** 0.7)
        across = (d + 20.0 + 7.0 * f.fbm(x, z, seed + 5, 44.0, 2)) / width  # centred just inside the crest
        body = np.clip(1.0 - across * across, 0.0, 1.0) * along ** 0.9
        motu = crest * body ** 0.75 + 0.6 * f.fbm(x, z, seed + 6, 12.0, 3) * body
        h = np.where(body > 0.0, np.maximum(h, -0.4 + motu), h)
        sand = np.maximum(sand, f.smoothstep(0.0, 0.2, body))
    # The pass: a channel through the reef.
    da = np.abs((deg - PASS_ANGLE + 180.0) % 360.0 - 180.0)
    gate = (1.0 - f.smoothstep(PASS_HALF * 0.6, PASS_HALF, da)) * f.smoothstep(-70.0, -40.0, d) * (1 - f.smoothstep(20.0, 45.0, d))
    h = f.lerp(h, np.minimum(h, -5.0), gate)
    return {"h": h, "hard": np.full_like(h, 0.6), "beach": sand, "rock_shore": np.zeros_like(h),
            "rock_kind": np.ones_like(h), "wind": (0.9, 0.4), "sand": sand, "reef_d": d, "flat_in": flat_in,
            "beach_inland": 7.0}


RECIPES = {
    "Ridgeback": (ridgeback, ridgeback_sites),
    "Sisters": (sisters, None),
    "Stacks": (stacks, stacks_sites),
    "Mesa": (mesa, None),
    "Dunes": (dunes, None),
    "Atoll": (atoll, None),
}

# Erosion per island: (hydraulic droplets per land cell, capacity, thermal repose deg on soft
# ground, thermal iterations). Sand does not gully (rain soaks in), so Dunes and Atoll only slump.
EROSION = {
    "Ridgeback": (1.6, 4.0, 37.0, 30),
    "Sisters": (0.6, 3.5, 38.0, 24),
    "Stacks": (0.3, 2.5, 40.0, 12),
    "Mesa": (0.8, 4.0, 34.0, 40),
    "Dunes": (0.0, 0.0, 31.0, 60),
    "Atoll": (0.0, 0.0, 30.0, 30),
}
