"""The Dog body generator (issue #1533): an original scruffy golden-retriever-type mutt.

Run headless:

    blender -b --factory-startup --python build_dog.py -- [out_dir] [preview_dir]

and it writes, into out_dir (default: this directory), everything the engine loads:

    Dog.gltf + Dog.bin   one skinned mesh (body, eyelid shells, tongue, teeth; eight materials
                         named by contract) on a 50-bone rig, with the clips Idle, HeadTilt,
                         Sit, Walk and Pant (30 fps, in place)
    DogCoatColor.png     the coat colours, both DogSkin's albedo and the groom's colour map
    Dog.rig.json         the eye radius, centres and gaze (the engine draws each eye as its own
                         sphere entity attached to eye_L / eye_R), the clip list
    DogEyeball.gltf      the unit sphere each eye entity draws, at close-up tessellation

Nothing is hand-edited after generation (issue #1533, criterion A6). The surface is a signed
distance field sculpted from primitives, meshed with surface nets, repaired to a manifold,
decimated against a size field (fine on the bare face, coarse under the torso's long coat),
refined and relaxed onto the field, and cut exactly along every material boundary. Weights are
Blender's bone heat, constrained region by region; the clips are authored as procedural poses.
README.md describes the design and the contract with the engine.
"""

import math
import os
import sys
import time

import numpy as np

# ----------------------------------------------------------------------------
# Blender coordinates throughout: metres, Z up, the dog faces -Y (glTF +Z),
# +X is the dog's LEFT (the "_L" side), paws on z = 0.
# ----------------------------------------------------------------------------

F32 = np.float32


def v3(a):
    return np.asarray(a, dtype=F32)


# ----------------------------------------------------------------------------
# SDF primitives, vectorised over an (N, 3) float32 array of points
# ----------------------------------------------------------------------------


def _norm(q):
    return np.sqrt(np.einsum("ij,ij->i", q, q))


def sd_sphere(p, c, r):
    return _norm(p - v3(c)) - F32(r)


def sd_ellipsoid(p, c, r, rot=None):
    """Ellipsoid (IQ's bound). `rot` columns are the ellipsoid's local axes."""
    q = p - v3(c)
    if rot is not None:
        q = q @ rot
    r = v3(r)
    k0 = _norm(q / r)
    k1 = _norm(q / (r * r))
    return k0 * (k0 - F32(1.0)) / np.maximum(k1, F32(1e-9))


def sd_capsule(p, a, b, r):
    a = v3(a)
    b = v3(b)
    pa = p - a
    ba = b - a
    h = np.clip((pa @ ba) / F32(ba @ ba), 0.0, 1.0).astype(F32)
    return _norm(pa - h[:, None] * ba) - F32(r)


def sd_round_cone(p, a, b, ra, rb):
    """Capsule with radius ra at a and rb at b (IQ's round cone, exact)."""
    a = v3(a)
    b = v3(b)
    ba = b - a
    l2 = float(ba @ ba)
    rr = ra - rb
    a2 = l2 - rr * rr
    il2 = 1.0 / l2
    pa = p - a
    y = pa @ ba
    z = y - l2
    x2v = pa * F32(l2) - np.outer(y, ba)
    x2 = np.einsum("ij,ij->i", x2v, x2v)
    y2 = y * y * F32(l2)
    z2 = z * z * F32(l2)
    k = math.copysign(1.0, rr) * rr * rr * x2
    out = ((np.sqrt(np.maximum(x2 * F32(a2 * il2), 0.0)) + y * F32(rr)) * F32(il2) - F32(ra)).astype(F32)
    m1 = np.sign(z) * F32(a2) * z2 > k
    m2 = (np.sign(y) * F32(a2) * y2 < k) & ~m1
    out[m1] = np.sqrt(x2[m1] + z2[m1]) * F32(il2) - F32(rb)
    out[m2] = np.sqrt(x2[m2] + y2[m2]) * F32(il2) - F32(ra)
    return out


def sd_round_box(p, c, b, r):
    q = np.abs(p - v3(c)) - v3(b)
    outside = _norm(np.maximum(q, 0.0))
    inside = np.minimum(np.max(q, axis=1), 0.0)
    return (outside + inside - F32(r)).astype(F32)


def sd_ellipse2(q, r):
    """2D ellipse bound for an (N, 2) array."""
    r = np.asarray(r, dtype=F32)
    k0 = np.sqrt(np.einsum("ij,ij->i", q / r, q / r))
    k1 = np.sqrt(np.einsum("ij,ij->i", q / (r * r), q / (r * r)))
    return k0 * (k0 - F32(1.0)) / np.maximum(k1, F32(1e-9))


def smin(a, b, k):
    if k <= 0.0:
        return np.minimum(a, b)
    h = np.clip(F32(0.5) + F32(0.5 / k) * (b - a), 0.0, 1.0)
    return b + (a - b) * h - F32(k) * h * (F32(1.0) - h)


def smax(a, b, k):
    return -smin(-a, -b, k)


def ssub(a, b, k):
    """a minus b, smoothly."""
    return smax(a, -b, k)


def chain(p, pts, radii, k):
    d = None
    for i in range(len(pts) - 1):
        s = sd_round_cone(p, pts[i], pts[i + 1], radii[i], radii[i + 1])
        d = s if d is None else smin(d, s, k)
    return d


def rot_xyz(rx, ry, rz):
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return (Rz @ Ry @ Rx).astype(F32)


def in_box(p, lo, hi, fn, far=1.0):
    """Evaluate fn only where p lies in [lo, hi]; `far` elsewhere (safe for unions and subtractions)."""
    m = np.all((p >= v3(lo)) & (p <= v3(hi)), axis=1)
    out = np.full(len(p), far, dtype=F32)
    if m.any():
        out[m] = fn(p[m])
    return out


# ----------------------------------------------------------------------------
# Design constants (the proportions of the approved prototype, refined)
# ----------------------------------------------------------------------------

HEAD = v3((0.0, -0.382, 0.580))  # centre of the head construction frame
EYE_R = 0.0235  # eyeball radius
EYE_GAZE_OUT = 0.06  # lateral component of the (un-normalised) gaze: nearly forward, so the dog looks AT you
EYE_OFF = v3((0.052, -0.088, 0.004))  # left eye centre relative to HEAD
SOCKET_CLEAR = 0.00275  # socket radius = EYE_R + this: just clears the stacked upper lid (a wider gap reads as a void);
# it follows LID_THICK, or the gap under the skin's edge shows as a dark ring round a closed eye
LID_GAP = 0.0010  # lid inner radius = EYE_R + this
LID_THICK = 0.0013  # the rolled margin is this thick, and it reads as the eye's dark outline: at 1.75 mm, eyeliner
MOUTH_GAP = 0.0050  # the lip slit / oral pocket gap between the upper head and the jaw


def eye_centre(side):
    """side = +1 left (+X), -1 right."""
    return HEAD + v3((side * EYE_OFF[0], EYE_OFF[1], EYE_OFF[2]))


def eye_gaze(side):
    g = np.array((side * EYE_GAZE_OUT, -1.0, -0.02))
    return (g / np.linalg.norm(g)).astype(F32)


# Front and hind legs: joint positions (left side; mirror x for the right).
FRONT_JOINTS = [(0.072, -0.165, 0.445), (0.080, -0.182, 0.338), (0.084, -0.160, 0.210), (0.079, -0.176, 0.094),
                (0.079, -0.188, 0.044), (0.079, -0.232, 0.022)]  # scapula top, shoulder, elbow, wrist, paw, toe
HIND_JOINTS = [(0.080, 0.168, 0.352), (0.088, 0.132, 0.214), (0.080, 0.214, 0.108), (0.080, 0.196, 0.044),
               (0.080, 0.150, 0.021)]  # hip, stifle, hock, paw, toe
TAIL_PTS = [(0.0, 0.242, 0.398), (0.0, 0.292, 0.424), (0.0, 0.340, 0.452), (0.0, 0.386, 0.484), (0.0, 0.426, 0.520),
            (0.0, 0.458, 0.560), (0.0, 0.480, 0.600)]
TAIL_R = [0.043, 0.040, 0.036, 0.032, 0.028, 0.024, 0.020]


# ----------------------------------------------------------------------------
# The dog SDF, by region
# ----------------------------------------------------------------------------


def sd_tri2(p, a, b, c):
    """Exact 2D triangle SDF (IQ) for an (N, 2) array."""
    a, b, c = (np.asarray(t, dtype=F32) for t in (a, b, c))
    e0, e1, e2 = b - a, c - b, a - c
    v0, v1, v2 = p - a, p - b, p - c

    def seg(v, e):
        h = np.clip((v @ e) / F32(e @ e), 0.0, 1.0)[:, None]
        w = v - e * h
        return np.einsum("ij,ij->i", w, w)

    s = np.sign(e0[0] * e2[1] - e0[1] * e2[0])
    dd = np.minimum(np.minimum(seg(v0, e0), seg(v1, e1)), seg(v2, e2))
    c0 = s * (v0[:, 0] * e0[1] - v0[:, 1] * e0[0])
    c1 = s * (v1[:, 0] * e1[1] - v1[:, 1] * e1[0])
    c2 = s * (v2[:, 0] * e2[1] - v2[:, 1] * e2[0])
    cc = np.minimum(np.minimum(c0, c1), c2)
    return (-np.sqrt(dd) * np.sign(cc)).astype(F32)


def hrel(p):
    return p - HEAD


def f_cranium(q):
    d = sd_ellipsoid(q, (0.0, 0.004, 0.020), (0.110, 0.108, 0.104))
    return smin(d, sd_ellipsoid(q, (0.0, -0.058, 0.030), (0.072, 0.045, 0.058)), 0.03)  # forehead: a clear stop


CHEEKS = ((0.0, -0.070, -0.040), (0.086, 0.066, 0.058))
JOWL = ((0.050, -0.022, -0.048), (0.052, 0.062, 0.052))  # masseter mass under the ear (left; mirror x)


def f_jowls(q):
    a = sd_ellipsoid(q, JOWL[0], JOWL[1])
    b = sd_ellipsoid(q, (-JOWL[0][0], JOWL[0][1], JOWL[0][2]), JOWL[1])
    return smin(a, b, 0.02)


def f_head_simple(q):
    """Cranium + cheeks + jowls + jaw bulk: the smooth surface the ears hang against."""
    d = f_cranium(q)
    d = smin(d, sd_ellipsoid(q, *CHEEKS), 0.04)
    d = smin(d, f_jowls(q), 0.03)
    d = smin(d, sd_ellipsoid(q, (0.0, -0.055, -0.080), (0.066, 0.066, 0.042)), 0.04)
    return d


JAW_R = rot_xyz(math.radians(-10.0), 0.0, 0.0)  # the chin recedes under the upper lip


def f_jaw(q):
    """The lower jaw: mandible, lower lip and chin, with the tongue bed on top."""
    d = sd_ellipsoid(q, (0.0, -0.112, -0.0905), (0.030, 0.068, 0.0125), JAW_R)
    d = smin(d, sd_ellipsoid(q, (0.0, -0.142, -0.0955), (0.016, 0.022, 0.0085), JAW_R), 0.012)  # chin
    d = smin(d, sd_ellipsoid(q, (0.0, -0.035, -0.087), (0.052, 0.048, 0.031)), 0.03)  # back of the jaw
    d = ssub(d, sd_ellipsoid(q, (0.0, -0.110, -0.0735), (0.020, 0.058, 0.0075)), 0.005)  # tongue bed
    return d


POCKET_END = -0.068  # the oral pocket (and the lip slit) ends here with a rounded cap: the mouth corner


def f_pocket(q):
    """The oral pocket: the jaw dilated by MOUTH_GAP, capped at POCKET_END.

    A full-width gap that ends in a rounded wall, never a slit tapering to nothing: a gap
    narrower than a voxel makes surface nets emit non-manifold edges."""
    return smax(f_jaw(q) - F32(MOUTH_GAP), q[:, 1] - F32(POCKET_END), 0.004)


MUZZLE_C = (0.0, -0.140, -0.037)
MUZZLE_R = rot_xyz(math.radians(2.0), 0.0, 0.0)  # the bridge slopes down toward the nose
# The nose leather's front outline, a rounded trapezoid in (x, z): wide and square-shouldered at the
# top, narrowing to the philtrum below. (top half-width, bottom half-width, top z, bottom z), before
# rounding. A button with volume rather than a plate: 3.9 cm deep front to back.
NOSE_TRAP = (0.0240, 0.0115, -0.0115, -0.0365)
NOSE_ROUND = 0.0090
NOSE_C = (0.0, -0.1925, -0.0225)
NOSE_R = (0.034, 0.0195, 0.0245)
NOSE_ROT = rot_xyz(math.radians(-10.0), 0.0, 0.0)  # the leather leans forward at the top, its underside recedes


def sd_trapezoid2(p, r1, r2, he):
    """Exact 2D isosceles trapezoid SDF (IQ) for an (N, 2) array: half-width r1 at y = -he, r2 at y = +he."""
    x = np.abs(p[:, 0])
    y = p[:, 1]
    k1 = np.array((r2, he), dtype=F32)
    k2 = np.array((r2 - r1, 2.0 * he), dtype=F32)
    cax = x - np.minimum(x, np.where(y < 0.0, F32(r1), F32(r2)))
    cay = np.abs(y) - F32(he)
    q = np.stack([x, y], axis=1)
    t = np.clip(((k1[None, :] - q) @ k2) / F32(k2 @ k2), 0.0, 1.0)[:, None]
    cb = q - k1[None, :] + k2[None, :] * t
    s = np.where((cb[:, 0] < 0.0) & (cay < 0.0), -1.0, 1.0).astype(F32)
    d2 = np.minimum(cax * cax + cay * cay, np.einsum("ij,ij->i", cb, cb))
    return (s * np.sqrt(d2)).astype(F32)


def nose_outline(q):
    """Signed distance to the nose leather's outline in the front (x, z) plane."""
    top, bottom, z_top, z_bottom = NOSE_TRAP
    zc = 0.5 * (z_top + z_bottom)
    he = 0.5 * (z_top - z_bottom)
    p2 = np.stack([q[:, 0], q[:, 2] - F32(zc)], axis=1)
    r = NOSE_ROUND
    return sd_trapezoid2(p2, bottom - r, top - r, he - r) - F32(r)


def f_nose(q):
    # The blend is the edge where the front of the leather turns into its sides. At 3 mm it caught
    # the sky as one even ring and the nose read as a bevelled button (Ole's review, #1533).
    return smax(sd_ellipsoid(q, NOSE_C, NOSE_R, NOSE_ROT), nose_outline(q), 0.0055)


def f_muzzle(q):
    """A slightly tapered, squared-off muzzle."""
    m = (q - v3(MUZZLE_C)) @ MUZZLE_R
    taper = np.clip(F32(1.0) + F32(2.2) * m[:, 1], 0.80, 1.12).astype(F32)  # narrower toward the nose (-y)
    m = m * np.stack([F32(1.0) / taper, np.ones_like(taper), np.ones_like(taper)], axis=1)
    return sd_round_box(m, (0.0, 0.0, 0.0), (0.028, 0.045, 0.018), 0.013)


def f_upper_head(q):
    """Cranium, cheeks, jowls, muzzle, whisker pads, flews, nose, brows (before carving)."""
    d = f_cranium(q)
    d = smin(d, sd_ellipsoid(q, *CHEEKS), 0.04)
    d = smin(d, f_jowls(q), 0.03)
    d = smin(d, f_muzzle(q), 0.028)
    # whisker pads (upper lips) either side of the philtrum, and the flews behind them: subtle
    pads = smin(sd_ellipsoid(q, (0.0170, -0.180, -0.064), (0.0175, 0.017, 0.0215)),
                sd_ellipsoid(q, (-0.0170, -0.180, -0.064), (0.0175, 0.017, 0.0215)), 0.010)
    d = smin(d, pads, 0.012)
    flews = smin(sd_ellipsoid(q, (0.034, -0.130, -0.071), (0.018, 0.052, 0.020)),
                 sd_ellipsoid(q, (-0.034, -0.130, -0.071), (0.018, 0.052, 0.020)), 0.01)
    d = smin(d, flews, 0.02)
    d = smin(d, f_nose(q), 0.0025)
    brows = smin(sd_ellipsoid(q, (0.046, -0.080, 0.034), (0.026, 0.020, 0.011)),
                 sd_ellipsoid(q, (-0.046, -0.080, 0.034), (0.026, 0.020, 0.011)), 0.005)
    d = smin(d, brows, 0.022)
    return d


# (x, z, radius) of the left comma: a small round opening low and inboard, its tail sweeping out and
# UP the side of the leather along the alar groove — a dog's nostril, not a pair of round holes that
# read as a second pair of eyes.
NOSTRIL_PATH = [(0.0082, -0.0262, 0.0030), (0.0120, -0.0276, 0.0025), (0.0158, -0.0268, 0.0019),
                (0.0188, -0.0242, 0.0015), (0.0204, -0.0208, 0.0012)]


def _nose_front_y(x, z):
    """y of the nose leather's front surface at (x, z) in the head frame (bisection on f_nose)."""
    lo, hi = -0.24, -0.17
    for _ in range(40):
        m = 0.5 * (lo + hi)
        if f_nose(v3([(x, m, z)]))[0] < 0.0:
            hi = m
        else:
            lo = m
    return 0.5 * (lo + hi)


def _nostril_chain():
    pts = []
    for x, z, r in NOSTRIL_PATH:
        y = _nose_front_y(x, z)
        pts.append(((x, y + 0.30 * r, z), r))  # centres just under the surface: a groove ~1.3 r deep
    return pts


NOSTRIL_CHAIN = None
NOSTRIL_DRY = 0.0015  # metres into the leather where a nostril's wall stops being wet leather


def f_nostrils(q):
    """Comma-shaped nostrils: a deep round hole medially, tapering into a slit that follows the
    leather's surface out and up along the side of the nose."""
    global NOSTRIL_CHAIN
    if NOSTRIL_CHAIN is None:
        NOSTRIL_CHAIN = _nostril_chain()
    d = None
    for s in (1.0, -1.0):
        pts = [((s * c[0], c[1], c[2]), r) for c, r in NOSTRIL_CHAIN]
        g = sd_round_cone(q, pts[0][0], (pts[0][0][0], pts[0][0][1] + 0.0090, pts[0][0][2] + 0.0015), pts[0][1], 0.0030)
        for (c0, r0), (c1, r1) in zip(pts[:-1], pts[1:]):
            g = np.minimum(g, sd_round_cone(q, c0, c1, r0, r1))
        d = g if d is None else np.minimum(d, g)
    return d


def f_philtrum(q):
    return sd_capsule(q, (0.0, -0.2010, -0.040), (0.0, -0.1975, -0.083), 0.0022)


def f_head(q):
    """The whole head surface: upper head carved by the nostrils, philtrum and the oral pocket, plus the jaw."""
    u = f_upper_head(q)
    u = ssub(u, f_nostrils(q), 0.0012)
    u = ssub(u, f_philtrum(q), 0.004)
    j = f_jaw(q)
    u = ssub(u, f_pocket(q), 0.0025)
    d = smin(u, j, 0.0015)
    for s in (1.0, -1.0):
        ball = sd_sphere(q, EYE_OFF * v3((s, 1, 1)), EYE_R + SOCKET_CLEAR)
        d = ssub(d, ball, SOCKET_FILLET)
        d = smin(d, f_orbit_skin(q, s), ORBIT_BLEND)
        d = ssub(d, ball, SOCKET_EDGE)
    return d


# The skin round the eye opening. The socket is a sphere carved out of the head, and the head's own
# surface met it far out -- 59 degrees off the gaze above the eye, 105 at the temple -- so the lid
# shells showed as a bald dome in a crater and the eye read as glued into a porthole. This shell of
# skin lies over the socket everywhere outside an opening a few degrees wider than the lids' almond,
# so the lids show as a band round the eye, as a real dog's do, and slide under it in a blink. Below
# the eye and toward the nose the face already closes in nearer than the opening, and is left alone.
ORBIT_SKIN = 0.0035  # its thickness over the socket (m): at 1.1 mm voxels, thinner meshes with holes
# The opening follows the lids' own almond, ORBIT_BAND degrees outside their margins at rest. A
# four-point outline (46 / 60 / 52 / 64 degrees up, outer, down, inner) left 12 to 14 degrees of lid
# showing -- a smooth, pale band of short lid fur round every eye, and the eyes read as glass balls in
# portholes. A real dog's face fur runs up to a thin dark eyelid.
ORBIT_BAND = 5.0
ORBIT_EDGE = 0.002  # the rounding of the opening's edge
ORBIT_BLEND = 0.006  # how far the skin blends into the face
# The first carve's fillet: where the face meets the socket. Below the eye the cheek closes over the
# globe 17 to 23 degrees off the gaze, above the lower lid, and at 6 mm this fillet rolled the cheek
# into the socket as a wide, smooth lip -- short-furred, lit from the sky, a pale crescent under
# every eye.
SOCKET_FILLET = 0.003
# The socket re-carved through the skin. The first carve's fillet is wider than the skin is thick and
# ate it whole; this one only has to clear the lids.
SOCKET_EDGE = 0.0015


# The lid under the skin keeps its fur this far (degrees) past the opening: the skin rides the brow
# bones, and a raised brow shows more lid. Lid skin that no blink or brow brings into view is left
# bare, or its fur grows out through the face.
ORBIT_LID_MARGIN = 12.0


_LID_APERTURE = {}


def lid_aperture(s):
    """The lids' opening at rest for the eye on side s, as (around, theta) samples in radians: every
    point of both margins inside the corners, `around` measured about the gaze as in orbit_angles and
    sorted for a periodic interpolation."""
    if s not in _LID_APERTURE:
        _c, f, up, axis = eye_frame(s)
        samples = []
        for upper in (True, False):
            for ps in np.radians(np.linspace(-LID_PSI, LID_PSI, 1441)):
                if abs(ps) > lid_fissure(ps, s):
                    continue
                ph = math.radians(lid_margin(ps, upper, s))
                d = math.cos(ps) * (math.cos(ph) * f + math.sin(ph) * up) + math.sin(ps) * axis
                theta = math.atan2(math.hypot(float(d @ up), float(d @ axis)), float(d @ f))
                around = (math.atan2(float(d @ axis), float(d @ up)) * s) % (2.0 * math.pi)
                samples.append((around, theta))
        samples.sort()
        _LID_APERTURE[s] = (np.array([a for a, _ in samples]), np.array([t for _, t in samples]))
    return _LID_APERTURE[s]


def orbit_angles(rel, s):
    """(theta, opening) for offsets `rel` (N x 3) from the centre of the eye on side s: each offset's
    angle off the gaze, and the opening's half-angle in its direction (radians)."""
    _c, f, up, axis = eye_frame(s)
    rel = np.asarray(rel, dtype=np.float64)
    fz = rel @ f
    fu = rel @ up
    fa = rel @ axis
    theta = np.arctan2(np.sqrt(fu * fu + fa * fa), fz)
    # 0 straight up the eye, +90 toward its OUTER corner: `axis` is the dog's left for either eye.
    around = np.mod(np.arctan2(fa, fu) * s, 2.0 * math.pi)
    aperture_around, aperture_theta = lid_aperture(s)
    opening = np.interp(around, aperture_around, aperture_theta, period=2.0 * math.pi) + math.radians(ORBIT_BAND)
    return theta, opening


def f_orbit_skin(q, s):
    """The skin over the socket outside the eye opening, for the eye on side s (head frame)."""
    c = (EYE_OFF * v3((s, 1, 1))).astype(np.float64)
    rel = np.asarray(q, dtype=np.float64) - c
    r = np.linalg.norm(rel, axis=1)
    theta, opening = orbit_angles(rel, s)
    cone = r * np.sin(np.clip(theta - opening, -0.5 * math.pi, 0.5 * math.pi))  # < 0 inside the opening
    rs = EYE_R + SOCKET_CLEAR
    shell = np.abs(r - (rs + 0.5 * ORBIT_SKIN)) - 0.5 * ORBIT_SKIN
    return ssub(shell.astype(F32), cone.astype(F32), ORBIT_EDGE)


EAR_TOP = 0.034  # the ear's root line (z, head frame): level with the top of the eye
EAR_THICK_TOP = 0.0075
EAR_THICK_TIP = 0.0055
EAR_OUTLINE_C = (0.012, -0.036)  # (y, z) centre of the leaf outline
EAR_OUTLINE_R = (0.063, 0.098)  # (y, z) semi-axes: ~12.6 cm wide, tip just below the jaw line
EAR_HANG_Z = -0.046  # the flap hangs straight(ish) down below this height
EAR_FOLD = 0.0032  # extra leather thickness at the root fold


def f_ear(q, s):
    """One ear (s = +1 left): a wide, thin, free leather flap hanging from a folded root beside the cheek."""
    x = q[:, 0] * F32(s)
    y = q[:, 1]
    z = q[:, 2]
    # Below the jowls' widest level the flap only follows the head 30% of the way in, so it hangs
    # down beside the jaw like a curtain instead of wrapping under it.
    dz = z - F32(EAR_HANG_Z)
    zc = (z - F32(0.35) * (dz - np.sqrt(dz * dz + F32(0.02 ** 2)))).astype(F32)  # smooth: z above, 0.3 slope below
    dh = f_head_simple(np.stack([x, y, zc], axis=1))
    tt = np.clip((EAR_TOP - z) / 0.17, 0.0, 1.0).astype(F32)
    thick = F32(EAR_THICK_TOP) + (F32(EAR_THICK_TIP) - F32(EAR_THICK_TOP)) * tt
    # the gentle fold at the root: the leather is thicker where it rolls over the skull edge
    fold = F32(EAR_FOLD) * np.exp(-(((z - F32(EAR_TOP - 0.010)) / F32(0.010)) ** 2)).astype(F32)
    # gap to the head: zero at the root (attached), opening to ~7 mm by mid-ear and ~10 mm at the tip
    root = np.clip((EAR_TOP - 0.006 - z) / 0.040, 0.0, 1.0).astype(F32)
    root = root * root * (F32(3.0) - F32(2.0) * root)
    gap = (F32(0.0070) + F32(0.0035) * tt) * root
    total = thick + fold
    shell = np.abs(dh - (gap + total * F32(0.5))) - total * F32(0.5)
    outline = sd_ellipse2(np.stack([y - F32(EAR_OUTLINE_C[0]), z - F32(EAR_OUTLINE_C[1])], axis=1), EAR_OUTLINE_R)
    outline = smax(outline, z - F32(EAR_TOP), 0.020)
    flap = smax(shell, outline, 0.0035)
    return smax(flap, F32(0.03) - x, 0.0)


def f_torso(p):
    d = sd_ellipsoid(p, (0.0, -0.150, 0.325), (0.140, 0.150, 0.148))
    d = smin(d, sd_ellipsoid(p, (0.0, 0.020, 0.340), (0.116, 0.130, 0.113)), 0.06)
    d = smin(d, sd_ellipsoid(p, (0.0, 0.150, 0.345), (0.114, 0.110, 0.115)), 0.06)
    d = smin(d, sd_ellipsoid(p, (0.0, -0.240, 0.290), (0.100, 0.070, 0.100)), 0.05)  # brisket
    return d


def f_neck(p):
    return chain(p, [(0.0, -0.228, 0.402), (0.0, -0.285, 0.466), (0.0, -0.326, 0.520)], [0.100, 0.092, 0.086], 0.004)


def f_front_leg(p, s):
    J = [v3((s * a[0], a[1], a[2])) for a in FRONT_JOINTS]
    leg = chain(p, [J[1], J[2], J[3], J[4]], [0.060, 0.046, 0.036, 0.037], 0.02)
    paw = sd_ellipsoid(p, (s * 0.079, -0.203, 0.030), (0.047, 0.050, 0.030))
    for tx in (-0.022, -0.008, 0.008, 0.022):
        paw = smin(paw, sd_ellipsoid(p, (s * 0.079 + tx, -0.232 + 0.12 * tx * tx / 0.022, 0.025),
                                     (0.0120, 0.0125, 0.017)), 0.008)
    return smin(leg, paw, 0.018)


def f_hind_leg(p, s):
    J = [v3((s * a[0], a[1], a[2])) for a in HIND_JOINTS]
    leg = chain(p, [J[0], J[1], J[2], J[3]], [0.066, 0.047, 0.034, 0.035], 0.02)
    thigh = sd_ellipsoid(p, (s * 0.084, 0.155, 0.285), (0.058, 0.078, 0.100))
    paw = sd_ellipsoid(p, (s * 0.080, 0.178, 0.029), (0.045, 0.048, 0.029))
    for tx in (-0.021, -0.007, 0.007, 0.021):
        paw = smin(paw, sd_ellipsoid(p, (s * 0.080 + tx, 0.150 + 0.12 * tx * tx / 0.021, 0.024),
                                     (0.0115, 0.012, 0.016)), 0.008)
    return smin(smin(leg, thigh, 0.03), paw, 0.018)


def f_tail(p):
    return chain(p, TAIL_PTS, TAIL_R, 0.002)


HEAD_LO = HEAD + v3((-0.16, -0.23, -0.17))
HEAD_HI = HEAD + v3((0.16, 0.17, 0.15))


def region_fields(p):
    """Every region's own SDF at p (for labels); +1 m outside each region's box."""
    far = 1.0
    out = dict(torso=in_box(p, (-0.20, -0.40, 0.10), (0.20, 0.36, 0.56), f_torso, far),
               neck=in_box(p, (-0.16, -0.42, 0.28), (0.16, -0.14, 0.64), f_neck, far),
               head=in_box(p, HEAD_LO, HEAD_HI, lambda pp: f_head(hrel(pp)), far),
               tail=in_box(p, (-0.08, 0.17, 0.30), (0.08, 0.56, 0.66), f_tail, far),
               ear_l=in_box(p, HEAD_LO, HEAD_HI, lambda pp: f_ear(hrel(pp), 1.0), far),
               ear_r=in_box(p, HEAD_LO, HEAD_HI, lambda pp: f_ear(hrel(pp), -1.0), far))
    for key, s in (("fl", 1.0), ("fr", -1.0)):
        fx = sorted((0.0, s * 0.17))
        out[key] = in_box(p, (fx[0] - 0.02, -0.33, -0.01), (fx[1] + 0.02, -0.05, 0.52), lambda pp, s=s: f_front_leg(pp, s), far)
    for key, s in (("hl", 1.0), ("hr", -1.0)):
        fx = sorted((0.0, s * 0.17))
        out[key] = in_box(p, (fx[0] - 0.02, 0.04, -0.01), (fx[1] + 0.02, 0.30, 0.50), lambda pp, s=s: f_hind_leg(pp, s), far)
    return out


def dog_sdf(p):
    """The combined dog surface, with bounding-box culling for speed."""
    far = 1.0
    body = smin(f_torso(p), f_neck(p), 0.06)
    for s in (1.0, -1.0):
        fx = sorted((s * 0.0, s * 0.17))
        body = smin(body, in_box(p, (fx[0] - 0.02, -0.33, -0.01), (fx[1] + 0.02, -0.05, 0.52),
                                 lambda pp, s=s: f_front_leg(pp, s), far), 0.04)
        body = smin(body, in_box(p, (fx[0] - 0.02, 0.04, -0.01), (fx[1] + 0.02, 0.30, 0.50),
                                 lambda pp, s=s: f_hind_leg(pp, s), far), 0.04)
    body = smin(body, in_box(p, (-0.08, 0.17, 0.30), (0.08, 0.56, 0.66), f_tail, far), 0.03)

    def head_part(pp):
        q = hrel(pp)
        d = f_head(q)
        for s in (1.0, -1.0):
            d = smin(d, f_ear(q, s), 0.004)
        return d

    head = in_box(p, HEAD_LO, HEAD_HI, head_part, far)
    return smin(body, head, 0.045)


# ----------------------------------------------------------------------------
# Materials (names are a contract with the engine scene)
# ----------------------------------------------------------------------------

MATERIALS = ["DogSkin", "DogNose", "DogLip", "DogGum", "DogTongue", "DogTeeth", "DogPad", "DogLid"]
M_SKIN, M_NOSE, M_LIP, M_GUM, M_TONGUE, M_TEETH, M_PAD, M_LID = range(8)
LIP_BAND = 0.0040  # dark lip band on the outer skin next to the mouth opening
LIP_INNER = 0.0035  # how far the dark lip continues into the mouth before the gum starts
# Dark, furless skin just outside the socket sphere: its walls, and the underside of the skin over the
# lids (f_orbit_skin), which show only in the hair's-breadth gap round the lids. At 2.2 mm it ran onto
# the head's own surface as a dark bald ring round the whole crater, a porthole round each eye; the
# eye's dark rim is the lids' margin band.
EYE_RIM = 0.0002


def classify_faces(centres, normals):
    """Material index for each body face, from where its centre sits relative to the SDF parts."""
    p = np.asarray(centres, dtype=F32)
    n = np.asarray(normals, dtype=F32)
    q = hrel(p)
    out = np.zeros(len(p), dtype=np.int32)
    near_head = np.all((p >= HEAD_LO) & (p <= HEAD_HI), axis=1)
    hq = q[near_head]
    idx = np.nonzero(near_head)[0]
    # nose leather (and its nostrils)
    d_nose = f_nose(hq)
    nose = d_nose < 0.0009
    # the mouth: pocket walls, lip rims, gums
    dU = f_upper_head(hq)
    dJ = f_jaw(hq)
    dP = f_pocket(hq)
    zone = (hq[:, 1] < -0.050) & (hq[:, 2] < -0.045)
    on_jaw = (np.abs(dJ) < 0.0012) & zone
    on_pocket = (np.abs(dP) < 0.0012) & (dU < -0.0004) & zone & ~on_jaw
    lip = np.zeros(len(hq), dtype=bool)
    gum = np.zeros(len(hq), dtype=bool)
    gum |= on_pocket & (dU < -LIP_INNER)
    lip |= on_pocket & (dU >= -LIP_INNER)
    lip |= zone & ~on_jaw & ~on_pocket & (np.abs(dU) < 0.0015) & (dP < LIP_BAND)
    gum |= on_jaw & (dU < -LIP_INNER)
    lip |= on_jaw & (dU >= -LIP_INNER) & (dU < 0.0022)
    # eyelid rims and the socket walls behind them
    for s in (1.0, -1.0):
        ds = _norm(hq - EYE_OFF * v3((s, 1, 1))) - F32(EYE_R + SOCKET_CLEAR)
        lip |= ds < EYE_RIM
    sub = np.zeros(len(hq), dtype=np.int32)
    sub[lip] = M_LIP
    sub[gum] = M_GUM
    sub[nose] = M_NOSE
    out[idx] = sub
    # paw pads: the undersides of the paws
    pad = (p[:, 2] < 0.0075) & (n[:, 2] < -0.35)
    out[pad] = M_PAD
    return out


# ----------------------------------------------------------------------------
# Narrow-band naive surface nets, slab by slab (bounded memory), symmetric in x
# ----------------------------------------------------------------------------

GRID_LO = (-0.17, -0.64, -0.006)
GRID_HI = (0.17, 0.60, 0.80)


def surface_nets(fn, voxel, lo=GRID_LO, hi=GRID_HI, block=8, slab=48, log=print):
    """Mesh the zero set of fn. Returns (verts (N,3) float64, quads (M,4) int64).

    The grid is symmetric about x = 0 and only x >= 0 is evaluated (mirrored), so
    the mesh is exactly mirror-symmetric. Only blocks the coarse pass finds near
    the surface are evaluated finely."""
    t0 = time.time()
    half = int(math.ceil(max(abs(lo[0]), abs(hi[0])) / voxel))
    nx = 2 * half + 1
    xs = ((np.arange(nx) - half) * voxel).astype(F32)
    ny = int(math.ceil((hi[1] - lo[1]) / voxel)) + 1
    nz = int(math.ceil((hi[2] - lo[2]) / voxel)) + 1
    ys = (lo[1] + np.arange(ny) * voxel).astype(F32)
    zs = (lo[2] + np.arange(nz) * voxel).astype(F32)
    # coarse pass over blocks of the x >= 0 half
    xh = xs[half:]
    nbx = int(math.ceil((len(xh) - 1) / block))
    nby = int(math.ceil((ny - 1) / block))
    nbz = int(math.ceil((nz - 1) / block))
    bc = np.stack(np.meshgrid((np.arange(nbx) + 0.5) * block * voxel,
                              lo[1] + (np.arange(nby) + 0.5) * block * voxel,
                              lo[2] + (np.arange(nbz) + 0.5) * block * voxel, indexing="ij"), axis=-1)
    bc = bc.reshape(-1, 3).astype(F32)
    dc = np.concatenate([fn(bc[i:i + 200000]) for i in range(0, len(bc), 200000)]).reshape(nbx, nby, nbz)
    reach = block * voxel * math.sqrt(3.0) * 0.5 * 1.6 + 3.0 * voxel
    near = np.abs(dc) < reach
    log(f"[nets] grid {nx}x{ny}x{nz} voxel {voxel*1000:.2f} mm, near blocks {near.sum()}/{near.size}")

    cells_lin = []
    cells_pos = []
    edges = []  # (axis, i, j, k, inside_at_lower)
    ncx, ncy, ncz = nx - 1, ny - 1, nz - 1
    n_eval = 0
    prev_last = None
    for j0 in range(0, ny - 1, slab):
        j1 = min(j0 + slab, ny - 1)  # samples j0..j1 inclusive
        jj = np.arange(j0, j1 + 1)
        # fine field on the x >= 0 half for these rows
        fh = np.empty((len(xh), len(jj), nz), dtype=F32)
        bj = np.minimum(jj // block, nby - 1)
        # sample (i, j, k) is near if any block touching it is near
        ib = np.minimum(np.arange(len(xh)) // block, nbx - 1)
        ib2 = np.clip((np.arange(len(xh)) - 1) // block, 0, nbx - 1)
        kb = np.minimum(np.arange(nz) // block, nbz - 1)
        kb2 = np.clip((np.arange(nz) - 1) // block, 0, nbz - 1)
        bj2 = np.clip((jj - 1) // block, 0, nby - 1)
        m = np.zeros(fh.shape, dtype=bool)
        sgn = np.empty(fh.shape, dtype=F32)
        for a in (ib, ib2):
            for b in (bj, bj2):
                for c in (kb, kb2):
                    m |= near[a][:, b][:, :, c]
        sgn[:] = np.sign(dc[ib][:, bj][:, :, kb])
        idx = np.argwhere(m)
        pts = np.stack([xh[idx[:, 0]], ys[jj[idx[:, 1]]], zs[idx[:, 2]]], axis=1)
        vals = np.concatenate([fn(pts[i:i + 250000]) for i in range(0, len(pts), 250000)]) if len(pts) else np.zeros(0, F32)
        n_eval += len(pts)
        fh[:] = sgn * F32(1.0)
        fh[idx[:, 0], idx[:, 1], idx[:, 2]] = vals
        f = np.concatenate([fh[:0:-1], fh], axis=0)  # mirror to the full x range
        inside = f < 0
        # active cells in rows j0..j1-1
        c_any = np.zeros((ncx, len(jj) - 1, ncz), dtype=bool)
        c_all = np.ones_like(c_any)
        for di in (0, 1):
            for dj in (0, 1):
                for dk in (0, 1):
                    s_ = inside[di:di + ncx, dj:dj + len(jj) - 1, dk:dk + ncz]
                    c_any |= s_
                    c_all &= s_
        act = np.argwhere(c_any & ~c_all)
        if len(act):
            ci, cj, ck = act[:, 0], act[:, 1], act[:, 2]
            corner = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0], [0, 0, 1], [1, 0, 1], [0, 1, 1], [1, 1, 1]])
            cv = np.stack([f[ci + o[0], cj + o[1], ck + o[2]] for o in corner], axis=1)
            acc = np.zeros((len(act), 3))
            cnt = np.zeros(len(act))
            for a, b in ((0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)):
                va, vb = cv[:, a].astype(np.float64), cv[:, b].astype(np.float64)
                mm = (va < 0) != (vb < 0)
                t = np.where(mm, va / np.where(mm, va - vb, 1.0), 0.0)
                pos = corner[a][None, :] + (corner[b] - corner[a])[None, :] * t[:, None]
                acc[mm] += pos[mm]
                cnt[mm] += 1
            off = acc / cnt[:, None]
            gj = cj + j0
            pos = np.stack([xs[ci].astype(np.float64) + off[:, 0] * voxel,
                            ys[gj].astype(np.float64) + off[:, 1] * voxel,
                            zs[ck].astype(np.float64) + off[:, 2] * voxel], axis=1)
            lin = (gj.astype(np.int64) * ncx + ci) * ncz + ck
            # exact mirror symmetry: cells with i >= ncx/2 copy their partner's position
            order = np.argsort(lin)
            lin, pos, ci = lin[order], pos[order], ci[order]
            hi_side = ci >= ncx // 2
            partner = (lin[hi_side] // ncz // ncx * ncx + (ncx - 1 - ci[hi_side])) * ncz + lin[hi_side] % ncz
            pidx = np.searchsorted(lin, partner)
            assert np.all(lin[pidx] == partner), "surface nets: asymmetric active cells"
            pos[hi_side] = pos[pidx] * np.array([-1.0, 1.0, 1.0])
            cells_lin.append(lin)
            cells_pos.append(pos)
        # sign-changing edges whose lower sample row is in j0..j1-1 (y edges) or j0..j1-1 (x/z edges)
        rows = len(jj) - 1
        ex = inside[:-1, :rows, :] != inside[1:, :rows, :]
        e = np.argwhere(ex)
        edges.append(np.column_stack([np.zeros(len(e), np.int64), e[:, 0], e[:, 1] + j0, e[:, 2], inside[e[:, 0], e[:, 1], e[:, 2]]]))
        ey = inside[:, :rows, :] != inside[:, 1:rows + 1, :]
        e = np.argwhere(ey)
        edges.append(np.column_stack([np.ones(len(e), np.int64), e[:, 0], e[:, 1] + j0, e[:, 2], inside[e[:, 0], e[:, 1], e[:, 2]]]))
        ez = inside[:, :rows, :-1] != inside[:, :rows, 1:]
        e = np.argwhere(ez)
        edges.append(np.column_stack([np.full(len(e), 2, np.int64), e[:, 0], e[:, 1] + j0, e[:, 2], inside[e[:, 0], e[:, 1], e[:, 2]]]))
    lin = np.concatenate(cells_lin)
    pos = np.concatenate(cells_pos)
    order = np.argsort(lin)
    lin, pos = lin[order], pos[order]
    E = np.concatenate(edges)
    ax, i, j, k, ins = E[:, 0], E[:, 1], E[:, 2], E[:, 3], E[:, 4].astype(bool)

    def cell(ci, cj, ck):
        ok = (ci >= 0) & (ci < ncx) & (cj >= 0) & (cj < ncy) & (ck >= 0) & (ck < ncz)
        l = (cj.astype(np.int64) * ncx + ci) * ncz + ck
        p_ = np.searchsorted(lin, l)
        p_ = np.minimum(p_, len(lin) - 1)
        ok &= lin[p_] == l
        return p_, ok

    quads = np.zeros((len(E), 4), dtype=np.int64)
    valid = np.ones(len(E), dtype=bool)
    for a in range(3):
        sel = ax == a
        ii, jj_, kk = i[sel], j[sel], k[sel]
        if a == 0:
            cs = [(ii, jj_ - 1, kk - 1), (ii, jj_, kk - 1), (ii, jj_, kk), (ii, jj_ - 1, kk)]
        elif a == 1:
            cs = [(ii - 1, jj_, kk - 1), (ii - 1, jj_, kk), (ii, jj_, kk), (ii, jj_, kk - 1)]
        else:
            cs = [(ii - 1, jj_ - 1, kk), (ii, jj_ - 1, kk), (ii, jj_, kk), (ii - 1, jj_, kk)]
        q = np.zeros((sel.sum(), 4), dtype=np.int64)
        okall = np.ones(sel.sum(), dtype=bool)
        for n, (a_, b_, c_) in enumerate(cs):
            q[:, n], ok = cell(a_, b_, c_)
            okall &= ok
        quads[sel] = q
        valid[sel] = okall
    # counter-clockwise seen from the outside when the lower sample is inside
    flip = ~ins
    quads[flip] = quads[flip][:, ::-1]
    quads = quads[valid]
    log(f"[nets] {len(pos)} verts, {len(quads)} quads, {n_eval} fine evaluations, {time.time() - t0:.1f}s")
    return pos, quads


# ----------------------------------------------------------------------------
# Mesh helpers
# ----------------------------------------------------------------------------


def taubin_smooth(verts, faces, iterations=4, lam=0.5, mu=-0.53, pinned=None):
    """Volume-preserving Laplacian smoothing on the face graph (numpy)."""
    v = verts.astype(np.float64).copy()
    f = np.asarray(faces)
    k = f.shape[1]
    a = f.ravel()
    b = np.roll(f, -1, axis=1).ravel()
    src = np.concatenate([a, b])
    dst = np.concatenate([b, a])
    key = np.unique(src.astype(np.int64) * len(v) + dst)
    src, dst = key // len(v), key % len(v)
    deg = np.bincount(src, minlength=len(v)).astype(np.float64)
    deg[deg == 0] = 1.0
    for _ in range(iterations):
        for w in (lam, mu):
            acc = np.zeros_like(v)
            np.add.at(acc, src, v[dst])
            d = acc / deg[:, None] - v
            if pinned is not None:
                d[pinned] = 0.0
            v += w * d
    return v


def bl_mesh(name, verts, faces):
    """A Blender mesh from numpy arrays (faces: (M, k) int array, all k-gons)."""
    import bpy

    me = bpy.data.meshes.new(name)
    faces = np.asarray(faces, dtype=np.int32)
    nv, nf, k = len(verts), len(faces), faces.shape[1]
    me.vertices.add(nv)
    me.vertices.foreach_set("co", np.asarray(verts, dtype=np.float32).ravel())
    me.loops.add(nf * k)
    me.loops.foreach_set("vertex_index", faces.ravel())
    me.polygons.add(nf)
    me.polygons.foreach_set("loop_start", np.arange(0, nf * k, k, dtype=np.int32))
    me.update(calc_edges=True)
    me.validate(clean_customdata=False)
    return me


def sdf_gradient(fn, p, h=2.5e-4):
    """Central-difference gradient of fn at the (N, 3) points p."""
    p = np.asarray(p, dtype=F32)
    g = np.zeros_like(p)
    for a in range(3):
        e = np.zeros(3, dtype=F32)
        e[a] = h
        g[:, a] = (fn(p + e) - fn(p - e)) / F32(2.0 * h)
    return g


def project_to_surface(fn, verts, iterations=3, max_step=0.0015, chunk=200000):
    """Newton steps onto the zero set of fn (moves each vertex at most max_step per iteration)."""
    v = np.asarray(verts, dtype=np.float64).copy()
    for _ in range(iterations):
        for i in range(0, len(v), chunk):
            p = v[i:i + chunk].astype(F32)
            d = fn(p).astype(np.float64)
            g = sdf_gradient(fn, p).astype(np.float64)
            g2 = np.maximum(np.einsum("ij,ij->i", g, g), 1e-8)
            step = -(d / g2)[:, None] * g
            n = np.linalg.norm(step, axis=1)
            scale = np.minimum(1.0, max_step / np.maximum(n, 1e-12))
            v[i:i + chunk] += step * scale[:, None]
    return v


def split_nonmanifold(verts, faces, fn):
    """Make a surface-nets mesh manifold.

    Ambiguous (saddle) grid faces make mesh edges shared by four faces. Around each such edge
    the four faces are sorted by angle; the pairs bounding the air wedges are joined when the
    SDF `fn` says the solid is connected through the edge, else the pairs bounding the solid
    wedges. Every vertex whose incident faces then form more than one fan gets one copy per fan.
    Returns (verts, faces, number_of_new_vertices)."""
    verts = np.asarray(verts, dtype=np.float64)
    faces = np.array(faces, dtype=np.int64)
    k = faces.shape[1]
    nv = len(verts)
    fi = np.repeat(np.arange(len(faces)), k)
    a = faces.ravel()
    b = np.roll(faces, -1, axis=1).ravel()
    key = np.minimum(a, b) * nv + np.maximum(a, b)
    order_k = np.argsort(key, kind="stable")
    ks = key[order_k]
    uk, first, cnt = np.unique(ks, return_index=True, return_counts=True)
    bad = np.nonzero(cnt != 2)[0]
    if len(bad) == 0:
        return verts, faces, 0
    manifold_edge = set(uk[cnt == 2].tolist())
    links = {}  # edge key -> list of (face, face) joined across it
    fverts = verts[faces]
    fcent = fverts.mean(axis=1)
    fnorm = np.zeros((len(faces), 3))
    for j in range(k):
        p0 = fverts[:, j]
        p1 = fverts[:, (j + 1) % k]
        fnorm += np.cross(p0, p1)
    for bi in bad.tolist():
        ek = int(uk[bi])
        fs = np.unique(fi[order_k[first[bi]:first[bi] + cnt[bi]]])
        nf = len(fs)
        if nf < 4 or nf % 2:
            continue
        va, vb = verts[ek // nv], verts[ek % nv]
        t = vb - va
        t /= max(np.linalg.norm(t), 1e-12)
        mid = 0.5 * (va + vb)
        u = fcent[fs] - mid
        u -= np.outer(u @ t, t)
        u /= np.maximum(np.linalg.norm(u, axis=1), 1e-12)[:, None]
        e1 = u[0]
        e2 = np.cross(t, e1)
        ang = np.arctan2(u @ e2, u @ e1)
        srt = np.argsort(ang)
        fs, u = fs[srt], u[srt]
        air = [float(fnorm[fs[i]] @ np.cross(t, u[i])) > 0.0 for i in range(nf)]
        want_air = float(fn(mid[None, :].astype(F32))[0]) < 0.0
        pairs = [(int(fs[i]), int(fs[(i + 1) % nf])) for i in range(nf) if air[i] == want_air]
        if len(pairs) == nf // 2:
            links[ek] = pairs
    cand = np.unique(np.concatenate([uk[bad] // nv, uk[bad] % nv]))
    order = np.argsort(a, kind="stable")
    a_sorted = a[order]
    starts = np.searchsorted(a_sorted, cand)
    ends = np.searchsorted(a_sorted, cand, side="right")
    extra = []
    for v, s0, s1 in zip(cand.tolist(), starts.tolist(), ends.tolist()):
        inc = np.unique(fi[order[s0:s1]]).tolist()
        parent = {f: f for f in inc}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        by_edge = {}
        for f in inc:
            row = faces[f].tolist()
            j = row.index(v)
            for w in (row[(j + 1) % k], row[(j - 1) % k]):
                ek = min(v, w) * nv + max(v, w)
                if ek in manifold_edge:
                    by_edge.setdefault(ek, set()).add(f)
                elif ek in links:
                    for f0, f1 in links[ek]:
                        if f0 in parent and f1 in parent:
                            parent[find(f1)] = find(f0)
        for fs in by_edge.values():
            fs = sorted(fs)
            for f in fs[1:]:
                parent[find(f)] = find(fs[0])
        comps = {}
        for f in inc:
            comps.setdefault(find(f), []).append(f)
        if len(comps) < 2:
            continue
        for g in sorted(comps.values(), key=min)[1:]:
            nid = nv + len(extra)
            extra.append(verts[v])
            for f in g:
                row = faces[f]
                row[row == v] = nid
    if extra:
        verts = np.concatenate([verts, np.asarray(extra)])
    return verts, faces, len(extra)


# ----------------------------------------------------------------------------
# The body mesh: surface nets -> manifold repair -> size-field decimation -> refine -> relax
# ----------------------------------------------------------------------------

BODY_VOXEL = 0.0011
SIZE_GRADE = 0.22  # how fast the target edge length may grow with distance from a detail feature


def size_field(p):
    """Target edge length (m) at surface points p.

    Fine where the bare skin is drawn at a close-up (nose leather, lips, eyelid rims, the face);
    coarse where long fur covers a smooth surface (torso)."""
    p = np.asarray(p, dtype=F32)
    q = hrel(p)
    R = region_fields(p)
    names = list(R.keys())
    lab = np.array(names)[np.argmin(np.stack([R[n] for n in names], axis=1), axis=1)]
    h = np.full(len(p), 0.0100, dtype=F32)
    h[lab == "neck"] = 0.0060
    h[np.isin(lab, ["fl", "fr", "hl", "hr"])] = 0.0070
    h[np.isin(lab, ["ear_l", "ear_r"])] = 0.0032
    h[lab == "tail"] = 0.0044
    h[lab == "head"] = 0.0040
    h = np.minimum(h, np.where(p[:, 2] < 0.15, 0.0050, 1.0)).astype(F32)  # lower legs
    h = np.minimum(h, np.where(p[:, 2] < 0.07, 0.0032, 1.0)).astype(F32)  # paws, toes, pads
    near = np.all((p >= HEAD_LO) & (p <= HEAD_HI), axis=1)
    if near.any():
        hq = q[near]
        hh = h[near]
        g = F32(SIZE_GRADE)
        face = np.clip((-0.055 - hq[:, 1]) / 0.03, 0.0, 1.0)
        hh = np.minimum(hh, F32(0.0040) - F32(0.0017) * face)
        hh = np.minimum(hh, F32(0.00075) + g * np.maximum(f_nose(hq), 0.0))
        u = f_upper_head(hq)
        hh = np.minimum(hh, F32(0.0010) + g * (np.abs(f_pocket(hq)) + np.abs(u)))
        for s in (1.0, -1.0):
            de = np.abs(_norm(hq - EYE_OFF * v3((s, 1, 1))) - F32(EYE_R + SOCKET_CLEAR))
            hh = np.minimum(hh, F32(0.0010) + g * (de + np.maximum(-u, 0.0)))
        h[near] = hh
    return h


def _nonmanifold_count(me):
    import bmesh

    bm = bmesh.new()
    bm.from_mesh(me)
    n = (sum(1 for e in bm.edges if not e.is_manifold), sum(1 for v in bm.verts if not v.is_manifold))
    bm.free()
    return n


def _mesh_arrays(me):
    co = np.zeros(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    me.calc_loop_triangles()
    tri = np.zeros(len(me.loop_triangles) * 3, dtype=np.int32)
    me.loop_triangles.foreach_get("vertices", tri)
    return co.reshape(-1, 3).astype(np.float64), tri.reshape(-1, 3)


def _edges_of(tri):
    e = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    return np.unique(np.sort(e, axis=1), axis=0)


def relax_tangential(fn, co, tri, iterations=3, step=0.45, pinned=None):
    """Move vertices toward their neighbours' centroid within the tangent plane, then back onto the surface."""
    e = _edges_of(tri)
    src = np.concatenate([e[:, 0], e[:, 1]])
    dst = np.concatenate([e[:, 1], e[:, 0]])
    deg = np.bincount(src, minlength=len(co)).astype(np.float64)
    for _ in range(iterations):
        acc = np.zeros_like(co)
        np.add.at(acc, src, co[dst])
        d = acc / np.maximum(deg, 1.0)[:, None] - co
        n = sdf_gradient(fn, co.astype(F32)).astype(np.float64)
        n /= np.maximum(np.linalg.norm(n, axis=1), 1e-9)[:, None]
        d -= np.einsum("ij,ij->i", d, n)[:, None] * n
        if pinned is not None:
            d[pinned] = 0.0
        co = co + step * d
        co = project_to_surface(fn, co, iterations=2)
    return co


def refine_long_edges(co, tri, fn, size_fn, passes=2, ratio=1.33, log=print):
    """Red-green refinement: split every edge longer than ratio x its target size at its midpoint
    (one shared vertex per edge, projected onto the surface) and re-triangulate each triangle by
    how many of its edges split. Keeps the mesh manifold and the winding consistent."""
    co = np.asarray(co, dtype=np.float64)
    tri = np.asarray(tri, dtype=np.int64)
    for it in range(passes):
        nv = len(co)
        sides = np.stack([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]], axis=1)  # (T, 3, 2)
        keys = np.minimum(sides[..., 0], sides[..., 1]) * nv + np.maximum(sides[..., 0], sides[..., 1])
        uk, inv = np.unique(keys.ravel(), return_inverse=True)
        inv = inv.reshape(-1, 3)
        ea, eb = uk // nv, uk % nv
        mid = 0.5 * (co[ea] + co[eb])
        length = np.linalg.norm(co[ea] - co[eb], axis=1)
        split = length > ratio * size_fn(mid.astype(F32))
        if not split.any():
            break
        new_id = np.full(len(uk), -1, dtype=np.int64)
        new_id[split] = nv + np.arange(split.sum())
        newpos = project_to_surface(fn, mid[split], iterations=3)
        co = np.concatenate([co, newpos])
        s = split[inv]  # (T, 3) which sides split
        m = new_id[inv]  # midpoint ids per side
        cnt = s.sum(axis=1)
        out = [tri[cnt == 0]]
        # rotate each triangle so its split pattern is canonical: side 0 = (v0, v1), side 1 = (v1, v2), side 2 = (v2, v0)
        for n in (1, 2, 3):
            sel = np.nonzero(cnt == n)[0]
            if len(sel) == 0:
                continue
            t = tri[sel]
            ss = s[sel]
            mm = m[sel]
            if n == 1:
                r = np.argmax(ss, axis=1)  # the split side becomes side 0
            elif n == 2:
                r = (np.argmin(ss, axis=1) + 1) % 3  # the unsplit side becomes side 2
            else:
                r = np.zeros(len(sel), dtype=np.int64)
            idx = (np.arange(3)[None, :] + r[:, None]) % 3
            v = np.take_along_axis(t, idx, axis=1)
            mp = np.take_along_axis(mm, idx, axis=1)
            a, b, c = v[:, 0], v[:, 1], v[:, 2]
            m0, m1, m2 = mp[:, 0], mp[:, 1], mp[:, 2]
            if n == 1:
                out += [np.stack([a, m0, c], 1), np.stack([m0, b, c], 1)]
            elif n == 2:
                # sides 0 (a-b) and 1 (b-c) split: corner triangle at b, then the quad a-m0-m1-c by its shorter diagonal
                out.append(np.stack([m0, b, m1], 1))
                d1 = np.linalg.norm(co[a] - co[m1], axis=1)
                d2 = np.linalg.norm(co[m0] - co[c], axis=1)
                use1 = d1 <= d2
                out.append(np.where(use1[:, None], np.stack([a, m0, m1], 1), np.stack([a, m0, c], 1)))
                out.append(np.where(use1[:, None], np.stack([a, m1, c], 1), np.stack([m0, m1, c], 1)))
            else:
                out += [np.stack([a, m0, m2], 1), np.stack([m0, b, m1], 1), np.stack([m2, m1, c], 1), np.stack([m0, m1, m2], 1)]
        tri = np.concatenate(out)
        log(f"[mesh] refine pass {it + 1}: split {int(split.sum())} edges -> {len(tri)} triangles")
    return co, tri


def mirror_exact(co, tol=5e-5):
    """Snap a (topologically) mirror-symmetric vertex set to exact symmetry about x = 0."""
    from mathutils import kdtree

    co = np.asarray(co, dtype=np.float64).copy()
    kd = kdtree.KDTree(len(co))
    for i, c in enumerate(co):
        kd.insert(c, i)
    kd.balance()
    done = np.zeros(len(co), dtype=bool)
    for i, c in enumerate(co):
        if done[i]:
            continue
        if abs(c[0]) < 1e-7:
            co[i, 0] = 0.0
            done[i] = True
            continue
        _, j, dist = kd.find((-c[0], c[1], c[2]))
        if j is not None and dist < tol and not done[j]:
            avg = 0.5 * (c + co[j] * np.array([-1.0, 1.0, 1.0]))
            co[i] = avg
            co[j] = avg * np.array([-1.0, 1.0, 1.0])
            done[i] = done[j] = True
    return co


def build_body_mesh(log=print, voxel=BODY_VOXEL, weight_exp=1.0, factor=10.0):
    """The skinned body surface as a Blender object (triangles, manifold, closed, mirror-symmetric)."""
    import bmesh
    import bpy

    verts, quads = surface_nets(dog_sdf, voxel, log=log)
    verts, quads, nsplit = split_nonmanifold(verts, quads, dog_sdf)
    log(f"[mesh] manifold repair split {nsplit} vertices")
    verts = taubin_smooth(verts, quads, iterations=2)
    # expected triangle count at the target sizes (never finer than the grid can carry)
    h = size_field(verts.astype(F32))
    hd = np.maximum(h, 1.25 * voxel)
    qa = verts[quads]
    qarea = 0.5 * np.linalg.norm(np.cross(qa[:, 2] - qa[:, 0], qa[:, 3] - qa[:, 1]), axis=1)
    hq = hd[quads].mean(axis=1)
    target = int(np.sum(qarea / (0.433 * hq * hq)))
    me = bl_mesh("Dog", verts, quads)
    ob = bpy.data.objects.new("Dog", me)
    bpy.context.scene.collection.objects.link(ob)
    c = 0.97 * float(hd.min()) ** weight_exp
    w = np.clip(1.0 - c / np.power(hd.astype(np.float64), weight_exp), 0.0, 0.999)
    vg = ob.vertex_groups.new(name="decimate")
    wq = np.round(w, 3)
    for val in np.unique(wq):
        vg.add(np.nonzero(wq == val)[0].tolist(), float(max(val, 0.001)), "REPLACE")
    mod = ob.modifiers.new("Decimate", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = min(1.0, target / (2.0 * len(quads)))
    mod.use_symmetry = False  # the symmetric collapse glues zero-volume fins onto the mirror plane
    mod.use_collapse_triangulate = True
    mod.vertex_group = "decimate"
    mod.vertex_group_factor = factor
    with bpy.context.temp_override(object=ob, active_object=ob):
        bpy.ops.object.modifier_apply(modifier="Decimate")
    ob.vertex_groups.clear()
    log(f"[mesh] decimated to {len(me.polygons)} triangles (size-field target {target})")
    # refine below the grid where the bare skin needs it, flip edges for well-shaped triangles, relax
    co, tri = _mesh_arrays(me)
    # the collapse can pinch two sheets onto one vertex pair (a duplicate edge): split them apart
    co, tri, nsplit = split_nonmanifold(co, tri, dog_sdf)
    log(f"[mesh] post-decimation repair split {nsplit} vertices")
    co, tri = refine_long_edges(co, tri, dog_sdf, size_field, passes=2, log=log)
    old = ob.data
    me = bl_mesh("Dog", co, tri)
    ob.data = me
    bpy.data.meshes.remove(old)
    log(f"[mesh] manifold after refine: {_nonmanifold_count(me)}")
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.beautify_fill(bm, faces=bm.faces[:], edges=bm.edges[:], use_restrict_tag=False, method="ANGLE")
    bm.to_mesh(me)
    bm.free()
    log(f"[mesh] manifold after beautify: {_nonmanifold_count(me)}")
    co, tri = _mesh_arrays(me)
    co = project_to_surface(dog_sdf, co, iterations=3)
    co = relax_tangential(dog_sdf, co, tri, iterations=3)
    me.vertices.foreach_set("co", co.astype(np.float32).ravel())
    me.update()
    log(f"[mesh] manifold after relax: {_nonmanifold_count(me)}")
    # exact mirror symmetry: keep the +X half, mirror it, weld the centre line
    with bpy.context.temp_override(object=ob, active_object=ob, edit_object=ob):
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.symmetrize(direction="POSITIVE_X", threshold=1e-5)
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.quads_convert_to_tris(quad_method="BEAUTY", ngon_method="BEAUTY")
        bpy.ops.object.mode_set(mode="OBJECT")
    log(f"[mesh] manifold after symmetrize: {_nonmanifold_count(me)}")
    co, tri = _mesh_arrays(me)
    co = project_to_surface(dog_sdf, co, iterations=2)
    co = relax_tangential(dog_sdf, co, tri, iterations=2, step=0.35)
    co, tri = cut_material_boundaries(co, tri, log=log)
    co = mirror_exact(co)
    old = ob.data
    me = bl_mesh("Dog", co, tri)
    ob.data = me
    bpy.data.meshes.remove(old)
    log(f"[mesh] manifold after contour cuts: {_nonmanifold_count(me)}")
    log(f"[mesh] body: {len(me.polygons)} triangles, {len(me.vertices)} vertices")
    return ob


# ----------------------------------------------------------------------------
# Eyelid shells, tongue and teeth (separate pieces of the one skinned mesh)
# ----------------------------------------------------------------------------

LID_UPPER_OPEN = 34.0  # degrees above the gaze axis: where the upper margin rests (relaxed: covers the white)
LID_LOWER_OPEN = -22.0  # degrees: where the lower margin rests -- at the cheek's edge, which closes over the globe
# 17 to 23 degrees below the gaze; at -40 the lower lid sat hidden under the cheek and the eye had no lower lid line
LID_MEET = -12.0  # degrees: where the margins meet in a blink (slightly below centre)
LID_SPAN = 100.0  # how far each shell reaches back from its margin (degrees)
LID_INNER_SPAN = 42.0
# Half-width of each shell about the lid axis (degrees): to within a degree of the globe's side poles.
# At 84 each pole kept a hole ~3 mm across, and a three-quarter view looked through the one at the
# outer corner into the socket: a dark notch with the white of the globe in it.
LID_PSI = 89.0
# Half-widths of the eye OPENING (degrees about the gaze): the canthi. A dog's opening is narrower
# than its globe; with the corners at LID_PSI the whole side of the globe showed between the lids,
# and every three-quarter view had a white triangle behind the iris. Past the corners the margins
# cross by LID_CLOSED_OVERLAP, so the shells overlap into closed, furred skin. The inner corner is
# the wider one because the bridge of the nose covers it: at 58 each side, the visible opening sat
# toward the temple and the irises looked cross-eyed in a white-cornered almond.
LID_FISSURE_INNER = 58.0
LID_FISSURE_OUTER = 46.0
LID_CLOSED_OVERLAP = 6.0
LID_SEGMENTS = 72
LID_RIM_BAND = 0.0007  # dark rim band on the outer surface next to the margin (m): the pigmented edge that frames the eye
LID_CANTHUS = -4.0  # degrees: where the two margins meet at the eye's corners
LID_OUTER_DROOP = 6.0  # degrees the outer corners sit lower than the inner ones (the soft, puppy-dog set)
LID_STACK = 0.00035  # the upper shell sits this much outside the lower, so their corners slide over each other in a blink


def eye_frame(side):
    """(centre, forward, up, axis) of one eye in Blender coordinates. The lids rotate about `axis`."""
    c = eye_centre(side).astype(np.float64)
    f = eye_gaze(side).astype(np.float64)
    up = np.array((0.0, 0.0, 1.0)) - f[2] * f
    up /= np.linalg.norm(up)
    axis = np.cross(up, f)
    axis /= np.linalg.norm(axis)
    return c, f, up, axis


def lid_fissure(psi, side):
    """The opening's half-width (radians) on psi's side of the gaze: +psi is the dog's left, so it
    is the outer side for the left eye (side = +1) and the inner side for the right."""
    return math.radians(LID_FISSURE_OUTER if psi * side > 0.0 else LID_FISSURE_INNER)


def lid_margin(psi, upper, side):
    """Where the margin rests (degrees above the gaze axis) at angle psi (radians) around the lid
    axis: highest mid-eye, falling to LID_CANTHUS at the corners, an almond rather than a circle.
    +psi is toward the dog's left (+X), so the OUTER corner is +psi for the left eye. Past the
    corners (lid_fissure) the two margins cross, the upper one below the lower one."""
    a = abs(psi) / lid_fissure(psi, side)
    t = min(a, 1.0)
    if upper:
        shape = (1.0 - t ** 2.2) ** 0.8  # the upper arc is fuller: it rises quickly from the corners
        top = LID_UPPER_OPEN
    else:
        shape = 1.0 - t ** 2.0
        top = LID_LOWER_OPEN
    outer = max(-1.0, min(1.0, psi / lid_fissure(psi, side))) * side  # +1 at the outer corner, -1 at the inner
    closed = float(_smoothstep(0.0, 0.14, a - 1.0))  # 0 inside the opening, 1 a few degrees past the corner
    cross = LID_CLOSED_OVERLAP * closed * (-1.0 if upper else 1.0)
    return LID_CANTHUS + (top - LID_CANTHUS) * shape - LID_OUTER_DROOP * 0.5 * outer + cross


def lid_shell(side, upper):
    """One eyelid as a single grid strip: outer back edge -> outer surface -> rounded margin ->
    inner surface -> inner back edge. Every point keeps a constant distance from the eye centre
    under a rotation about the lid axis, so a blink can never cut into the eyeball. The margin is
    an almond (lid_margin) and the upper shell sits LID_STACK outside the lower one.
    Returns verts, quads, per-quad material, per-vertex uv."""
    c, f, up, axis = eye_frame(side)
    ri = EYE_R + LID_GAP + (LID_STACK if upper else 0.0)
    ro = ri + LID_THICK
    rm = 0.5 * (ri + ro)
    half_t = 0.5 * LID_THICK
    sgn = 1.0 if upper else -1.0  # the shell reaches back toward +phi (upper) or -phi (lower)
    # The profile as OFFSETS from the margin (radians, radius), fine near the visible margin; the
    # margin itself moves with psi.
    prof = []
    n_out = 22
    for i in range(n_out):
        u = (1.0 - i / (n_out - 1)) ** 2.2  # 1 at the back edge -> 0 at the margin
        prof.append((sgn * math.radians(LID_SPAN) * u, ro))
    for j in range(1, 8):
        th = math.pi * j / 8.0
        prof.append((-sgn * (half_t / rm) * math.sin(th), rm + half_t * math.cos(th)))
    n_in = 10
    for i in range(n_in):
        u = (i / (n_in - 1)) ** 1.6
        prof.append((sgn * math.radians(LID_INNER_SPAN) * u, ri))
    psis = np.radians(np.linspace(-LID_PSI, LID_PSI, LID_SEGMENTS + 1))
    margins = [math.radians(lid_margin(ps, upper, side)) for ps in psis]
    verts = []
    uvs = []
    # arc length along the profile (for v) and across (for u), at the centre of the shell
    arc = [0.0]
    for (p0, r0), (p1, r1) in zip(prof[:-1], prof[1:]):
        arc.append(arc[-1] + math.hypot(r1 * math.cos(p1) - r0 * math.cos(p0), r1 * math.sin(p1) - r0 * math.sin(p0)))
    for k, (dph, rr) in enumerate(prof):
        for ps, m0 in zip(psis, margins):
            ph = m0 + dph
            d = math.cos(ps) * (math.cos(ph) * f + math.sin(ph) * up) + math.sin(ps) * axis
            verts.append(c + rr * d)
            uvs.append((ro * ps, arc[k]))
    verts = np.array(verts)
    uvs = np.array(uvs)
    W = LID_SEGMENTS + 1
    quads = []
    mats = []
    # A blink turns the shell about the lid axis, which moves each point's phi and nothing else: the
    # phis a point sweeps, from rest to closed.
    close = math.radians(LID_CLOSE_UPPER if upper else LID_CLOSE_LOWER) * (-1.0 if upper else 1.0)
    sweep = np.linspace(0.0, 1.0, 9) * close
    for k in range(len(prof) - 1):
        # outer surface far from the margin is lid skin; the margin band, rim and inner side are dark
        on_outer = k < n_out - 1
        dist = abs(0.5 * (prof[k][0] + prof[k + 1][0])) * ro
        mat = M_LID if (on_outer and dist > LID_RIM_BAND) else M_LIP
        for j in range(LID_SEGMENTS):
            a, b = k * W + j, k * W + j + 1
            quads.append((a, b, b + W, a + W))
            # Past the corners the upper shell's rim and margin lie over the closed lower one: skin,
            # furred like the rest of the lid, or the eyeliner runs on past the eye as a dark crack.
            mid = 0.5 * (psis[j] + psis[j + 1])
            closed = abs(mid) > lid_fissure(mid, side)
            m = M_LID if (upper and closed and k < n_out + 7) else mat
            if m == M_LID:
                ph = 0.5 * (margins[j] + margins[j + 1] + prof[k][0] + prof[k + 1][0]) + sweep
                dirs = (np.cos(mid) * (np.cos(ph)[:, None] * f + np.sin(ph)[:, None] * up) + np.sin(mid) * axis)
                theta, opening = orbit_angles(dirs, side)
                if np.all(theta - opening > math.radians(ORBIT_LID_MARGIN)):
                    m = M_LIP  # under the skin at rest and through the whole blink
            mats.append(m)
    quads = np.array(quads)
    # outward winding: the outer surface's normal must point away from the eye centre
    q0 = quads[0]
    n = np.cross(verts[q0[1]] - verts[q0[0]], verts[q0[3]] - verts[q0[0]])
    if np.dot(n, verts[q0[0]] - c) < 0:
        quads = quads[:, ::-1]
    return verts, quads, np.array(mats), uvs


TONGUE_BONE_Y = (-0.060, -0.100, -0.134, -0.168)  # tongue_01 head .. tongue_03 tail (head frame y)
TONGUE_Z = -0.0785


def f_tongue(q):
    """A wide, flat tongue with a rounded tip and a groove down the middle, lying in the tongue bed."""
    d = sd_ellipsoid(q, (0.0, -0.082, TONGUE_Z), (0.0185, 0.030, 0.0046))
    d = smin(d, sd_ellipsoid(q, (0.0, -0.120, TONGUE_Z + 0.0003), (0.0200, 0.034, 0.0042)), 0.012)
    d = smin(d, sd_ellipsoid(q, (0.0, -0.151, TONGUE_Z + 0.0006), (0.0180, 0.019, 0.0036)), 0.010)
    groove = sd_capsule(q, (0.0, -0.070, TONGUE_Z + 0.0052), (0.0, -0.160, TONGUE_Z + 0.0044), 0.0017)
    return ssub(d, groove, 0.0022)


def build_tongue_mesh(log=print):
    """Tongue triangles (head frame -> world), meshed from its SDF and decimated to a clean ~6k tris."""
    import bpy

    fn = lambda p: f_tongue(p - HEAD)  # noqa: E731
    lo = tuple((HEAD + v3((-0.03, -0.18, -0.09))).tolist())
    hi = tuple((HEAD + v3((0.03, -0.045, -0.066))).tolist())
    verts, quads = surface_nets(fn, 0.0006, lo=lo, hi=hi, block=6, slab=64, log=log)
    verts, quads, _ = split_nonmanifold(verts, quads, fn)
    verts = taubin_smooth(verts, quads, iterations=2)
    me = bl_mesh("Tongue", verts, quads)
    ob = bpy.data.objects.new("Tongue", me)
    bpy.context.scene.collection.objects.link(ob)
    mod = ob.modifiers.new("Decimate", "DECIMATE")
    mod.ratio = 6000.0 / (2.0 * len(quads))
    mod.use_collapse_triangulate = True
    with bpy.context.temp_override(object=ob, active_object=ob):
        bpy.ops.object.modifier_apply(modifier="Decimate")
    co, tri = _mesh_arrays(me)
    co = project_to_surface(fn, co, iterations=3)
    co = relax_tangential(fn, co, tri, iterations=3)
    bpy.data.objects.remove(ob)
    bpy.data.meshes.remove(me)
    return co, tri


# teeth: (base point in the head frame, direction, length, base radii (mesio-distal, labio-lingual), jaw)
#
# TWO SIZES, and the split is about the coat, not the teeth (#1533). The body's UVs are one
# smart projection over the whole mesh, teeth included, and the groom keys its colour map and clump
# cells on the pelt's root UVs: an island that changes shape repacks the pelt and regrows every clump.
# So the teeth enter the projection at their FIRST size (final=False), and reshape_teeth puts them at
# the size the face shows after the coat map is baked, with the same topology; only their own
# material samples them from there on. The first size was a stylised half-size row: the lower
# incisors sank in the jaw and the canines stayed behind the flews with the mouth open. A golden
# retriever's upper canine crown is 15-20 mm; 16 mm keeps its tip in the lip slit with the mouth
# shut (22 mm stood out under the flews like a fang, measured against f_head) and shows the crown
# when it opens. The lower incisors stand 3-4 mm clear of the gum, the lower canines 10 mm.
def _teeth_layout(final=False):
    teeth = []
    # upper incisors: an arc behind the upper lip, hanging down
    for x in (-0.0100, -0.0060, -0.0020, 0.0020, 0.0060, 0.0100):
        y = -0.1790 + 0.022 * x * x / 0.011
        length = 0.0042 if abs(x) < 0.008 else 0.0047
        if final:
            length += 0.0008
        teeth.append(((x, y, -0.0712), (0.0, -0.12, -1.0), length, (0.0019, 0.0012), "upper"))
    for s in (1.0, -1.0):  # upper canines, in the gap outside the lower jaw
        length, radius = (0.0160, 0.0030) if final else (0.0080, 0.0021)
        teeth.append(((s * 0.0245, -0.1600, -0.0705), (s * 0.05, 0.10, -1.0), length, (radius, radius), "upper"))
    # lower incisors on the front of the lower jaw, pointing up
    for x in (-0.0084, -0.0050, -0.0016, 0.0016, 0.0050, 0.0084):
        y = -0.1725 + 0.020 * x * x / 0.009
        z, length = (-0.0773, 0.0055) if final else (-0.0795, 0.0037)
        teeth.append(((x, y, z), (0.0, -0.18, 1.0), length, (0.0016, 0.0011), "lower"))
    for s in (1.0, -1.0):  # lower canines
        length, radius = (0.0100, 0.0024) if final else (0.0070, 0.0019)
        teeth.append(((s * 0.0170, -0.1650, -0.0800), (s * 0.06, -0.10, 1.0), length, (radius, radius), "lower"))
    return teeth


def build_teeth(final=False):
    """Stylised teeth as open-based tapered tubes (disk topology: one UV island each).
    Returns list of (verts, tris, jaw) in world coordinates. The same topology at either size."""
    out = []
    seg, rings = 14, 9
    for base, d, length, (ra, rb), jaw in _teeth_layout(final):
        b = np.array(base, dtype=np.float64) + HEAD
        d = np.array(d, dtype=np.float64)
        d /= np.linalg.norm(d)
        side = np.cross(d, (0.0, 1.0, 0.0))
        if np.linalg.norm(side) < 1e-6:
            side = np.array((1.0, 0.0, 0.0))
        side /= np.linalg.norm(side)
        fwd = np.cross(side, d)
        verts = []
        pointed = ra == rb
        for k in range(rings):
            t = 0.93 * k / (rings - 1)
            # canines taper to a point; incisors stay broad and round off into a blunt edge
            prof = (1.0 - t / 0.97) ** 0.8 if pointed else math.sqrt(max(0.0, 1.0 - (t / 0.97) ** 3))
            curve = -0.12 * length * t * t  # a slight backward curve
            ctr = b + d * (length * t) + fwd * curve
            for j in range(seg):
                a = 2.0 * math.pi * j / seg
                verts.append(ctr + prof * (ra * math.cos(a) * side + rb * math.sin(a) * fwd))
        tip = b + d * (length * 1.02) + fwd * (-0.12 * length)
        verts.append(tip)
        verts = np.array(verts)
        tris = []
        for k in range(rings - 1):
            for j in range(seg):
                a0, a1 = k * seg + j, k * seg + (j + 1) % seg
                b0, b1 = a0 + seg, a1 + seg
                tris += [(a0, a1, b1), (a0, b1, b0)]
        tip_i = len(verts) - 1
        last = (rings - 1) * seg
        for j in range(seg):
            tris.append((last + j, last + (j + 1) % seg, tip_i))
        tris = np.array(tris)
        # outward winding
        cen = verts.mean(axis=0)
        t0 = tris[len(tris) // 2]
        n = np.cross(verts[t0[1]] - verts[t0[0]], verts[t0[2]] - verts[t0[0]])
        if np.dot(n, verts[t0[0]] - (b + d * length * 0.5)) < 0:
            tris = tris[:, ::-1]
        out.append((verts, tris, jaw))
    return out


def reshape_teeth(ob, log=print):
    """The teeth at the size the face shows (see _teeth_layout), AFTER the coat map is baked: the
    same topology as the size they entered the UV projection at, so this moves vertices and nothing
    else. They are the mesh's last vertices (assemble appends them after the tongue)."""
    me = ob.data
    final = np.concatenate([v for v, _t, _j in build_teeth(final=True)])
    first = np.concatenate([v for v, _t, _j in build_teeth(final=False)])
    total = len(me.vertices)
    co = np.zeros(total * 3, dtype=np.float64)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    n = len(final)
    if not np.allclose(co[total - n:], first, atol=1e-5):
        raise RuntimeError("reshape_teeth: the mesh's last vertices are not the teeth assemble appended")
    co[total - n:] = final
    me.vertices.foreach_set("co", co.ravel())
    me.update()
    log(f"[teeth] resized after the coat bake: {n} vertices, the same topology")


# ----------------------------------------------------------------------------
# Material boundaries as exact iso-contours (a close-up shows a per-face boundary as a sawtooth)
# ----------------------------------------------------------------------------

LIP_LOWER_OUT = 0.0022  # the lower lip's dark band reaches this far outside the upper head's surface

PAD_SHAPES = []  # (centre, radii) of the paw pads, filled below


def _pad_shapes():
    out = []
    for s in (1.0, -1.0):
        for cy, big, toes in ((-0.198, 0.0, -0.228), (0.180, 0.0, 0.152)):
            x0 = s * (0.079 if cy < 0 else 0.080)
            out.append(((x0, cy + 0.008, 0.0015), (0.020, 0.015, 0.0085)))  # metacarpal / metatarsal pad
            for tx in (-0.019, -0.0065, 0.0065, 0.019):
                ty = toes + 0.11 * tx * tx / 0.019
                out.append(((x0 + tx, ty, 0.0015), (0.0078, 0.0088, 0.0080)))
    return out


PAD_SHAPES = _pad_shapes()


def material_fields(p):
    """Smooth scalar fields whose zero sets are the material boundaries (negative = inside).

    Returns dict: nose, nostril (the nostrils' walls deep inside the leather), eye (lid rims and
    socket walls), mouth (lip band), pad, plus the boolean `gum_side` telling which positive-mouth
    side is gum rather than skin."""
    p = np.asarray(p, dtype=F32)
    q = hrel(p)
    n = len(p)
    out = {k: np.full(n, 1.0, dtype=F32) for k in ("nose", "nostril", "eye", "mouth", "pad")}
    gum = np.zeros(n, dtype=bool)
    near = np.all((p >= HEAD_LO) & (p <= HEAD_HI), axis=1)
    if near.any():
        hq = q[near]
        d_nose = f_nose(hq)
        out["nose"][near] = d_nose - F32(0.0006)
        # INSIDE THE NOSTRILS the leather's wet film caught the key light on the walls the
        # camera looks into, and the pair read as glass beads (Ole's review, #1533). A real
        # nostril is dark because its inside is dry and unlit: past NOSTRIL_DRY into the
        # leather, on the nostrils' walls, the surface is DogPad's matte dark skin.
        out["nostril"][near] = np.maximum(d_nose + F32(NOSTRIL_DRY), f_nostrils(hq) - F32(0.001))
        e = np.full(len(hq), 1.0, dtype=F32)
        for s in (1.0, -1.0):
            e = np.minimum(e, _norm(hq - EYE_OFF * v3((s, 1, 1))) - F32(EYE_R + SOCKET_CLEAR + EYE_RIM))
        out["eye"][near] = e
        zone = (hq[:, 1] < POCKET_END + 0.02) & (hq[:, 2] < -0.04)
        if zone.any():
            zq = hq[zone]
            dU = f_upper_head(zq)
            dJ = f_jaw(zq)
            dP = f_pocket(zq)
            carved = np.maximum(dU, -dP)
            on_jaw = np.abs(dJ) < np.abs(carved)
            s_up = np.maximum(dP - F32(LIP_BAND), -dU - F32(LIP_INNER))
            s_lo = np.maximum(np.maximum(-dU - F32(LIP_INNER), dU - F32(LIP_LOWER_OUT)), zq[:, 1] - F32(POCKET_END) - F32(LIP_BAND))
            m = np.where(on_jaw, s_lo, s_up)
            sub = out["mouth"][near]
            sub[zone] = m
            out["mouth"][near] = sub
            g = np.zeros(len(hq), dtype=bool)
            g[zone] = dU < -F32(LIP_INNER)
            gum[near] = g
    low = p[:, 2] < 0.02
    if low.any():
        d = np.full(int(low.sum()), 1.0, dtype=F32)
        for c, r in PAD_SHAPES:
            d = np.minimum(d, sd_ellipsoid(p[low], c, r))
        out["pad"][low] = d
    out["gum_side"] = gum
    return out


def classify_by_fields(centres):
    f = material_fields(centres)
    m = np.full(len(centres), M_SKIN, dtype=np.int32)
    mouth_in = f["mouth"] < 0
    m[(f["mouth"] >= 0) & f["gum_side"]] = M_GUM
    m[mouth_in] = M_LIP
    m[f["eye"] < 0] = M_LIP
    m[f["pad"] < 0] = M_PAD
    m[f["nose"] < 0] = M_NOSE
    m[f["nostril"] < 0] = M_PAD
    return m


def cut_isocontour(co, tri, s, snap=0.22):
    """Insert the zero set of the per-vertex scalar s into the triangle mesh as edges.

    Vertices whose nearest crossing lies within `snap` of an edge's length are moved onto the
    contour first (no slivers); then every edge whose ends have strictly opposite signs is split
    at the linear crossing and its triangles re-cut. Returns co, tri, s (new vertices s = 0),
    and the ids of vertices that moved or were created."""
    co = np.asarray(co, dtype=np.float64).copy()
    tri = np.asarray(tri, dtype=np.int64)
    s = np.asarray(s, dtype=np.float64).copy()
    nv = len(co)
    e = np.unique(np.sort(np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]]), axis=1), axis=0)
    sa, sb = s[e[:, 0]], s[e[:, 1]]
    cr = (sa * sb) < 0
    t = np.where(cr, sa / np.where(cr, sa - sb, 1.0), 0.5)
    moved = []
    # snap: each vertex takes its closest near crossing
    best_t = np.full(nv, np.inf)
    target = np.zeros((nv, 3))
    for (va, vb, tt) in ((e[:, 0], e[:, 1], t), (e[:, 1], e[:, 0], 1.0 - t)):
        sel = cr & (tt < snap)
        idx = np.nonzero(sel)[0]
        for i in idx[np.argsort(tt[idx])[::-1]]:  # ascending overwrite keeps the smallest
            a = va[i]
            if tt[i] < best_t[a]:
                best_t[a] = tt[i]
                target[a] = co[a] + (co[vb[i]] - co[a]) * tt[i]
    snapv = np.nonzero(np.isfinite(best_t))[0]
    co[snapv] = target[snapv]
    s[snapv] = 0.0
    moved += snapv.tolist()
    # split the strictly crossing edges
    sides = np.stack([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]], axis=1)
    keys = np.minimum(sides[..., 0], sides[..., 1]) * nv + np.maximum(sides[..., 0], sides[..., 1])
    uk, inv = np.unique(keys.ravel(), return_inverse=True)
    inv = inv.reshape(-1, 3)
    ea, eb = uk // nv, uk % nv
    sa, sb = s[ea], s[eb]
    split = (sa * sb) < 0
    tt = np.where(split, sa / np.where(split, sa - sb, 1.0), 0.0)
    new_id = np.full(len(uk), -1, dtype=np.int64)
    new_id[split] = nv + np.arange(split.sum())
    newpos = co[ea[split]] + (co[eb[split]] - co[ea[split]]) * tt[split][:, None]
    co = np.concatenate([co, newpos])
    s = np.concatenate([s, np.zeros(int(split.sum()))])
    moved += list(range(nv, len(co)))
    sp = split[inv]
    mm = new_id[inv]
    cnt = sp.sum(axis=1)
    out = [tri[cnt == 0]]
    for n_ in (1, 2):
        sel = np.nonzero(cnt == n_)[0]
        if len(sel) == 0:
            continue
        ss = sp[sel]
        r = np.argmax(ss, axis=1) if n_ == 1 else (np.argmin(ss, axis=1) + 1) % 3
        idx = (np.arange(3)[None, :] + r[:, None]) % 3
        v = np.take_along_axis(tri[sel], idx, axis=1)
        mp = np.take_along_axis(mm[sel], idx, axis=1)
        a, b, c = v[:, 0], v[:, 1], v[:, 2]
        m0, m1 = mp[:, 0], mp[:, 1]
        if n_ == 1:
            out += [np.stack([a, m0, c], 1), np.stack([m0, b, c], 1)]
        else:
            out.append(np.stack([m0, b, m1], 1))
            d1 = np.linalg.norm(co[a] - co[m1], axis=1)
            d2 = np.linalg.norm(co[m0] - co[c], axis=1)
            use1 = d1 <= d2
            out.append(np.where(use1[:, None], np.stack([a, m0, m1], 1), np.stack([a, m0, c], 1)))
            out.append(np.where(use1[:, None], np.stack([a, m1, c], 1), np.stack([m0, m1, c], 1)))
    return co, np.concatenate(out), s, np.unique(np.array(moved, dtype=np.int64))


def cut_material_boundaries(co, tri, log=print):
    """Cut every material boundary into the mesh as an exact contour; re-project what moved."""
    for key in ("nose", "nostril", "eye", "mouth", "pad"):
        s = material_fields(co.astype(F32))[key].astype(np.float64)
        co, tri, _, moved = cut_isocontour(co, tri, s)
        if len(moved):
            co[moved] = project_to_surface(dog_sdf, co[moved], iterations=3)
        log(f"[mesh] cut {key} contour: {len(moved)} vertices moved or added")
    return co, tri


# ----------------------------------------------------------------------------
# The armature (bone names are a contract with the engine scene)
# ----------------------------------------------------------------------------

NON_DEFORM = {"root", "eye_L", "eye_R"}


def ear_mid(side, z, y=EAR_OUTLINE_C[0]):
    """A point on the ear flap's mid-surface at height z (head frame), found by sampling the SDF."""
    xs = np.linspace(0.05, 0.17, 2401).astype(F32)
    q = np.stack([side * xs, np.full_like(xs, y), np.full_like(xs, z)], axis=1)
    d = f_ear(q, side)
    return np.array((side * xs[int(np.argmin(d))], y, z), dtype=np.float64)


def bone_table():
    """name -> (head, tail, parent, connected). Blender coordinates."""
    H = HEAD.astype(np.float64)
    B = {}

    def add(name, head, tail, parent=None, connect=False):
        B[name] = (np.asarray(head, dtype=np.float64), np.asarray(tail, dtype=np.float64), parent, connect)

    add("root", (0.0, 0.165, 0.0), (0.0, 0.265, 0.0))
    spine = [(0.0, 0.165, 0.362), (0.0, 0.070, 0.378), (0.0, -0.030, 0.388), (0.0, -0.130, 0.398), (0.0, -0.215, 0.412)]
    add("pelvis", spine[0], spine[1], "root")
    add("spine_01", spine[1], spine[2], "pelvis", True)
    add("spine_02", spine[2], spine[3], "spine_01", True)
    add("spine_03", spine[3], spine[4], "spine_02", True)
    add("neck_01", (0.0, -0.228, 0.420), (0.0, -0.284, 0.472), "spine_03")
    add("neck_02", (0.0, -0.284, 0.472), (0.0, -0.322, 0.522), "neck_01", True)
    add("head", (0.0, -0.322, 0.522), H + (0.0, -0.120, -0.010), "neck_02", True)
    add("jaw", H + (0.0, -0.010, -0.064), H + (0.0, -0.170, -0.084), "head")
    ty = TONGUE_BONE_Y
    add("tongue_01", H + (0.0, ty[0], TONGUE_Z), H + (0.0, ty[1], TONGUE_Z), "jaw")
    add("tongue_02", H + (0.0, ty[1], TONGUE_Z), H + (0.0, ty[2], TONGUE_Z), "tongue_01", True)
    add("tongue_03", H + (0.0, ty[2], TONGUE_Z), H + (0.0, ty[3], TONGUE_Z), "tongue_02", True)
    for s, sfx in ((1.0, "L"), (-1.0, "R")):
        e = [H + ear_mid(s, z) for z in (EAR_TOP - 0.006, -0.016, -0.066, EAR_OUTLINE_C[1] - EAR_OUTLINE_R[1] + 0.012)]
        add(f"ear_{sfx}_01", e[0], e[1], "head")
        add(f"ear_{sfx}_02", e[1], e[2], f"ear_{sfx}_01", True)
        add(f"ear_{sfx}_03", e[2], e[3], f"ear_{sfx}_02", True)
        c, f, up, axis = eye_frame(s)
        add(f"eye_{sfx}", c, c + 0.030 * f, "head")
        add(f"lid_upper_{sfx}", c, c + 0.024 * f, "head")
        add(f"lid_lower_{sfx}", c, c + 0.020 * f, "head")
        add(f"brow_{sfx}", H + (s * 0.046, -0.070, 0.030), H + (s * 0.046, -0.094, 0.034), "head")
        fj = [np.array((s * a[0], a[1], a[2])) for a in FRONT_JOINTS]
        add(f"scapula_{sfx}", fj[0], fj[1], "spine_03")
        add(f"upperarm_{sfx}", fj[1], fj[2], f"scapula_{sfx}", True)
        add(f"forearm_{sfx}", fj[2], fj[3], f"upperarm_{sfx}", True)
        add(f"wrist_{sfx}", fj[3], fj[4], f"forearm_{sfx}", True)
        add(f"paw_front_{sfx}", fj[4], fj[5], f"wrist_{sfx}", True)
        hj = [np.array((s * a[0], a[1], a[2])) for a in HIND_JOINTS]
        add(f"thigh_{sfx}", hj[0], hj[1], "pelvis")
        add(f"shin_{sfx}", hj[1], hj[2], f"thigh_{sfx}", True)
        add(f"hock_{sfx}", hj[2], hj[3], f"shin_{sfx}", True)
        add(f"paw_rear_{sfx}", hj[3], hj[4], f"hock_{sfx}", True)
    for i in range(6):
        add(f"tail_{i + 1:02d}", TAIL_PTS[i], TAIL_PTS[i + 1], "pelvis" if i == 0 else f"tail_{i:02d}", i > 0)
    return B


def build_armature(name="DogRig"):
    """The armature object. Every bone's local X points to the dog's left (+X), so a rotation about
    local X is always the flex / pitch axis (and, for eye and lid bones, the blink axis)."""
    import bpy

    table = bone_table()
    data = bpy.data.armatures.new(name)
    arm = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    with bpy.context.temp_override(active_object=arm, object=arm, edit_object=arm):
        bpy.ops.object.mode_set(mode="EDIT")
        eb = data.edit_bones
        for nm, (h, t, parent, connect) in table.items():
            b = eb.new(nm)
            b.head = h
            b.tail = t
        for nm, (h, t, parent, connect) in table.items():
            b = eb[nm]
            if parent:
                b.parent = eb[parent]
                b.use_connect = connect
            y = (t - h) / np.linalg.norm(t - h)
            if nm == "root":
                b.roll = 0.0
            else:
                z = np.cross((1.0, 0.0, 0.0), y)
                b.align_roll(z / np.linalg.norm(z))
            b.use_deform = nm not in NON_DEFORM
        bpy.ops.object.mode_set(mode="OBJECT")
    return arm


# ----------------------------------------------------------------------------
# Skin weights
# ----------------------------------------------------------------------------

HEAT_SCALE = 100.0  # bone heat fails outright on a sub-millimetre mesh; solve on a scaled copy
BROW_INNER, BROW_OUTER = 0.012, 0.030  # brow influence falls off between these distances (m)
EAR_ROOT_INNER = 0.003  # head skin this close to an ear keeps the ear root's full weight
EAR_ROOT_OUTER = 0.018  # ... and from here out, the head alone (at 30 mm the eye's outer corner kept 0.57)
EYE_SKIN_RIGID = 0.020  # skin within this of the socket follows the head and the brows, never an ear
JAW_FALLOFF = 0.035  # the jaw's pull fades over this distance from the chin into the throat and jowls (m)
JAW_FALLOFF_PEAK = 0.9
JAW_FALLOFF_BELOW = -0.072  # only skin below the lip line (head frame z) follows the jaw at all


def heat_weights(body, arm, exclude, log=print):
    """Blender's bone-heat weights for `body`, solved on 100x scaled copies. Returns (V, B) array, bone names."""
    import bpy
    from mathutils import Matrix

    scene = bpy.context.scene
    bcopy = body.copy()
    bcopy.data = body.data.copy()
    bcopy.vertex_groups.clear()
    scene.collection.objects.link(bcopy)
    bcopy.data.transform(Matrix.Scale(HEAT_SCALE, 4))
    acopy = arm.copy()
    acopy.data = arm.data.copy()
    scene.collection.objects.link(acopy)
    bpy.context.view_layer.objects.active = acopy
    with bpy.context.temp_override(active_object=acopy, object=acopy, edit_object=acopy):
        bpy.ops.object.mode_set(mode="EDIT")
        rest = {e.name: (e.head.copy(), e.tail.copy()) for e in acopy.data.edit_bones}
        for e in acopy.data.edit_bones:
            e.head = rest[e.name][0] * HEAT_SCALE
            e.tail = rest[e.name][1] * HEAT_SCALE
            if e.name in exclude:
                e.use_deform = False
        bpy.ops.object.mode_set(mode="OBJECT")
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    bcopy.select_set(True)
    acopy.select_set(True)
    bpy.context.view_layer.objects.active = acopy
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    names = [b.name for b in arm.data.bones]
    col = {n: i for i, n in enumerate(names)}
    W = np.zeros((len(bcopy.data.vertices), len(names)))
    gname = {g.index: g.name for g in bcopy.vertex_groups}
    for v in bcopy.data.vertices:
        for g in v.groups:
            nm = gname[g.group]
            if nm in col:
                W[v.index, col[nm]] = g.weight
    empty = int((W.sum(axis=1) < 1e-9).sum())
    log(f"[weights] bone heat: {empty} of {len(W)} vertices without a solution")
    me_c, arm_c = bcopy.data, acopy.data
    bpy.data.objects.remove(bcopy)
    bpy.data.objects.remove(acopy)
    bpy.data.meshes.remove(me_c)
    bpy.data.armatures.remove(arm_c)
    return W, names


def _vertex_edges(tri):
    e = np.unique(np.sort(np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]]), axis=1), axis=0)
    return np.concatenate([e[:, 0], e[:, 1]]), np.concatenate([e[:, 1], e[:, 0]])


def smooth_weights(W, tri, iterations, factor=0.5, lock=None, lock_values=None):
    src, dst = _vertex_edges(tri)
    deg = np.bincount(src, minlength=len(W)).astype(np.float64)
    for _ in range(iterations):
        acc = np.zeros_like(W)
        np.add.at(acc, src, W[dst])
        W = W + factor * (acc / np.maximum(deg, 1.0)[:, None] - W)
        if lock is not None:
            W[lock] = lock_values
    return W


def limit_normalize(W, k=4, floor=0.01):
    """Keep the k largest influences per vertex (dropping tiny ones), renormalised to sum 1."""
    W = np.where(W < floor, 0.0, W)
    if W.shape[1] > k:
        order = np.argsort(-W, axis=1, kind="stable")
        keep = np.zeros_like(W, dtype=bool)
        np.put_along_axis(keep, order[:, :k], True, axis=1)
        W = np.where(keep, W, 0.0)
    s = W.sum(axis=1, keepdims=True)
    return W / np.maximum(s, 1e-12)


def fill_unweighted(W, tri, iterations=200):
    """Vertices bone heat left empty take their neighbours' weights (diffused until every vertex has some)."""
    src, dst = _vertex_edges(tri)
    empty = W.sum(axis=1) < 1e-9
    for _ in range(iterations):
        if not empty.any():
            break
        acc = np.zeros_like(W)
        np.add.at(acc, src, W[dst])
        got = empty & (acc.sum(axis=1) > 1e-9)
        W[got] = acc[got] / acc[got].sum(axis=1, keepdims=True)
        empty &= ~got
    return W


def body_weights(co, tri, W, names, log=print):
    """Constrain the heat weights: which bones may pull which skin, the jaw split, smooth, limit to 4."""
    col = {n: i for i, n in enumerate(names)}
    p = co.astype(F32)
    R = region_fields(p)
    rn = list(R.keys())
    lab = np.array(rn)[np.argmin(np.stack([R[n] for n in rn], axis=1), axis=1)]
    q = hrel(p)
    x = p[:, 0]
    W = fill_unweighted(W, tri)

    def zero(mask, bones):
        for b in bones:
            W[mask, col[b]] = 0.0

    legs_l = [f"{b}_L" for b in ("scapula", "upperarm", "forearm", "wrist", "paw_front", "thigh", "shin", "hock", "paw_rear")]
    legs_r = [b[:-1] + "R" for b in legs_l]
    lower_legs = [f"{b}_{s}" for s in "LR" for b in ("forearm", "wrist", "paw_front", "shin", "hock", "paw_rear")]
    tails = [f"tail_{i:02d}" for i in range(1, 7)]
    ears = {s: [f"ear_{s}_0{i}" for i in (1, 2, 3)] for s in "LR"}
    face = ["jaw", "brow_L", "brow_R", "tongue_01", "tongue_02", "tongue_03",
            "lid_upper_L", "lid_lower_L", "lid_upper_R", "lid_lower_R"]
    # side separation
    zero(x > 0.012, legs_r)
    zero(x < -0.012, legs_l)
    zero(np.abs(x) <= 0.012, legs_l[2:5] + legs_r[2:5] + legs_l[6:] + legs_r[6:])
    not_leg = ~np.isin(lab, ["fl", "fr", "hl", "hr"])
    zero(not_leg, lower_legs)
    # ears: only their own chain and the head; the rest of the body never sees ear_02/03
    for s, key in (("L", "ear_l"), ("R", "ear_r")):
        m = lab == key
        keep = set(ears[s] + ["head"])
        zero(m, [n for n in names if n not in keep])
        zero(~m, ears[s][1:])
    # tail: its own chain plus the pelvis and lumbar spine
    m = lab == "tail"
    keep = set(tails + ["pelvis", "spine_01"])
    zero(m, [n for n in names if n not in keep])
    zero(~m, tails[1:])
    # nothing below the head is pulled by face bones; the skin never follows the lids or the tongue
    zero(lab != "head", face)
    zero(np.ones(len(p), dtype=bool), [f for f in face if f.startswith(("lid", "tongue"))])
    zero(~np.isin(lab, ["head", "ear_l", "ear_r"]), ears["L"][:1] + ears["R"][:1])
    # the head is not pulled by the legs, tail or lumbar spine
    zero(lab == "head", legs_l + legs_r + tails + ["pelvis", "spine_01", "spine_02"])
    # What a falloff below takes from a bone on the head's skin, the HEAD gets: removed outright, a
    # vertex bone heat had given wholly to that bone was left with no weight at all, stayed at the
    # bind pose while the head moved, and pushed bald flaps out through the coat.
    head_col = col["head"]
    on_head = lab == "head"

    def hand_to_head(mask, bone, keep):
        removed = W[mask, col[bone]] * (1.0 - keep)
        W[mask, col[bone]] -= removed
        W[mask, head_col] += removed

    # brows: a soft pad around the brow, not the whole forehead
    for s in ("L", "R"):
        c = np.array(bone_table()[f"brow_{s}"][0], dtype=np.float32)
        d = np.linalg.norm(p[on_head] - c, axis=1)
        hand_to_head(on_head, f"brow_{s}", np.clip((BROW_OUTER - d) / (BROW_OUTER - BROW_INNER), 0.0, 1.0) ** 2)
    # ear roots: on the head, only the skin along each ear's attachment. Bone heat handed them the
    # temples and the crown between the ears, and every ear twitch lifted the back of the head with
    # the ear and dragged the skin at the eye's outer corner. The falloff is measured from the EAR
    # (its own field), not from the bone's head: measured from one point, the head skin along the
    # rest of the attachment lost the weight the ear beside it kept, and the twitch tore it open.
    for s, key in (("L", "ear_l"), ("R", "ear_r")):
        d = np.maximum(R[key][on_head], 0.0)
        hand_to_head(on_head, f"ear_{s}_01",
                     np.clip((EAR_ROOT_OUTER - d) / (EAR_ROOT_OUTER - EAR_ROOT_INNER), 0.0, 1.0) ** 2)
    # ...and none of it round the eyes: the skin over the socket rides the head (and the brow), or it
    # slides across the lids beneath it.
    for side, s in ((1.0, "L"), (-1.0, "R")):
        near_eye = on_head & (np.linalg.norm(p - eye_centre(side), axis=1) < EYE_R + SOCKET_CLEAR + EYE_SKIN_RIGID)
        hand_to_head(near_eye, f"ear_{s}_01", 0.0)
    # the mouth: the lower jaw's skin moves with the jaw, the upper lips and palate with the head
    jaw = col["jaw"]
    lock = np.zeros(len(p), dtype=bool)
    near = np.all((p >= HEAD_LO) & (p <= HEAD_HI), axis=1)
    hq = q[near]
    dJ = f_jaw(hq)
    dU = f_upper_head(hq)
    dP = f_pocket(hq)
    carved = np.maximum(dU, -dP)
    on_jaw = (np.abs(dJ) < np.abs(carved)) & (np.abs(dJ) < 0.002)
    front = hq[:, 1] < POCKET_END - 0.004
    lower = on_jaw & front
    upper = ~on_jaw & (hq[:, 1] < POCKET_END + 0.004) & (hq[:, 2] > -0.10) & (np.abs(carved) < 0.002)
    idx = np.nonzero(near)[0]
    jl = idx[lower]
    ju = idx[upper]
    W[jl, :] = 0.0
    W[jl, jaw] = 1.0
    W[ju, jaw] = 0.0
    lock[jl] = True
    s_u = W[ju].sum(axis=1)
    W[ju, col["head"]] += np.maximum(1.0 - s_u, 0.0)
    lock[ju] = True
    # Every other vertex loses whatever jaw weight bone heat gave it, then the throat and the
    # jowls below the lip line take a falloff from the nearest chin vertex: the jaw's pull fades
    # over JAW_FALLOFF instead of stopping at the chin, where an open mouth tore a crease.
    W[~lock, jaw] = 0.0
    if len(jl):
        from mathutils import kdtree

        kd = kdtree.KDTree(len(jl))
        for i in jl.tolist():
            kd.insert(p[i].tolist(), i)
        kd.balance()
        cand = np.nonzero(near & ~lock)[0]
        cand = cand[q[cand, 2] < JAW_FALLOFF_BELOW]
        d = np.array([kd.find(p[i].tolist())[2] for i in cand.tolist()])
        wj = JAW_FALLOFF_PEAK * (1.0 - _smoothstep(0.0, JAW_FALLOFF, d))
        rest_w = W[cand]
        rest_w /= np.maximum(rest_w.sum(axis=1, keepdims=True), 1e-12)
        W[cand] = rest_w * (1.0 - wj)[:, None]
        W[cand, jaw] = wj
        log(f"[weights] jaw falloff over {int((wj > 0.01).sum())} throat and jowl vertices")
    W = W / np.maximum(W.sum(axis=1, keepdims=True), 1e-12)
    locked_vals = W[lock].copy()
    W = smooth_weights(W, tri, iterations=6, factor=0.5, lock=lock, lock_values=locked_vals)
    W = limit_normalize(W, 4)
    # No vertex leaves without a bone: one that has none stays at the bind pose whatever the
    # skeleton does. Refilled from its neighbours, and counted, so a constraint that strands skin
    # says so here instead of in a render.
    stranded = int((W.sum(axis=1) < 0.5).sum())
    if stranded:
        W = limit_normalize(fill_unweighted(np.where(W.sum(axis=1, keepdims=True) < 0.5, 0.0, W), tri), 4)
    log(f"[weights] body: jaw-locked {len(jl)}, head-locked mouth {len(ju)}, max influences {(W > 0).sum(axis=1).max()}, "
        f"refilled {stranded} stranded, still unweighted {int((W.sum(axis=1) < 0.5).sum())}")
    return W


def chain_weights(t, knots, blend=0.35):
    """Weights along a bone chain: t is a coordinate along it, knots the joint coordinates
    (n_bones + 1, increasing); each joint blends its two bones over +-blend x the shorter one."""
    n = len(knots) - 1
    W = np.zeros((len(t), n))
    for i in range(n):
        a, b = knots[i], knots[i + 1]
        w = np.ones(len(t))
        if i > 0:
            h = blend * min(b - a, a - knots[i - 1])
            w *= np.clip((t - (a - h)) / (2 * h), 0.0, 1.0)
        if i < n - 1:
            h = blend * min(b - a, knots[i + 2] - b)
            w *= np.clip(((b + h) - t) / (2 * h), 0.0, 1.0)
        W[:, i] = w
    W /= np.maximum(W.sum(axis=1, keepdims=True), 1e-12)
    return W


# ----------------------------------------------------------------------------
# Assembly: the body, four lid shells, the tongue and the teeth as ONE skinned mesh
# ----------------------------------------------------------------------------

PIECES = ["body", "lid_upper_L", "lid_lower_L", "lid_upper_R", "lid_lower_R", "tongue", "teeth_upper", "teeth_lower"]


def assemble(body, W_body, bone_names, log=print):
    """Join the body with the lids, the tongue and the teeth into `body`'s mesh (one skinned mesh,
    one material slot per MATERIALS entry). Returns the full (V, B) weight matrix."""
    import bpy

    co_b, tri_b = _mesh_arrays(body.data)
    assert len(co_b) == len(W_body), "the body weights were solved on a different mesh"
    col = {n: i for i, n in enumerate(bone_names)}
    cos, tris, mats, Ws = [co_b], [tri_b], [classify_by_fields(co_b[tri_b].mean(axis=1).astype(F32))], [W_body]
    base = len(co_b)
    lid_bones = ["lid_upper_L", "lid_lower_L", "lid_upper_R", "lid_lower_R"]
    for k, (s, up) in enumerate(((1.0, True), (1.0, False), (-1.0, True), (-1.0, False))):
        v, q, mt, _uv = lid_shell(s, up)
        t = np.concatenate([q[:, [0, 1, 2]], q[:, [0, 2, 3]]])
        cos.append(v)
        tris.append(t + base)
        mats.append(np.concatenate([mt, mt]))
        w = np.zeros((len(v), len(bone_names)))
        w[:, col[lid_bones[k]]] = 1.0
        Ws.append(w)
        base += len(v)
    co_t, tri_t = build_tongue_mesh(log=log)
    tq = co_t - HEAD.astype(np.float64)
    ty = np.asarray(TONGUE_BONE_Y, dtype=np.float64)
    cw = chain_weights(-tq[:, 1], -ty)  # along the tongue, root to tip
    w = np.zeros((len(co_t), len(bone_names)))
    for i, nm in enumerate(("tongue_01", "tongue_02", "tongue_03")):
        w[:, col[nm]] = cw[:, i]
    cos.append(co_t)
    tris.append(tri_t + base)
    mats.append(np.full(len(tri_t), M_TONGUE))
    Ws.append(w)
    base += len(co_t)
    for v, t, jaw in build_teeth():
        cos.append(v)
        tris.append(t + base)
        mats.append(np.full(len(t), M_TEETH))
        w = np.zeros((len(v), len(bone_names)))
        w[:, col["head" if jaw == "upper" else "jaw"]] = 1.0
        Ws.append(w)
        base += len(v)
    co = np.concatenate(cos)
    tri = np.concatenate(tris)
    mat = np.concatenate(mats).astype(np.int32)
    W = np.concatenate(Ws)
    old = body.data
    me = bl_mesh("Dog", co, tri)
    body.data = me
    bpy.data.meshes.remove(old)
    for nm in MATERIALS:
        me.materials.append(bpy.data.materials.get(nm) or bpy.data.materials.new(nm))
    me.polygons.foreach_set("material_index", mat)
    me.shade_smooth()
    counts = {MATERIALS[i]: int((mat == i).sum()) for i in range(len(MATERIALS))}
    log(f"[assemble] {len(tri)} triangles, {len(co)} vertices; per material {counts}")
    return W


def write_weights(ob, arm, W, names, log=print):
    """Vertex groups from the (V, B) weights, and the armature modifier + parent (no inverse)."""
    Wl = limit_normalize(W, 4)
    for j, nm in enumerate(names):
        if nm in NON_DEFORM:
            continue
        vg = ob.vertex_groups.new(name=nm)
        nz = np.nonzero(Wl[:, j] > 0.0)[0]
        if len(nz) == 0:
            continue
        # bucket by quantised weight: one add() call per distinct value
        wq = np.round(Wl[nz, j], 4)
        for val in np.unique(wq):
            vg.add(nz[wq == val].tolist(), float(val), "REPLACE")
    mod = ob.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    ob.parent = arm
    log(f"[weights] wrote {len(ob.vertex_groups)} groups; max influences {(Wl > 0).sum(axis=1).max()}")


# ----------------------------------------------------------------------------
# UVs and the coat colour map
# ----------------------------------------------------------------------------

COLOR_MAP_SIZE = 2048


def unwrap(ob, log=print):
    """Smart-project UVs: unique, non-overlapping, inside [0,1] (the groom keys its colour map and
    clump cells on the ROOT UV, so the pelt may not share UV space anywhere)."""
    import bpy

    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    with bpy.context.temp_override(active_object=ob, object=ob, edit_object=ob):
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(72.0), island_margin=0.004, area_weight=0.0,
                                 correct_aspect=True, scale_to_bounds=False)
        bpy.ops.object.mode_set(mode="OBJECT")
    uv = np.zeros(len(ob.data.loops) * 2, dtype=np.float32)
    ob.data.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    log(f"[uv] smart project: uv range [{uv.min():.3f}, {uv.max():.3f}]")


def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


# The coat, as sRGB: a warm golden retriever mix. The groom's colour map and DogSkin's albedo are
# the same texture, so a gap between strands reads as coat.
COAT_GOLD = (0.80, 0.56, 0.28)
COAT_CREAM = (0.93, 0.80, 0.58)   # chest, belly, inner legs, paws, muzzle, tail underside
COAT_RED = (0.74, 0.47, 0.22)     # the ears and the ridge of the back: a deeper gold (a browner red read as bare leather)
COAT_SADDLE = (0.74, 0.49, 0.23)  # the top of the back and neck


def coat_colour(p, n):
    """Per-vertex sRGB coat colour from position and normal (Blender coordinates)."""
    p = np.asarray(p, dtype=F32)
    n = np.asarray(n, dtype=np.float64)
    R = region_fields(p)
    names = list(R.keys())
    lab = np.array(names)[np.argmin(np.stack([R[k] for k in names], axis=1), axis=1)]
    q = hrel(p).astype(np.float64)
    pd = p.astype(np.float64)
    gold, cream, red, saddle = (np.asarray(c) for c in (COAT_GOLD, COAT_CREAM, COAT_RED, COAT_SADDLE))
    c = np.tile(gold, (len(p), 1))
    # the back and the top of the neck: a slightly deeper saddle
    top = _smoothstep(0.35, 0.85, n[:, 2]) * np.isin(lab, ["torso", "neck"])
    c += (saddle - gold) * top[:, None]
    # underside: chest, brisket and belly
    under = _smoothstep(-0.15, -0.6, n[:, 2]) * np.isin(lab, ["torso", "neck"])
    brisket = _smoothstep(-0.10, -0.35, n[:, 1]) * (lab == "torso") * _smoothstep(0.42, 0.30, pd[:, 2])
    c += (cream - c) * np.clip(under + 0.8 * brisket, 0.0, 1.0)[:, None]
    # legs: cream on the inside, fading lighter toward the paws
    legs = np.isin(lab, ["fl", "fr", "hl", "hr"])
    inner = _smoothstep(0.1, 0.6, -n[:, 0] * np.sign(pd[:, 0] + 1e-9)) * legs
    low = _smoothstep(0.16, 0.05, pd[:, 2]) * legs
    c += (cream - c) * np.clip(0.7 * inner + 0.75 * low, 0.0, 1.0)[:, None]
    # the head: a lighter muzzle and cheeks, the cranium golden
    head = lab == "head"
    muzzle = _smoothstep(-0.07, -0.13, q[:, 1]) * head
    c += (cream - c) * (0.75 * muzzle)[:, None]
    # ears: deeper red-gold, darkest at the tips
    ears = np.isin(lab, ["ear_l", "ear_r"])
    tip = _smoothstep(0.02, -0.12, q[:, 2])
    c += (red - c) * (ears * (0.65 + 0.35 * tip))[:, None]
    # tail: a deeper gold than the body, the underside barely paler. The plume is a few fibres deep
    # where the body is dozens, so its fibres gather far less of the colour dual scattering compounds
    # with every crossing: at the body's gold the flag read as a pale cream brush beside a golden dog
    # (Ole's review, #1533); at 0.8 cream it read white.
    tail = lab == "tail"
    c += (red - c) * (tail * 0.45)[:, None]
    c += (cream - c) * (tail * _smoothstep(0.0, -0.6, n[:, 2]) * 0.10)[:, None]
    # large, soft variation so the coat is not a flat fill (deterministic)
    k = 2.0 * math.pi
    v = (np.sin(k * (pd[:, 0] * 3.1 + pd[:, 1] * 2.3 + 0.4)) * np.sin(k * (pd[:, 2] * 2.7 - pd[:, 1] * 1.3 + 0.9))
         + 0.5 * np.sin(k * (pd[:, 0] * 7.3 - pd[:, 2] * 5.1 + pd[:, 1] * 4.7)))
    c *= (1.0 + 0.045 * v)[:, None]
    return np.clip(c, 0.0, 1.0)


def fill_empty_texels(rgb, mask):
    """Push-pull fill: every texel outside `mask` takes the mask-weighted average of the nearest
    covered texels at the coarsest pyramid level that has any. Mipmaps and bilinear taps near a UV
    seam then average coat with coat instead of with the black the bake left outside the islands."""
    levels = [(rgb * mask[..., None], mask.astype(np.float32))]
    while levels[-1][1].shape[0] > 1:
        c, w = levels[-1]
        h = c.shape[0] // 2
        c2 = c.reshape(h, 2, h, 2, 3).sum(axis=(1, 3))
        w2 = w.reshape(h, 2, h, 2).sum(axis=(1, 3))
        levels.append((c2, w2))
    colour = levels[-1][0] / np.maximum(levels[-1][1], 1e-9)[..., None]
    for c, w in reversed(levels[:-1]):
        up = np.repeat(np.repeat(colour, 2, axis=0), 2, axis=1)
        mine = c / np.maximum(w, 1e-9)[..., None]
        colour = np.where((w > 0)[..., None], mine, up)
    return np.where(mask[..., None], rgb, colour)


def erode(mask, texels):
    """`mask` with every texel within `texels` (4-neighbourhood steps) of an uncovered one cleared."""
    m = mask.copy()
    for _ in range(texels):
        e = m.copy()
        e[1:, :] &= m[:-1, :]
        e[:-1, :] &= m[1:, :]
        e[:, 1:] &= m[:, :-1]
        e[:, :-1] &= m[:, 1:]
        m = e
    return m


def bake_coat_map(ob, out_path, log=print):
    """Bake the per-vertex coat colour into a UV texture with Cycles (EMIT pass)."""
    import bpy

    me = ob.data
    co = np.zeros(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    nrm = np.zeros(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("normal", nrm)
    srgb = coat_colour(co.reshape(-1, 3), nrm.reshape(-1, 3))
    # Soften every patch boundary over a few triangles: a boundary that crosses a 1 cm torso
    # triangle is otherwise a sawtooth in the bake.
    me.calc_loop_triangles()
    tri = np.zeros(len(me.loop_triangles) * 3, dtype=np.int32)
    me.loop_triangles.foreach_get("vertices", tri)
    srgb = smooth_weights(srgb, tri.reshape(-1, 3), iterations=12, factor=0.5)
    # The groom multiplies its fibres by this map as a LINEAR tint, which dual scattering then
    # compounds with every fibre crossing; the look was graded with the authored colours as that
    # tint. The engine decodes a colour PNG from sRGB (its name says colour), so the authored
    # colours go in as the linear values and the PNG holds their sRGB encoding. DogSkin's albedo,
    # the same texture, reads as the same colours: paler than a strict sRGB albedo, which is what
    # a gap in the coat should look like.
    lin = srgb
    attr = me.color_attributes.new("coat_bake", "FLOAT_COLOR", "POINT")
    rgba = np.concatenate([lin, np.ones((len(lin), 1))], axis=1).astype(np.float32)
    attr.data.foreach_set("color", rgba.ravel())
    img = bpy.data.images.new("DogCoatColor", COLOR_MAP_SIZE, COLOR_MAP_SIZE, alpha=False, float_buffer=False)
    img.colorspace_settings.name = "sRGB"
    bake = bpy.data.materials.new("CoatBake")
    nt = bake.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emit = nt.nodes.new("ShaderNodeEmission")
    at = nt.nodes.new("ShaderNodeAttribute")
    at.attribute_name = "coat_bake"
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(at.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
    nt.nodes.active = tex
    saved = list(me.materials)
    saved_idx = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("material_index", saved_idx)
    me.materials.clear()
    me.materials.append(bake)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    # NO MARGIN: what the bake covers is then exactly the islands, and the gutters are filled
    # below from the islands' interiors. Blender's margin copied each island's edge texels
    # outward -- some of them darkened where a texel straddles the island's edge -- and the fill
    # then counted those as coat, so the gutters held blocks down to near black, which mipmaps
    # and bilinear taps at every seam averaged back into the coat and the skin (#1533 review).
    scene.render.bake.margin = 0
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    t0 = time.time()
    bpy.ops.object.bake(type="EMIT", margin=0, use_clear=True)
    log(f"[coat] baked {COLOR_MAP_SIZE}^2 colour map in {time.time() - t0:.1f}s")
    px = np.zeros(COLOR_MAP_SIZE * COLOR_MAP_SIZE * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(COLOR_MAP_SIZE, COLOR_MAP_SIZE, 4)
    # The islands less the two texels at their edges, which can straddle it; everything else is
    # filled from what is left.
    covered = erode(px[..., :3].sum(axis=2) > 1e-4, 2)
    px[..., :3] = fill_empty_texels(px[..., :3], covered)
    darkest = float(px[..., :3].min())
    log(f"[coat] colour map: {int(covered.sum())} interior texels kept, darkest channel {darkest:.3f}")
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    me.materials.clear()
    for m in saved:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", saved_idx)
    me.color_attributes.remove(me.color_attributes["coat_bake"])
    bpy.data.materials.remove(bake)
    return img


# The nose leather's cobblestones (Ole's review, #1533): a dog's planum nasale is a mosaic of
# small domed polygons with grooves between them, and a smooth button under a wet coat read as a
# plastic bead. The cells are 1.5-2 mm, under two voxels of the body's 1.1 mm grid, so they are a
# tangent-space normal map baked from a procedural height rather than geometry. The leather is
# ~0.1% of the pelt, so in the pelt's atlas a 4096^2 map would still give it ~0.25 mm a texel --
# the grooves one texel wide -- and 64 MB of texture to a few square centimetres. It gets its own
# UV square instead: the bare skins grow no fur, and the groom keys only its roots on the UVs.
NOSE_NORMAL_SIZE = 2048  # ~0.02 mm a texel on the leather: finer than a pixel at any editor close-up
NOSE_CELL = 0.0017  # metres between cell centres
NOSE_RELIEF = 0.00035  # metres from groove floor to dome top
NOSE_AO_DISTANCE = 0.012  # metres: the nostrils and the crease where the leather meets the muzzle
NOSE_AO_SAMPLES = 64


def unwrap_nose(ob, log=print):
    """Re-unwrap the DogNose faces alone into the whole UV square (after the coat map is baked:
    they overlap the pelt's islands from here on, which only the nose material samples)."""
    import bpy

    me = ob.data
    idx = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("material_index", idx)
    face_sel = idx == M_NOSE
    vert_sel = np.zeros(len(me.vertices), dtype=bool)
    loop_v = np.zeros(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", loop_v)
    start = np.zeros(len(me.polygons), dtype=np.int32)
    total = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("loop_start", start)
    me.polygons.foreach_get("loop_total", total)
    for f in np.nonzero(face_sel)[0]:
        vert_sel[loop_v[start[f]:start[f] + total[f]]] = True
    edge_v = np.zeros(len(me.edges) * 2, dtype=np.int32)
    me.edges.foreach_get("vertices", edge_v)
    edge_sel = vert_sel[edge_v.reshape(-1, 2)].all(axis=1)
    me.polygons.foreach_set("select", face_sel)
    me.vertices.foreach_set("select", vert_sel)
    me.edges.foreach_set("select", edge_sel)
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    with bpy.context.temp_override(active_object=ob, object=ob, edit_object=ob):
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.01, area_weight=0.0,
                                 correct_aspect=True, scale_to_bounds=False)
        bpy.ops.object.mode_set(mode="OBJECT")
    uv = np.zeros(len(me.loops) * 2, dtype=np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    nose_loops = np.concatenate([np.arange(start[f], start[f] + total[f]) for f in np.nonzero(face_sel)[0]])
    lo, hi = uv[nose_loops].min(axis=0), uv[nose_loops].max(axis=0)
    log(f"[uv] nose: {int(face_sel.sum())} faces into their own square, uv [{lo[0]:.3f},{lo[1]:.3f}]-"
        f"[{hi[0]:.3f},{hi[1]:.3f}]")


def bake_nose_normal(ob, out_path, log=print):
    """Bake the nose leather's cobblestone relief into a tangent-space normal map (Cycles NORMAL).

    The height is a Voronoi mosaic in OBJECT space -- domed cells with narrow grooves between
    them -- so its scale is the same on every part of the leather whatever the UVs do. Baked from
    a copy holding only the DogNose faces, since from unwrap_nose on they share UV space with the
    pelt."""
    import bmesh
    import bpy

    dup = ob.copy()
    dup.data = ob.data.copy()
    bpy.context.collection.objects.link(dup)
    bm = bmesh.new()
    bm.from_mesh(dup.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != M_NOSE], context="FACES")
    bm.to_mesh(dup.data)
    bm.free()
    dup.data.polygons.foreach_set("material_index", np.zeros(len(dup.data.polygons), dtype=np.int32))

    img = bpy.data.images.new("DogNoseNormal", NOSE_NORMAL_SIZE, NOSE_NORMAL_SIZE, alpha=False,
                              float_buffer=False)
    img.colorspace_settings.name = "Non-Color"
    bake = bpy.data.materials.new("NoseBake")
    nt = bake.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    coord = nt.nodes.new("ShaderNodeTexCoord")
    # F1: each cell a dome, highest at its centre.
    dome = nt.nodes.new("ShaderNodeTexVoronoi")
    dome.voronoi_dimensions = "3D"
    dome.feature = "F1"
    dome.inputs["Scale"].default_value = 1.0 / NOSE_CELL
    # The grooves: narrow valleys along the cells' edges.
    edge = nt.nodes.new("ShaderNodeTexVoronoi")
    edge.voronoi_dimensions = "3D"
    edge.feature = "DISTANCE_TO_EDGE"
    edge.inputs["Scale"].default_value = 1.0 / NOSE_CELL
    nt.links.new(coord.outputs["Object"], dome.inputs["Vector"])
    nt.links.new(coord.outputs["Object"], edge.inputs["Vector"])
    # height = max(0, 1 - 4 F1^2) * smoothstep(0, 0.12, edge), both in cell units: a dome falling to
    # zero half a cell out, ~35 degrees steep at its foot. A shallower dome (1 - F1^2) left the lit
    # side one broad streak; a wet nose shows one small glint per cobble.
    sq = nt.nodes.new("ShaderNodeMath")
    sq.operation = "MULTIPLY"
    nt.links.new(dome.outputs["Distance"], sq.inputs[0])
    nt.links.new(dome.outputs["Distance"], sq.inputs[1])
    inv = nt.nodes.new("ShaderNodeMath")
    inv.operation = "MULTIPLY_ADD"
    inv.use_clamp = True
    inv.inputs[1].default_value = -4.0
    inv.inputs[2].default_value = 1.0
    nt.links.new(sq.outputs["Value"], inv.inputs[0])
    groove = nt.nodes.new("ShaderNodeMapRange")
    groove.interpolation_type = "SMOOTHSTEP"
    groove.inputs["From Min"].default_value = 0.0
    groove.inputs["From Max"].default_value = 0.12
    nt.links.new(edge.outputs["Distance"], groove.inputs["Value"])
    height = nt.nodes.new("ShaderNodeMath")
    height.operation = "MULTIPLY"
    nt.links.new(inv.outputs["Value"], height.inputs[0])
    nt.links.new(groove.outputs["Result"], height.inputs[1])
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 1.0
    bump.inputs["Distance"].default_value = NOSE_RELIEF
    nt.links.new(height.outputs["Value"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.nodes.active = tex
    dup.data.materials.clear()
    dup.data.materials.append(bake)

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    scene.render.bake.normal_space = "TANGENT"
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    dup.select_set(True)
    bpy.context.view_layer.objects.active = dup
    t0 = time.time()
    bpy.ops.object.bake(type="NORMAL", margin=16, use_clear=True)
    log(f"[nose] baked {NOSE_NORMAL_SIZE}^2 cobblestone normal map in {time.time() - t0:.1f}s")
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    mesh = dup.data
    bpy.data.objects.remove(dup)
    bpy.data.meshes.remove(mesh)
    bpy.data.materials.remove(bake)
    return img


def bake_nose_occlusion(ob, out_path, log=print):
    """Bake the nose leather's ambient occlusion (Cycles AO node, EMIT) into its UV square.

    Without it the nostrils were lit from inside: the wet film reflected the sky off their walls and
    the pair read as two glass beads. The engine applies a material's occlusion to the ambient
    light, the film's reflection of the environment included. Baked on a copy of the WHOLE body so
    the muzzle occludes too, with every other face's UVs collapsed outside the image, where the
    bake writes nothing; the AO node's only_local keeps the original body, coincident with the
    copy, out of the rays."""
    import bpy

    dup = ob.copy()
    dup.data = ob.data.copy()
    bpy.context.collection.objects.link(dup)
    me = dup.data
    idx = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("material_index", idx)
    start = np.zeros(len(me.polygons), dtype=np.int32)
    total = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("loop_start", start)
    me.polygons.foreach_get("loop_total", total)
    uv = np.zeros(len(me.loops) * 2, dtype=np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    for f in np.nonzero(idx != M_NOSE)[0]:
        uv[start[f]:start[f] + total[f]] = (-4.0, -4.0)
    me.uv_layers.active.data.foreach_set("uv", uv.ravel())
    me.polygons.foreach_set("material_index", np.zeros(len(me.polygons), dtype=np.int32))

    img = bpy.data.images.new("DogNoseOcclusion", NOSE_NORMAL_SIZE, NOSE_NORMAL_SIZE, alpha=False,
                              float_buffer=False)
    img.colorspace_settings.name = "Non-Color"
    bake = bpy.data.materials.new("NoseAOBake")
    nt = bake.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emit = nt.nodes.new("ShaderNodeEmission")
    ao = nt.nodes.new("ShaderNodeAmbientOcclusion")
    ao.samples = NOSE_AO_SAMPLES
    ao.only_local = True
    ao.inputs["Distance"].default_value = NOSE_AO_DISTANCE
    ao.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    nt.links.new(ao.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.nodes.active = tex
    me.materials.clear()
    me.materials.append(bake)

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 8
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    dup.select_set(True)
    bpy.context.view_layer.objects.active = dup
    t0 = time.time()
    bpy.ops.object.bake(type="EMIT", margin=16, use_clear=True)
    px = np.zeros(NOSE_NORMAL_SIZE * NOSE_NORMAL_SIZE * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    ao_px = px.reshape(-1, 4)[:, 0]
    log(f"[nose] baked {NOSE_NORMAL_SIZE}^2 occlusion in {time.time() - t0:.1f}s "
        f"(p5 {np.percentile(ao_px, 5):.2f}, median {np.median(ao_px):.2f})")
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    bpy.data.objects.remove(dup)
    bpy.data.meshes.remove(me)
    bpy.data.materials.remove(bake)
    return img


# ----------------------------------------------------------------------------
# The mouth's own maps (#1533). As flat glTF colour and roughness the tongue, gums, lips and teeth
# read wrong: the tongue as smooth pink plastic, and with no occlusion the wet film on the palate
# reflected the sky's sun as a white blaze. Each part is re-unwrapped into its own UV square after
# the coat map is baked (the pelt's islands are not touched), then gets
#   * a COLOUR map painted from its own surface (position and normal baked per texel, the pattern
#     evaluated in 3D so it is the same size whatever the UVs do): the tongue redder at the root
#     and the edges, paler at the tip, its median groove darker, the pale tips of the filiform
#     papillae over the dorsum, a purple, veined underside; the gums pink with black pigment;
#   * on the tongue, a NORMAL map of those papillae (Cycles, domes in object space);
#   * an OCCLUSION map from the whole head, baked in the PANT pose: the only clip that shows the
#     inside of the mouth, and a tongue baked inside the shut mouth would hang out dark.
# ----------------------------------------------------------------------------
MOUTH_TEX_SIZE = 1024
TONGUE_PAPILLA = 0.00055  # metres between the filiform papillae on the dorsum
TONGUE_RELIEF = 0.00010  # metres from a papilla's foot to its tip
MOUTH_AO_DISTANCE = 0.03  # metres: the whole oral cavity
MOUTH_AO_SAMPLES = 48
PANT_WIDEST_FRAME = 2  # Pant's jaw is 22 + 3 sin(8 pi t) degrees: widest at t = 1/16 s, frame 2 at 30 fps
MOUTH_PARTS = ((M_TONGUE, "Tongue"), (M_GUM, "Gum"), (M_LIP, "Lip"), (M_TEETH, "Teeth"))


def unwrap_material(ob, mat_index, label, angle=66.0, margin=0.01, log=print):
    """Re-unwrap one material's faces alone into the whole UV square, after the coat map is baked:
    they overlap the pelt's islands from then on, which only that material samples."""
    import bpy

    me = ob.data
    idx = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("material_index", idx)
    face_sel = idx == mat_index
    vert_sel = np.zeros(len(me.vertices), dtype=bool)
    loop_v = np.zeros(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", loop_v)
    start = np.zeros(len(me.polygons), dtype=np.int32)
    total = np.zeros(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("loop_start", start)
    me.polygons.foreach_get("loop_total", total)
    for f in np.nonzero(face_sel)[0]:
        vert_sel[loop_v[start[f]:start[f] + total[f]]] = True
    edge_v = np.zeros(len(me.edges) * 2, dtype=np.int32)
    me.edges.foreach_get("vertices", edge_v)
    edge_sel = vert_sel[edge_v.reshape(-1, 2)].all(axis=1)
    me.polygons.foreach_set("select", face_sel)
    me.vertices.foreach_set("select", vert_sel)
    me.edges.foreach_set("select", edge_sel)
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    with bpy.context.temp_override(active_object=ob, object=ob, edit_object=ob):
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(angle), island_margin=margin, area_weight=0.0,
                                 correct_aspect=True, scale_to_bounds=False)
        bpy.ops.object.mode_set(mode="OBJECT")
    log(f"[uv] {label}: {int(face_sel.sum())} faces into their own square")


def _bake_copy(ob, mat_index, whole, posed_from=None):
    """A temporary copy of the body for baking one material's faces. whole=False keeps only those
    faces; True keeps every face as an occluder with the others' UVs collapsed outside the image,
    where the bake writes nothing. posed_from: a mesh object in a pose whose vertex positions replace
    the copy's (the same topology and UVs), for a bake in that pose."""
    import bmesh
    import bpy

    dup = ob.copy()
    dup.data = ob.data.copy()
    dup.modifiers.clear()
    bpy.context.collection.objects.link(dup)
    me = dup.data
    if posed_from is not None:
        src = posed_from.data
        co = np.zeros(len(src.vertices) * 3, dtype=np.float64)
        src.vertices.foreach_get("co", co)
        me.vertices.foreach_set("co", co)
        me.update()
    if whole:
        idx = np.zeros(len(me.polygons), dtype=np.int32)
        me.polygons.foreach_get("material_index", idx)
        start = np.zeros(len(me.polygons), dtype=np.int32)
        total = np.zeros(len(me.polygons), dtype=np.int32)
        me.polygons.foreach_get("loop_start", start)
        me.polygons.foreach_get("loop_total", total)
        uv = np.zeros(len(me.loops) * 2, dtype=np.float32)
        me.uv_layers.active.data.foreach_get("uv", uv)
        uv = uv.reshape(-1, 2)
        for f in np.nonzero(idx != mat_index)[0]:
            uv[start[f]:start[f] + total[f]] = (-4.0, -4.0)
        me.uv_layers.active.data.foreach_set("uv", uv.ravel())
    else:
        bm = bmesh.new()
        bm.from_mesh(me)
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != mat_index], context="FACES")
        bm.to_mesh(me)
        bm.free()
    me.polygons.foreach_set("material_index", np.zeros(len(me.polygons), dtype=np.int32))
    return dup


def _bake_into(dup, name, size, build, bake_type, samples, float_buffer, colorspace, log):
    """Bake the shader `build(nt, out)` wires into a fresh material on `dup`, into a new image."""
    import bpy

    img = bpy.data.images.new(name, size, size, alpha=False, float_buffer=float_buffer)
    img.colorspace_settings.name = colorspace
    mat = bpy.data.materials.new(name + "Bake")
    nt = mat.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    build(nt, out)
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.nodes.active = tex
    dup.data.materials.clear()
    dup.data.materials.append(mat)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.render.bake.normal_space = "TANGENT"
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    dup.select_set(True)
    bpy.context.view_layer.objects.active = dup
    t0 = time.time()
    bpy.ops.object.bake(type=bake_type, margin=16, use_clear=True)
    log(f"[mouth] baked {name} ({bake_type}, {size}^2) in {time.time() - t0:.1f}s")
    bpy.data.materials.remove(mat)
    return img


def _emit_of(socket_name):
    """A shader that emits one Texture Coordinate output as its colour (a position or normal bake)."""

    def build(nt, out):
        emit = nt.nodes.new("ShaderNodeEmission")
        coord = nt.nodes.new("ShaderNodeTexCoord")
        nt.links.new(coord.outputs[socket_name], emit.inputs["Color"])
        nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])

    return build


def _emit_white(nt, out):
    emit = nt.nodes.new("ShaderNodeEmission")
    emit.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])


def _pixels(img):
    px = np.zeros(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(img.size[1], img.size[0], 4)


def _hash3(i, j, k, seed):
    """A hash of integer lattice coordinates to [0, 1]."""
    h = (i * 73856093) ^ (j * 19349663) ^ (k * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    return ((h ^ (h >> 16)) & 0xFFFFFF).astype(np.float64) / float(0xFFFFFF)


def _value_noise3(p, cell, seed):
    """Smooth value noise in [0, 1] over 3D points p (N, 3), one lattice cell `cell` metres wide."""
    q = p / cell
    i0 = np.floor(q).astype(np.int64)
    f = q - i0
    f = f * f * (3.0 - 2.0 * f)
    acc = np.zeros(len(p))
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = ((f[:, 0] if dx else 1.0 - f[:, 0]) * (f[:, 1] if dy else 1.0 - f[:, 1]) *
                     (f[:, 2] if dz else 1.0 - f[:, 2]))
                acc += w * _hash3(i0[:, 0] + dx, i0[:, 1] + dy, i0[:, 2] + dz, seed)
    return acc


def _cell_dots3(p, cell, seed):
    """Distance, in cells, to the nearest jittered lattice point, and that point's random value."""
    q = p / cell
    i0 = np.floor(q).astype(np.int64)
    best = np.full(len(p), 9.0)
    value = np.zeros(len(p))
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                ci = i0 + np.array((dx, dy, dz))
                jitter = np.stack([_hash3(ci[:, 0], ci[:, 1], ci[:, 2], seed + s) for s in range(3)], axis=1)
                d = np.linalg.norm(ci + 0.15 + 0.7 * jitter - q, axis=1)
                closer = d < best
                best = np.where(closer, d, best)
                value = np.where(closer, _hash3(ci[:, 0], ci[:, 1], ci[:, 2], seed + 7), value)
    return best, value


def _lerp_rows(a, b, t):
    return a + (b - a) * t[:, None]


def _to_srgb(rgb):
    rgb = np.maximum(rgb, 0.0)
    return np.clip(np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1.0 / 2.4) - 0.055), 0.0, 1.0)


def paint_tongue(p, n):
    """sRGB colours for tongue texels at rest-pose positions p with normals n (both (N, 3))."""
    lin = lambda c: _srgb_to_linear(np.array(c, dtype=np.float64))  # noqa: E731
    q = p - HEAD.astype(np.float64)
    along = np.clip((-0.060 - q[:, 1]) / 0.110, 0.0, 1.0)  # 0 at the root, 1 at the tip
    root, mid, tip = lin((0.66, 0.24, 0.28)), lin((0.80, 0.34, 0.38)), lin((0.86, 0.46, 0.49))
    dorsal = np.where((along < 0.5)[:, None], _lerp_rows(root, mid, along * 2.0),
                      _lerp_rows(mid, tip, along * 2.0 - 1.0))
    # The edges a little darker and redder, the median groove darker still toward the root.
    edge = np.clip((np.abs(q[:, 0]) - 0.012) / 0.007, 0.0, 1.0)
    dorsal *= (1.0 - 0.14 * edge)[:, None] * np.power(np.array((1.0, 0.86, 0.88))[None, :], edge[:, None])
    groove = np.exp(-(q[:, 0] / 0.0016) ** 2) * np.clip((0.95 - along) * 3.0, 0.0, 1.0)
    dorsal *= (1.0 - 0.22 * groove)[:, None]
    dorsal *= (1.0 + 0.10 * (_value_noise3(p, 0.004, 3) - 0.5))[:, None]
    # The filiform papillae's pale tips, on about half of them.
    dist, val = _cell_dots3(p, TONGUE_PAPILLA, 11)
    tips = np.clip(1.0 - dist / 0.35, 0.0, 1.0) * (val > 0.55)
    dorsal = dorsal + (lin((0.92, 0.70, 0.70)) - dorsal) * (0.30 * tips)[:, None]
    # The underside: deeper and bluer, with veins.
    under = np.tile(lin((0.60, 0.22, 0.32)), (len(p), 1))
    ridge = np.abs(_value_noise3(p, 0.009, 21) - 0.5) + 0.35 * np.abs(_value_noise3(p, 0.0035, 23) - 0.5)
    vein = np.clip(1.0 - ridge / 0.03, 0.0, 1.0)
    under = under * (1.0 - vein[:, None] * (1.0 - np.array((0.80, 0.68, 0.90))[None, :]))
    top = np.clip((n[:, 2] + 0.15) / 0.5, 0.0, 1.0)
    return _to_srgb(_lerp_rows(under, dorsal, top))


def paint_gum(p, n):
    """sRGB colours for gum texels: pink, with the black pigment patches a golden's gums carry."""
    lin = lambda c: _srgb_to_linear(np.array(c, dtype=np.float64))  # noqa: E731
    base = np.tile(lin((0.70, 0.36, 0.38)), (len(p), 1))
    base *= (1.0 + 0.12 * (_value_noise3(p, 0.003, 5) - 0.5))[:, None]
    pigment = np.clip((_value_noise3(p, 0.0045, 9) - 0.66) / 0.05, 0.0, 1.0)
    return _to_srgb(base + (lin((0.05, 0.035, 0.035)) - base) * pigment[:, None])


def _papilla_normal(nt, out):
    """The filiform papillae as domes in object space, through a bump into the BSDF's normal."""
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    coord = nt.nodes.new("ShaderNodeTexCoord")
    dome = nt.nodes.new("ShaderNodeTexVoronoi")
    dome.voronoi_dimensions = "3D"
    dome.feature = "F1"
    dome.inputs["Scale"].default_value = 1.0 / TONGUE_PAPILLA
    nt.links.new(coord.outputs["Object"], dome.inputs["Vector"])
    sq = nt.nodes.new("ShaderNodeMath")
    sq.operation = "MULTIPLY"
    nt.links.new(dome.outputs["Distance"], sq.inputs[0])
    nt.links.new(dome.outputs["Distance"], sq.inputs[1])
    inv = nt.nodes.new("ShaderNodeMath")
    inv.operation = "MULTIPLY_ADD"
    inv.use_clamp = True
    inv.inputs[1].default_value = -3.0
    inv.inputs[2].default_value = 1.0
    nt.links.new(sq.outputs["Value"], inv.inputs[0])
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 1.0
    bump.inputs["Distance"].default_value = TONGUE_RELIEF
    nt.links.new(inv.outputs["Value"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])


def _mouth_ao(nt, out):
    emit = nt.nodes.new("ShaderNodeEmission")
    ao = nt.nodes.new("ShaderNodeAmbientOcclusion")
    ao.samples = MOUTH_AO_SAMPLES
    ao.only_local = True
    ao.inputs["Distance"].default_value = MOUTH_AO_DISTANCE
    ao.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    nt.links.new(ao.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])


def _save_png(img, path):
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    return img


def bake_mouth(ob, arm, out_dir, log=print):
    """The mouth's maps (see the block comment above): {material index: {kind: image}}."""
    import bpy

    for mat_index, label in MOUTH_PARTS:
        unwrap_material(ob, mat_index, label.lower(), angle=60.0, margin=0.008, log=log)
    maps = {}
    size = MOUTH_TEX_SIZE
    # Colour, painted from the rest pose's surface.
    for mat_index, label, paint in ((M_TONGUE, "Tongue", paint_tongue), (M_GUM, "Gum", paint_gum)):
        dup = _bake_copy(ob, mat_index, whole=False)
        scratch = [f"Dog{label}Pos", f"Dog{label}Nrm", f"Dog{label}Cover"]
        pos = _pixels(_bake_into(dup, scratch[0], size, _emit_of("Object"), "EMIT", 1, True, "Non-Color", log))
        nrm = _pixels(_bake_into(dup, scratch[1], size, _emit_of("Normal"), "EMIT", 1, True, "Non-Color", log))
        cover = _pixels(_bake_into(dup, scratch[2], size, _emit_white, "EMIT", 1, True, "Non-Color", log))
        me = dup.data
        bpy.data.objects.remove(dup)
        bpy.data.meshes.remove(me)
        inside = cover[..., 0] > 0.5
        rgb = np.zeros((size, size, 3))
        rgb[inside] = paint(pos[..., :3][inside].astype(np.float64), nrm[..., :3][inside].astype(np.float64))
        img = bpy.data.images.new(f"Dog{label}Color", size, size, alpha=False, float_buffer=False)
        img.colorspace_settings.name = "sRGB"
        img.pixels.foreach_set(np.concatenate([rgb, np.ones((size, size, 1))], axis=2).astype(np.float32).ravel())
        maps.setdefault(mat_index, {})["color"] = _save_png(img, os.path.join(out_dir, f"Dog{label}Color.png"))
        for name in scratch:
            bpy.data.images.remove(bpy.data.images[name])
    # The papillae.
    dup = _bake_copy(ob, M_TONGUE, whole=False)
    normal = _bake_into(dup, "DogTongueNormal", size, _papilla_normal, "NORMAL", 1, False, "Non-Color", log)
    maps[M_TONGUE]["normal"] = _save_png(normal, os.path.join(out_dir, "DogTongueNormal.png"))
    me = dup.data
    bpy.data.objects.remove(dup)
    bpy.data.meshes.remove(me)
    # Occlusion, in the pant pose at its widest jaw.
    use_clip(arm, "Pant")
    scene = bpy.context.scene
    scene.frame_set(PANT_WIDEST_FRAME + 1)
    scene.frame_set(PANT_WIDEST_FRAME)
    posed = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    posed_mesh = bpy.data.meshes.new_from_object(posed)
    holder = bpy.data.objects.new("PantPose", posed_mesh)
    for mat_index, label in MOUTH_PARTS:
        dup = _bake_copy(ob, mat_index, whole=True, posed_from=holder)
        img = _bake_into(dup, f"Dog{label}Occlusion", size, _mouth_ao, "EMIT", 8, False, "Non-Color", log)
        ao_px = _pixels(img)[..., 0]
        lit = ao_px[ao_px > 0.0]
        if len(lit):
            log(f"[mouth] {label.lower()} occlusion: p5 {np.percentile(lit, 5):.2f}, median {np.median(lit):.2f}")
        maps.setdefault(mat_index, {})["occlusion"] = _save_png(img, os.path.join(out_dir, f"Dog{label}Occlusion.png"))
        me = dup.data
        bpy.data.objects.remove(dup)
        bpy.data.meshes.remove(me)
    bpy.data.objects.remove(holder)
    bpy.data.meshes.remove(posed_mesh)
    use_clip(arm, None)
    scene.frame_set(0)
    return maps


def _gltf_occlusion_group():
    """The glTF exporter's custom output group: an Occlusion socket it exports as occlusionTexture."""
    import bpy

    ng = bpy.data.node_groups.get("glTF Material Output")
    if ng is None:
        ng = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        ng.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
    return ng


MATERIAL_LOOKS = {  # sRGB base colour, roughness (the engine patches skin profiles over these)
    "DogNose": ((0.035, 0.030, 0.030), 0.30),
    "DogLip": ((0.090, 0.060, 0.055), 0.55),
    "DogGum": ((0.58, 0.26, 0.28), 0.40),
    "DogTongue": ((0.86, 0.38, 0.42), 0.30),
    "DogTeeth": ((0.93, 0.90, 0.80), 0.25),
    "DogPad": ((0.075, 0.065, 0.065), 0.80),
}


def build_materials(coat_img, nose_normal=None, nose_occlusion=None):
    """Principled materials under the contract names. DogSkin and DogLid sample the coat map; DogNose
    carries the cobblestone normal map and its occlusion."""
    import bpy

    for nm in MATERIALS:
        m = bpy.data.materials.get(nm) or bpy.data.materials.new(nm)
        nt = m.node_tree
        bsdf = nt.nodes.get("Principled BSDF")
        if nm in ("DogSkin", "DogLid"):
            tex = nt.nodes.new("ShaderNodeTexImage")
            tex.image = coat_img
            nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
            bsdf.inputs["Roughness"].default_value = 0.65
        else:
            rgb, rough = MATERIAL_LOOKS[nm]
            bsdf.inputs["Base Color"].default_value = (*_srgb_to_linear(rgb).tolist(), 1.0)
            bsdf.inputs["Roughness"].default_value = rough
            if nm == "DogNose" and nose_normal is not None:
                ntex = nt.nodes.new("ShaderNodeTexImage")
                ntex.image = nose_normal
                nmap = nt.nodes.new("ShaderNodeNormalMap")
                nmap.space = "TANGENT"
                nt.links.new(ntex.outputs["Color"], nmap.inputs["Color"])
                nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
            if nm == "DogNose" and nose_occlusion is not None:
                otex = nt.nodes.new("ShaderNodeTexImage")
                otex.image = nose_occlusion
                sep = nt.nodes.new("ShaderNodeSeparateColor")
                grp = nt.nodes.new("ShaderNodeGroup")
                grp.node_tree = _gltf_occlusion_group()
                nt.links.new(otex.outputs["Color"], sep.inputs["Color"])
                nt.links.new(sep.outputs["Red"], grp.inputs["Occlusion"])
        m.diffuse_color = (*_srgb_to_linear(MATERIAL_LOOKS.get(nm, (COAT_GOLD, 0))[0]).tolist(), 1.0)


def wire_mouth_materials(maps):
    """Point the mouth's materials at their maps (bake_mouth): colour, the tongue's normal map, and
    every part's occlusion through the glTF exporter's occlusion group, as DogNose's is."""
    import bpy

    for mat_index, kinds in maps.items():
        m = bpy.data.materials.get(MATERIALS[mat_index])
        nt = m.node_tree
        bsdf = nt.nodes.get("Principled BSDF")
        if "color" in kinds:
            ctex = nt.nodes.new("ShaderNodeTexImage")
            ctex.image = kinds["color"]
            nt.links.new(ctex.outputs["Color"], bsdf.inputs["Base Color"])
        if "normal" in kinds:
            ntex = nt.nodes.new("ShaderNodeTexImage")
            ntex.image = kinds["normal"]
            nmap = nt.nodes.new("ShaderNodeNormalMap")
            nmap.space = "TANGENT"
            nt.links.new(ntex.outputs["Color"], nmap.inputs["Color"])
            nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        if "occlusion" in kinds:
            otex = nt.nodes.new("ShaderNodeTexImage")
            otex.image = kinds["occlusion"]
            sep = nt.nodes.new("ShaderNodeSeparateColor")
            grp = nt.nodes.new("ShaderNodeGroup")
            grp.node_tree = _gltf_occlusion_group()
            nt.links.new(otex.outputs["Color"], sep.inputs["Color"])
            nt.links.new(sep.outputs["Red"], grp.inputs["Occlusion"])


# ----------------------------------------------------------------------------
# Clips (in place, 30 fps). Rotations about ARMATURE axes, so a bone roll can never flip a pose:
# +X is the dog's left (pitch), +Y its tail (roll about the body axis), +Z up (yaw).
# ----------------------------------------------------------------------------

FPS = 30


class Rig:
    def __init__(self, arm):
        from mathutils import Quaternion

        self.arm = arm
        self.pb = arm.pose.bones
        for b in self.pb:
            b.rotation_mode = "QUATERNION"
        self.rest = {b.name: b.bone.matrix_local.to_quaternion() for b in self.pb}
        self.Q = Quaternion
        self.pose = {}

    def reset(self):
        self.pose = {}

    def _entry(self, bone):
        from mathutils import Vector

        return self.pose.setdefault(bone, [self.Q((1, 0, 0, 0)), Vector((0, 0, 0))])

    def rot(self, bone, axis, degrees):
        """Rotate `bone` about an ARMATURE-space axis (right-hand rule) regardless of its roll."""
        from mathutils import Vector

        q_arm = self.Q(Vector(axis).normalized(), math.radians(degrees))
        r = self.rest[bone]
        e = self._entry(bone)
        e[0] = e[0] @ (r.inverted() @ q_arm @ r)

    def move(self, bone, offset):
        """Translate `bone` by an armature-space offset."""
        from mathutils import Vector

        self._entry(bone)[1] += self.rest[bone].inverted() @ Vector(offset)

    def bake(self, name, seconds, sampler, loop=True):
        """sampler(rig, t) fills the pose for phase t in [0, 1]; a looping clip's last key repeats its first."""
        from mathutils import Vector

        frames = max(2, int(round(seconds * FPS)))
        arm = self.arm
        arm.animation_data_create()
        act = bpy_data().actions.new(name)
        arm.animation_data.action = act
        self.slot = None
        touched = set()
        poses = []
        for f in range(frames + 1):
            self.reset()
            sampler(self, (f / frames) if loop else f / frames)
            poses.append(self.pose)
            touched |= set(self.pose)
        for f, pose in enumerate(poses):
            for nm in touched:
                q, loc = pose.get(nm, (self.Q((1, 0, 0, 0)), Vector((0, 0, 0))))
                b = self.pb[nm]
                b.rotation_quaternion = q
                b.location = loc
                b.keyframe_insert("rotation_quaternion", frame=f)
                b.keyframe_insert("location", frame=f)
        self.slot = arm.animation_data.action_slot
        track = arm.animation_data.nla_tracks.new()
        track.name = name
        track.strips.new(name, 0, act)
        arm.animation_data.action = None
        for b in self.pb:
            b.rotation_quaternion = (1, 0, 0, 0)
            b.location = (0, 0, 0)
        return act


def use_clip(arm, name):
    """Play one baked clip alone (every NLA track muted), or none: the rest pose, reset
    explicitly because nothing else would overwrite the last evaluated pose."""
    import bpy

    ad = arm.animation_data
    for tr in ad.nla_tracks:
        tr.mute = True
    if name:
        act = bpy.data.actions[name]
        ad.action = act
        if ad.action_slot is None and len(act.slots):
            ad.action_slot = act.slots[0]
    else:
        ad.action = None
        for pb in arm.pose.bones:
            pb.rotation_quaternion = (1, 0, 0, 0)
            pb.location = (0, 0, 0)


def bpy_data():
    import bpy

    return bpy.data


AX_X, AX_Y, AX_Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)
LID_CLOSE_UPPER = LID_UPPER_OPEN - LID_MEET  # degrees the upper lid travels to meet the lower
LID_CLOSE_LOWER = LID_MEET - LID_LOWER_OPEN


def _ease(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def _pulse(t, at, rise, hold=0.0, fall=None):
    """0 -> 1 -> 0 around phase `at` (rise, hold, fall as phase lengths)."""
    fall = rise if fall is None else fall
    if t < at - rise or t > at + hold + fall:
        return 0.0
    if t < at:
        return _ease((t - (at - rise)) / rise)
    if t <= at + hold:
        return 1.0
    return 1.0 - _ease((t - at - hold) / fall)


def blink(r, amount):
    """Both eyes; lids rotate about each eye's own lid axis until the margins meet. `axis` is
    up x gaze, so a POSITIVE turn about it carries up toward the gaze: the upper lid closes on +,
    the lower on -. With the signs the other way round every blink opened the lids to the whole
    globe, and the eyes popped out twice every Idle loop."""
    for s, side in ((1.0, "L"), (-1.0, "R")):
        _c, _f, _up, axis = eye_frame(s)
        r.rot(f"lid_upper_{side}", tuple(axis), LID_CLOSE_UPPER * amount)
        r.rot(f"lid_lower_{side}", tuple(axis), -LID_CLOSE_LOWER * amount)


def brows(r, raise_amount, worry=0.0):
    for s, side in ((1.0, "L"), (-1.0, "R")):
        r.move(f"brow_{side}", (s * -0.0012 * worry, -0.0015 * raise_amount, 0.0045 * raise_amount))
        r.rot(f"brow_{side}", AX_Y, s * 10.0 * worry)


def tail_wag(r, t, cycles, amplitude, lift=0.0, travel=0.55):
    for i in range(1, 7):
        w = 0.55 + 0.1 * i
        r.rot(f"tail_{i:02d}", AX_Z, amplitude * w * math.sin(2 * math.pi * cycles * t - travel * i))
        if lift:
            r.rot(f"tail_{i:02d}", AX_X, -lift * (0.3 + 0.12 * i))


def breathe(r, t, cycles, depth=1.0):
    s = math.sin(2 * math.pi * cycles * t)
    r.rot("spine_02", AX_X, 0.7 * depth * s)
    r.rot("spine_03", AX_X, -0.9 * depth * s)
    r.move("spine_03", (0.0, 0.0, 0.0018 * depth * s))


def clip_idle(r, t):
    """4 s loop: breathing, two blinks, an ear twitch, a gentle wag, a small head drift."""
    breathe(r, t, 3)
    r.rot("neck_02", AX_X, -1.2 * math.sin(2 * math.pi * t + 0.7))
    r.rot("head", AX_Z, 5.0 * math.sin(2 * math.pi * t + 1.0))
    r.rot("head", AX_Y, 2.0 * math.sin(2 * math.pi * t + 2.1))
    blink(r, max(_pulse(t, 0.27, 0.025, 0.012, 0.035), _pulse(t, 0.78, 0.025, 0.012, 0.035)))
    tw = _pulse(t, 0.55, 0.03, 0.02, 0.08)
    r.rot("ear_L_01", AX_Y, 16.0 * tw)
    r.rot("ear_L_02", AX_Y, 8.0 * tw)
    brows(r, 0.25 * _pulse(t, 0.42, 0.08, 0.1, 0.12))
    tail_wag(r, t, 4, 9.0, lift=4.0)


def clip_head_tilt(r, t):
    """2.5 s: the head rolls ~20 degrees with raised, worried brows; the ears flop with it; back."""
    k = _ease(t / 0.28) * (1.0 - _ease((t - 0.72) / 0.28))
    breathe(r, t, 2, 0.8)
    r.rot("neck_02", AX_Y, -6.0 * k)
    r.rot("head", AX_Y, -16.0 * k)
    r.rot("head", AX_X, -5.0 * k)
    r.rot("ear_L_01", AX_Y, -10.0 * k)
    r.rot("ear_R_01", AX_Y, -6.0 * k)
    brows(r, 0.9 * k, worry=0.7 * k)
    blink(r, _pulse(t, 0.5, 0.03, 0.01, 0.04))
    tail_wag(r, t, 3, 6.0 * (1.0 - 0.5 * k), lift=3.0)


def clip_sit(r, t):
    """3 s, holds: the pelvis pitches nose-up and drops onto the rump, the hind legs fold under
    with the metatarsals flat, the forelegs stay vertical and planted. A NEGATIVE rotation about
    +X pitches a bone's forward end up."""
    k = _ease(t / 0.75)
    breathe(r, t, 3, 0.8)
    r.move("pelvis", (0.0, SIT_PELVIS_BACK * k, -SIT_PELVIS_DROP * k))
    r.rot("pelvis", AX_X, -SIT_PELVIS_PITCH * k)
    for sp in ("spine_01", "spine_02", "spine_03"):
        r.rot(sp, AX_X, SIT_SPINE_COUNTER * k)
    chest = SIT_PELVIS_PITCH - 3.0 * SIT_SPINE_COUNTER  # the chest's net nose-up pitch
    r.rot("neck_01", AX_X, 0.45 * chest * k)
    r.rot("neck_02", AX_X, 0.25 * chest * k)
    r.rot("head", AX_X, 0.15 * chest * k)
    for side in ("L", "R"):
        r.rot(f"upperarm_{side}", AX_X, chest * k)  # the foreleg back to vertical
        r.rot(f"thigh_{side}", AX_X, SIT_THIGH * k)
        r.rot(f"shin_{side}", AX_X, SIT_SHIN * k)
        r.rot(f"hock_{side}", AX_X, SIT_HOCK * k)
        r.rot(f"paw_rear_{side}", AX_X, SIT_TOES * k)
    for i in range(1, 7):
        r.rot(f"tail_{i:02d}", AX_Z, 6.0 * k * (0.5 + 0.1 * i))  # the tail curls round to one side
    brows(r, 0.3 * k)
    blink(r, _pulse(t, 0.86, 0.02, 0.01, 0.03))
    tail_wag(r, t, 2, 4.0 * k)


SIT_PELVIS_PITCH = 38.0
SIT_SPINE_COUNTER = 4.0
SIT_PELVIS_DROP = 0.20
SIT_PELVIS_BACK = 0.02
SIT_THIGH = -25.0
SIT_SHIN = 70.0
SIT_HOCK = -81.0
SIT_TOES = 47.0


def clip_walk(r, t):
    """~1 s four-beat walk: LH, LF, RH, RF a quarter stride apart; head bob, tail sway."""
    duty = 0.62
    offsets = {("L", "h"): 0.0, ("L", "f"): 0.75, ("R", "h"): 0.5, ("R", "f"): 0.25}
    for (side, kind), off in offsets.items():
        ph = (t + off) % 1.0
        swing = 20.0 if kind == "f" else 18.0
        if ph < duty:
            u = ph / duty
            ang = swing * (1.0 - 2.0 * u)
            lift = 0.0
        else:
            u = (ph - duty) / (1.0 - duty)
            ang = -swing + 2.0 * swing * _ease(u)
            lift = math.sin(math.pi * u)
        # forward swing = the paw moves toward -Y: a leg bone pointing down rotates NEGATIVELY about +X
        # Through the stance the paw counter-rotates by the leg's swing, so the pad stays
        # flat on the ground instead of rocking its toes into it.
        plant = (1.0 - lift) * ang
        if kind == "f":
            r.rot(f"upperarm_{side}", AX_X, -ang)
            r.rot(f"forearm_{side}", AX_X, 50.0 * lift)
            r.rot(f"paw_front_{side}", AX_X, 30.0 * lift + plant)
        else:
            r.rot(f"thigh_{side}", AX_X, -ang)
            r.rot(f"shin_{side}", AX_X, -28.0 * lift)
            r.rot(f"hock_{side}", AX_X, 40.0 * lift)
            r.rot(f"paw_rear_{side}", AX_X, -30.0 * lift + plant)
    bob = math.sin(4.0 * math.pi * t)
    r.move("pelvis", (0.0, 0.0, 0.005 * bob))
    r.rot("spine_02", AX_Z, 2.0 * math.sin(2 * math.pi * t))
    r.rot("neck_02", AX_X, 2.5 * bob)
    r.rot("head", AX_X, -2.0 * bob)
    for side in ("L", "R"):
        r.rot(f"ear_{side}_01", AX_X, 5.0 * math.sin(4.0 * math.pi * t + 0.8))
    tail_wag(r, t, 1, 12.0, lift=6.0)


def clip_pant(r, t):
    """2 s loop: the mouth open, the tongue out over the lower lip, the chest heaving, one blink."""
    s = math.sin(2 * math.pi * 4 * t)
    r.rot("jaw", AX_X, 22.0 + 3.0 * s)
    # The tongue SLIDES out 45 mm and lifts 10 mm, so tongue_03's bend pivot lands in front of the
    # lower incisors: the tongue lies over them and hangs past the lip from there. A bend pivot
    # behind the incisors drives the tongue down through them (they stand 5.5 mm clear of the gum,
    # _teeth_layout), and bending in place drives the tip through the chin. At 30 fps the closest
    # frame leaves 1.7 mm over the incisors, 2.5 mm over the canines and 4.6 mm over the lower lip;
    # DogShowcaseEvidenceTest's TheTeethStayOutOfTheTongueThroughEveryClip holds every clip to it.
    r.move("tongue_01", (0.0, -0.045, 0.010))
    r.rot("tongue_01", AX_X, -6.0)
    r.rot("tongue_02", AX_X, 6.0 + 2.0 * s)
    r.rot("tongue_03", AX_X, 35.0 + 5.0 * s)
    breathe(r, t, 4, 2.2)
    brows(r, 0.35 + 0.1 * s)
    blink(r, _pulse(t, 0.62, 0.03, 0.01, 0.04))
    tail_wag(r, t, 3, 16.0, lift=8.0)


def clip_rest(r, t):
    """1 s, the rest pose held: every bone at its bind transform. Keyed on the root alone (an action
    with no key exports nothing). What look development and the evidence judge a face in, with no
    head turn or blink in the way."""
    r.rot("root", AX_X, 0.0)


CLIPS = [("Idle", 4.0, clip_idle, True), ("HeadTilt", 2.5, clip_head_tilt, False), ("Sit", 3.0, clip_sit, False),
         ("Walk", 1.0, clip_walk, True), ("Pant", 2.0, clip_pant, True), ("Rest", 1.0, clip_rest, True)]


def bake_clips(arm, log=print):
    import bpy

    # The exporter turns frames into seconds with the SCENE's rate; left at Blender's 24 every
    # clip came out 1.25 times too long.
    bpy.context.scene.render.fps = FPS
    bpy.context.scene.render.fps_base = 1.0
    r = Rig(arm)
    for name, seconds, sampler, loop in CLIPS:
        r.bake(name, seconds, sampler, loop)
        log(f"[clips] {name}: {seconds:.1f} s ({'loop' if loop else 'once'})")


# ----------------------------------------------------------------------------
# Export and the rig description
# ----------------------------------------------------------------------------


def to_gltf(v):
    """Blender (x, y, z), Z up, dog facing -Y  ->  glTF (x, z, -y), Y up, dog facing +Z."""
    v = np.asarray(v, dtype=np.float64)
    return [float(v[0]), float(v[2]), float(-v[1])]


def write_rig_json(path):
    import json

    eyes = {}
    for s, side in ((1.0, "L"), (-1.0, "R")):
        c, f, up, axis = eye_frame(s)
        eyes[side] = {"bone": f"eye_{side}", "centre": to_gltf(c), "gaze": to_gltf(f), "up": to_gltf(up)}
    data = {
        "units": "metres, glTF Y-up, the head faces +Z",
        "eyeRadius": float(EYE_R),
        "lidInnerRadius": float(EYE_R + LID_GAP),
        "socketRadius": float(EYE_R + SOCKET_CLEAR),
        "eyes": eyes,
        "clips": [{"name": n, "seconds": s, "loop": lp} for n, s, _f, lp in CLIPS],
        "materials": MATERIALS,
    }
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")


# The painted iris (Ole's review, #1533). The engine's ocular model (DogEye.oloskin) refracts the
# view into the eye and draws the pupil and the limbal ring on the iris plane, but its iris is one
# flat colour: a dark disc with a hole in it read as a glass bead. The iris's structure is painted
# into the globe's albedo instead, which the model multiplies by its tint inside the iris, and the
# material stages FETCH it at the refracted iris point (oloSkinOcularIrisShift), so the painted
# pattern moves behind the cornea with the pupil the model draws. Sizes are the profile's, as
# fractions of the globe radius: the limbus at IrisRadiusMM / EyeRadiusMM, the pupil's edge at
# PupilRadiusMM / EyeRadiusMM. Because the fetch is refracted, those fractions are positions on the
# IRIS PLANE, the same coordinates the model's pupil and ring use.
#
# A golden retriever's iris is a dark, warm brown (the second review: the amber first version read
# as a toy's eye), lighter and finer-grained inside the collarette, with a crisp black pupil. The
# pupil is painted too: the texture's edge is mip-filtered, so it stays sharp at a close-up without
# crawling, and the model's own soft pupil band then reads as the pigment ruff just outside it.
IRIS_TEX_SIZE = 2048
IRIS_LIMBUS = 9.6 / 12.0
IRIS_PUPIL = 4.3 / 12.0
IRIS_SCLERA_LINEAR = (0.78, 0.74, 0.71)  # the globe's albedo outside the iris, warm: the eye's old base colour
# sRGB: a warm brown in the pupillary zone, a darker chestnut across the ciliary zone, near-black at the limbus
IRIS_PUPILLARY = (0.40, 0.235, 0.095)
IRIS_CILIARY = (0.30, 0.165, 0.065)
IRIS_OUTER = (0.13, 0.065, 0.027)
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


def export_eyeball(out_dir, log=print):
    """DogEyeball.gltf: a unit sphere at film-close-up tessellation, with the painted iris
    (DogIrisColor.png -- "Color" in the name, or the asset loader decodes it as linear data) on a
    planar UV across the gaze. The scene draws each eye as this mesh on its own
    entity (the engine's primitive sphere reloads from a scene file at 16 segments, a polygon
    silhouette at a face close-up). The engine takes the gaze as the entity's +Z: glTF +Z, which is
    Blender's -Y."""
    import bpy

    bpy.ops.mesh.primitive_uv_sphere_add(segments=128, ring_count=64, radius=1.0, location=(0.0, 0.0, 0.0))
    ob = bpy.context.active_object
    ob.name = "DogEyeball"
    ob.data.name = "DogEyeball"
    ob.data.shade_smooth()
    me = ob.data
    loop_v = np.zeros(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", loop_v)
    co = np.zeros(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)[loop_v]
    # Planar across the gaze, looking down it (from glTF +Z, the viewer's side): u follows glTF +X
    # (Blender +X), v follows glTF +Y (Blender +Z).
    uv = np.stack([0.5 + 0.5 * co[:, 0], 0.5 + 0.5 * co[:, 2]], axis=1).astype(np.float32)
    me.uv_layers.active.data.foreach_set("uv", uv.ravel())
    px = iris_albedo()
    img = bpy.data.images.new("DogIrisColor", IRIS_TEX_SIZE, IRIS_TEX_SIZE, alpha=False, float_buffer=False)
    img.colorspace_settings.name = "sRGB"
    rgba = np.concatenate([px, np.ones(px.shape[:2] + (1,), dtype=px.dtype)], axis=2).astype(np.float32)
    img.pixels.foreach_set(rgba.ravel())
    img.filepath_raw = os.path.join(out_dir, "DogIrisColor.png")
    img.file_format = "PNG"
    img.save()
    mat = bpy.data.materials.new("DogEyeball")
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.5
    me.materials.append(mat)
    for o in bpy.context.view_layer.objects:
        o.select_set(o is ob)
    path = os.path.join(out_dir, "DogEyeball.gltf")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True,
                              export_animations=False, export_skins=False, export_materials="EXPORT",
                              export_image_format="AUTO", export_texcoords=True, export_normals=True)
    bpy.data.objects.remove(ob)
    bpy.data.meshes.remove(me)
    log(f"[export] wrote {path}")


def export_gltf(ob, arm, out_dir, log=print):
    import bpy

    for o in bpy.context.view_layer.objects:
        o.select_set(o in (ob, arm))
    bpy.context.view_layer.objects.active = arm
    path = os.path.join(out_dir, "Dog.gltf")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True,
                              export_animations=True, export_animation_mode="NLA_TRACKS", export_skins=True,
                              export_def_bones=False, export_image_format="AUTO", export_apply=False,
                              export_texcoords=True, export_normals=True, export_tangents=False,
                              export_materials="EXPORT", export_morph=False)
    log(f"[export] wrote {path}")
    return path


# ----------------------------------------------------------------------------
# Preview renders (Workbench), with stand-in eyeballs: the engine draws the real eyes
# ----------------------------------------------------------------------------


def render_previews(body, arm, out_dir, log=print):
    import bpy
    from mathutils import Matrix, Vector

    os.makedirs(out_dir, exist_ok=True)
    scene = bpy.context.scene
    mats = []
    for nm, rgb in (("PrevSclera", (0.85, 0.82, 0.78)), ("PrevIris", (0.22, 0.10, 0.03)), ("PrevPupil", (0.01, 0.01, 0.01))):
        m = bpy.data.materials.new(nm)
        m.diffuse_color = (*rgb, 1.0)
        mats.append(m)
    eyes = []
    for s, side in ((1.0, "L"), (-1.0, "R")):
        c, f, _up, _axis = eye_frame(s)
        bpy.ops.mesh.primitive_uv_sphere_add(segments=96, ring_count=48, radius=float(EYE_R), location=tuple(c))
        e = bpy.context.active_object
        for m in mats:
            e.data.materials.append(m)
        fv = Vector(tuple(f))
        for poly in e.data.polygons:
            d = poly.center.normalized().dot(fv)
            poly.material_index = 2 if d > 0.93 else (1 if d > 0.72 else 0)
        e.data.shade_smooth()
        e.parent = arm
        e.parent_type = "BONE"
        e.parent_bone = f"eye_{side}"
        e.matrix_world = Matrix.Translation(Vector(tuple(c)))
        eyes.append(e)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.display.shading.show_specular_highlight = True
    scene.render.resolution_x = 1100
    scene.render.resolution_y = 1100
    scene.world = bpy.data.worlds.new("PreviewWorld")
    cam_data = bpy.data.cameras.new("PreviewCam")
    cam = bpy.data.objects.new("PreviewCam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    H = Vector(HEAD.tolist())

    def shoot(name, loc, target, lens):
        cam_data.lens = lens
        cam.location = loc
        cam.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(out_dir, name + ".png")
        bpy.ops.render.render(write_still=True)

    def at(clip, frame):
        use_clip(arm, clip)
        scene.frame_set(frame + 1)
        scene.frame_set(frame)

    at(None, 0)
    shoot("rest_face34", H + Vector((0.30, -0.62, 0.10)), H + Vector((0, -0.08, -0.01)), 70)
    shoot("rest_front", H + Vector((0.0, -0.70, 0.04)), H + Vector((0, -0.08, -0.01)), 70)
    shoot("rest_profile", H + Vector((0.62, -0.10, 0.02)), H + Vector((0, -0.08, -0.02)), 70)
    shoot("rest_body34", Vector((1.10, -1.20, 0.75)), Vector((0.0, -0.05, 0.32)), 50)
    shoot("rest_side", Vector((1.70, -0.05, 0.40)), Vector((0.0, -0.05, 0.34)), 50)
    shoot("rest_rear34", Vector((-0.9, 1.25, 0.70)), Vector((0.0, 0.05, 0.34)), 50)
    shoot("rest_low_hero", Vector((0.55, -1.05, 0.12)), Vector((0.0, -0.12, 0.40)), 40)
    for name, seconds, _fn, _loop in CLIPS:
        frames = int(round(seconds * FPS))
        for i, f in enumerate((0, frames // 4, frames // 2, (3 * frames) // 4, frames)):
            at(name, f)
            if name in ("Walk", "Sit"):
                shoot(f"clip_{name}_{i}", Vector((1.60, -0.35, 0.45)), Vector((0.0, -0.05, 0.28)), 50)
            else:
                shoot(f"clip_{name}_{i}", H + Vector((0.34, -0.66, 0.08)), H + Vector((0, -0.06, -0.03)), 60)
    at("Idle", int(round(0.27 * 4.0 * FPS)))
    shoot("blink_closed", H + Vector((0.30, -0.62, 0.10)), H + Vector((0, -0.08, -0.01)), 70)  # rest_face34's camera
    at("Pant", 10)
    shoot("pant_mouth", H + Vector((0.20, -0.42, -0.06)), H + Vector((0, -0.15, -0.085)), 85)
    for e in eyes:
        bpy.data.objects.remove(e)
    bpy.data.objects.remove(cam)
    at(None, 0)
    log(f"[preview] wrote renders to {out_dir}")


# ----------------------------------------------------------------------------
# Entry point
# ----------------------------------------------------------------------------


def main():
    """blender -b --factory-startup --python build_dog.py -- [out_dir] [preview_dir]

    Writes Dog.gltf + Dog.bin + DogCoatColor.png + DogNoseNormal.png + DogNoseOcclusion.png +
    Dog.rig.json into out_dir, then the coat (build_dog_groom.py) into .dog-groom/Dog.abc (default: this
    script's directory) and, when preview_dir is given, Workbench renders of the rest pose,
    every clip, a blink and the pant."""
    import bpy

    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_dir = os.path.abspath(argv[0]) if argv else os.path.dirname(os.path.abspath(__file__))
    preview_dir = os.path.abspath(argv[1]) if len(argv) > 1 else ""
    t0 = time.time()

    def log(msg):
        print(f"[{time.time() - t0:7.1f}s] {msg}", flush=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    body = build_body_mesh(log=log)
    arm = build_armature()
    names = [b.name for b in arm.data.bones]
    co_b, tri_b = _mesh_arrays(body.data)
    W_body, _ = heat_weights(body, arm, NON_DEFORM, log=log)
    W_body = body_weights(co_b, tri_b, W_body, names, log=log)
    W = assemble(body, W_body, names, log=log)
    unwrap(body, log=log)
    img = bake_coat_map(body, os.path.join(out_dir, "DogCoatColor.png"), log=log)
    unwrap_nose(body, log=log)
    nose = bake_nose_normal(body, os.path.join(out_dir, "DogNoseNormal.png"), log=log)
    nose_ao = bake_nose_occlusion(body, os.path.join(out_dir, "DogNoseOcclusion.png"), log=log)
    reshape_teeth(body, log=log)
    build_materials(img, nose, nose_ao)
    write_weights(body, arm, W, names, log=log)
    bake_clips(arm, log=log)
    wire_mouth_materials(bake_mouth(body, arm, out_dir, log=log))
    use_clip(arm, None)
    export_gltf(body, arm, out_dir, log=log)
    export_eyeball(out_dir, log=log)
    write_rig_json(os.path.join(out_dir, "Dog.rig.json"))
    if preview_dir:
        render_previews(body, arm, preview_dir, log=log)
    # The coat, grown on the dog just written (build_dog_groom.py, which resets the scene).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_dog_groom

    build_dog_groom.build(out_dir, build_dog_groom.default_abc_path(), log=log)
    log("done")


if __name__ == "__main__":
    main()
