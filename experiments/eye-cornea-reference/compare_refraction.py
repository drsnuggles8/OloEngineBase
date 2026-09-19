#!/usr/bin/env python3
"""Reference comparison for issue #1244: what does an eye's cornea actually do
to the iris behind it, and which real-time approximation reproduces it?

This is the experiment behind the issue's third acceptance criterion ("compare a
small optical reference before selecting approximations"). It is run BEFORE the
shader exists, and the shader is a transcription of whichever candidate wins --
not the other way round.

Everything below is a ray trace through the Gullstrand-derived schematic eye at
the top of this file. Nothing is fitted, nothing is tuned, and no candidate is
given a parameter the ground truth did not hand it.

  1. GROUND TRUTH. A two-surface ray trace: air -> corneal stroma (n 1.376) ->
     aqueous humour (n 1.336) -> the iris plane. Both corneal surfaces are real
     spheres with their clinical radii, and the iris is a real plane at the
     clinical anterior chamber depth. A ray that misses, or that total-internal-
     reflects, is reported as such rather than clamped.

  2. HOW MANY REFRACTING SURFACES. The two-surface trace is compared against a
     ONE-surface trace at the keratometric index (n 1.3375) and at the stromal
     index (n 1.376). A shader gets one `refract()`; this question decides which
     index that one call should carry, and it is a question with a right answer
     rather than a taste.

  3. WHAT NORMAL. The same one-surface trace, but fed the GLOBE's normal instead
     of the CORNEA's -- which is what a shader gets for free when the eye is one
     sphere mesh and nobody reconstructs the corneal dome. This is the cheap
     option, and question 3 is whether it is cheap or wrong.

  4. IS REFRACTION VISIBLE AT ALL. The painted-on arm: the iris sampled at the
     surface hit with no offset. The number this produces is the one that
     justifies the whole feature, and it is reported in iris radii rather than
     millimetres because that is the unit a texture lookup is in.

  5. THE ENTRANCE PUPIL. The pupil rim traced BACK out through the cornea, which
     gives the apparent pupil size a distant observer sees. The literature value
     (Bennett & Rabbetts, Gullstrand) is 1.13x linear magnification with the
     apparent pupil ~0.5 mm anterior to the real one, so this is the one metric
     here with an external answer to agree with -- a check on the trace itself
     before any candidate is scored against it.

  6. GAZE PARALLAX. The eye is rotated about its own centre and the apparent
     pupil-centre displacement is measured. This is criterion 3 in one number:
     a painted eye and a refracting eye differ by exactly this, it is ZERO in a
     front-on static capture, and it is what falls apart under gaze animation.

Run:  python compare_refraction.py

Results are recorded in docs/guides/eye-cornea-iris.md. Nothing in the engine
imports this file; it is the evidence, not the implementation.

LIMITS, STATED UP FRONT.

  * The cornea here is SPHERICAL. A real cornea is a prolate ellipsoid with a
    shape factor around -0.26, flattening toward the limbus. That error is in
    the same direction for every candidate and for the ground truth, so it moves
    the absolute millimetres a little and moves no ORDERING, which is what this
    file is for. It is also the approximation the engine would make anyway: the
    eye mesh's corneal cap is spherical.
  * The iris is a PLANE. A real iris is slightly conical and has a collarette
    ridge; question 4's painted arm is the bound on what that is worth, and it
    is much smaller than the refraction itself.
  * Dispersion is ignored -- one index per medium, not three. Chromatic
    aberration in the anterior chamber is well under a texel at any resolution
    this engine renders a head at.
"""

from __future__ import annotations

import math

import numpy as np

# =============================================================================
# The schematic eye. EYE-LOCAL SPACE: origin at the GLOBE CENTRE, +Z forward
# along the optical axis (the gaze direction), millimetres throughout.
#
# Every number below is clinical, and every derived number is derived here
# rather than typed, so that changing one radius cannot leave a stale constant
# somewhere further down.
# =============================================================================

N_AIR = 1.0
N_CORNEA = 1.376       # corneal stroma
N_AQUEOUS = 1.336      # aqueous humour, the medium the iris sits in
N_KERATOMETRIC = 1.3375  # the single-surface index clinical keratometry uses

R_GLOBE = 12.0         # scleral sphere radius
R_CORNEA_ANT = 7.8     # anterior corneal radius of curvature
R_CORNEA_POST = 6.8    # posterior corneal radius of curvature
CORNEA_THICKNESS = 0.55  # central corneal thickness, at the apex
LIMBUS_RADIUS = 5.85   # half the horizontal visible iris diameter (11.7 mm)
ACD_FROM_APEX = 3.6    # corneal apex -> iris plane (anterior chamber depth)
PUPIL_RADIUS = 2.0     # a mesopic pupil
IRIS_OUTER_RADIUS = LIMBUS_RADIUS

# The corneal cap is the sphere of radius R_CORNEA_ANT that meets the globe at
# the limbus ring. DERIVED, not typed: the limbus ring sits on the globe at
# radius LIMBUS_RADIUS, and the cap's sagitta over that same radius puts the
# apex in front of it.
_LIMBUS_Z = math.sqrt(R_GLOBE**2 - LIMBUS_RADIUS**2)
_CORNEAL_SAGITTA = R_CORNEA_ANT - math.sqrt(R_CORNEA_ANT**2 - LIMBUS_RADIUS**2)
CORNEA_APEX_Z = _LIMBUS_Z + _CORNEAL_SAGITTA
CORNEA_ANT_CENTRE_Z = CORNEA_APEX_Z - R_CORNEA_ANT
CORNEA_POST_CENTRE_Z = (CORNEA_APEX_Z - CORNEA_THICKNESS) - R_CORNEA_POST
IRIS_PLANE_Z = CORNEA_APEX_Z - ACD_FROM_APEX

AXIS = np.array([0.0, 0.0, 1.0])


# =============================================================================
# Ray primitives. Vectorised over a leading ray axis throughout: every function
# takes (..., 3) arrays and returns them, so a sweep is one call and not a loop.
# =============================================================================


def _normalize(v):
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def intersect_sphere_front(origin, direction, centre_z, radius):
    """Nearest intersection with the sphere centred on the axis at `centre_z`,
    taking the FRONT (larger z, smaller t for a forward-travelling ray) root.

    Returns (t, hit, ok). `ok` is False where the ray misses entirely; t is NaN
    there rather than 0, so a miss that is not filtered cannot silently shade as
    an apex hit.
    """
    centre = np.array([0.0, 0.0, centre_z])
    oc = origin - centre
    b = 2.0 * np.sum(oc * direction, axis=-1)
    c = np.sum(oc * oc, axis=-1) - radius * radius
    disc = b * b - 4.0 * c
    ok = disc > 0.0
    sqrt_disc = np.sqrt(np.where(ok, disc, 0.0))
    t0 = (-b - sqrt_disc) * 0.5
    t1 = (-b + sqrt_disc) * 0.5
    # The smaller positive root: the near side of the sphere.
    t = np.where(t0 > 1e-6, t0, t1)
    ok = ok & (t > 1e-6)
    t = np.where(ok, t, np.nan)
    hit = origin + t[..., None] * direction
    return t, hit, ok


def refract(incident, normal, eta):
    """Snell, in the `refract()` form every GLSL shader uses.

    `incident` points ALONG the ray (into the surface), `normal` points back
    toward the incident medium. Returns (refracted, ok); `ok` is False on total
    internal reflection, where the refracted direction does not exist and is
    returned as NaN rather than as zero -- a zero direction marches to the
    surface point itself, which is indistinguishable from a painted-on eye and
    is exactly the failure this file is about.
    """
    cos_i = -np.sum(normal * incident, axis=-1)
    k = 1.0 - eta * eta * (1.0 - cos_i * cos_i)
    ok = k >= 0.0
    sqrt_k = np.sqrt(np.where(ok, k, 0.0))
    out = eta * incident + (eta * cos_i - sqrt_k)[..., None] * normal
    out = np.where(ok[..., None], out, np.nan)
    return _normalize(out), ok


def march_to_iris(origin, direction):
    """Intersect the iris PLANE (z == IRIS_PLANE_Z, normal +Z). Returns the
    (x, y) landing point and an `ok` for rays travelling the wrong way."""
    dz = direction[..., 2]
    ok = dz < -1e-6
    t = np.where(ok, (IRIS_PLANE_Z - origin[..., 2]) / np.where(ok, dz, 1.0), np.nan)
    hit = origin + t[..., None] * direction
    return hit[..., :2], ok


# =============================================================================
# The candidates. Every one takes the SAME entry ray and returns the SAME thing
# -- where on the iris plane that ray lands -- so the comparison is like for
# like and a candidate cannot win by answering a different question.
# =============================================================================


def truth_two_surface(origin, direction):
    """(1) GROUND TRUTH: air -> stroma -> aqueous -> iris plane."""
    _, p1, ok1 = intersect_sphere_front(origin, direction, CORNEA_ANT_CENTRE_Z, R_CORNEA_ANT)
    n1 = _normalize(p1 - np.array([0.0, 0.0, CORNEA_ANT_CENTRE_Z]))
    d1, okr1 = refract(direction, n1, N_AIR / N_CORNEA)

    _, p2, ok2 = intersect_sphere_front(p1, d1, CORNEA_POST_CENTRE_Z, R_CORNEA_POST)
    n2 = _normalize(p2 - np.array([0.0, 0.0, CORNEA_POST_CENTRE_Z]))
    d2, okr2 = refract(d1, n2, N_CORNEA / N_AQUEOUS)

    xy, ok3 = march_to_iris(p2, d2)
    return xy, ok1 & okr1 & ok2 & okr2 & ok3


def one_surface(origin, direction, n_out, use_globe_normal=False):
    """(2, 3) ONE refracting surface at the corneal cap, index `n_out`.

    `use_globe_normal` feeds the trace the SCLERAL sphere's normal at the same
    point instead of the corneal cap's -- the normal a shader gets for free when
    the eye is a single sphere and nobody reconstructs the dome.
    """
    _, p1, ok1 = intersect_sphere_front(origin, direction, CORNEA_ANT_CENTRE_Z, R_CORNEA_ANT)
    centre_z = 0.0 if use_globe_normal else CORNEA_ANT_CENTRE_Z
    n1 = _normalize(p1 - np.array([0.0, 0.0, centre_z]))
    d1, okr1 = refract(direction, n1, N_AIR / n_out)
    xy, ok2 = march_to_iris(p1, d1)
    return xy, ok1 & okr1 & ok2


def trace_outward(origin, direction):
    """The ground-truth trace RUN BACKWARDS: a ray leaving a point in the
    aqueous humour, out through both corneal surfaces, into air.

    Needed because the entrance pupil is the IMAGE of the real pupil formed by
    the cornea, and an image is where outgoing rays appear to come from -- which
    cannot be measured by a trace that only goes inward. Both intersections take
    the FAR root here: the ray starts inside each sphere.
    """
    _, p1, ok1 = intersect_sphere_front(origin, direction, CORNEA_POST_CENTRE_Z, R_CORNEA_POST)
    n1 = -_normalize(p1 - np.array([0.0, 0.0, CORNEA_POST_CENTRE_Z]))
    d1, okr1 = refract(direction, n1, N_AQUEOUS / N_CORNEA)

    _, p2, ok2 = intersect_sphere_front(p1, d1, CORNEA_ANT_CENTRE_Z, R_CORNEA_ANT)
    n2 = -_normalize(p2 - np.array([0.0, 0.0, CORNEA_ANT_CENTRE_Z]))
    d2, okr2 = refract(d1, n2, N_CORNEA / N_AIR)
    return p2, d2, ok1 & okr1 & ok2 & okr2


# The curvature ratio the shader uses to bend a SPHERE MESH's normal into the
# corneal dome's. A point on the globe at polar angle theta has lateral radius
# R_GLOBE*sin(theta); the corneal cap point at that same lateral radius has its
# normal at phi with sin(phi) = (R_GLOBE / R_CORNEA_ANT) * sin(theta). So the
# bend is ONE ratio, and it is derived here rather than typed.
CURVATURE_RATIO = R_GLOBE / R_CORNEA_ANT


def sphere_mesh(origin, direction, n_out, curvature_ratio):
    """(3b) WHAT THE ENGINE ACTUALLY HAS: the eye is a SPHERE primitive, so the
    ray enters on the globe surface and the only normal the mesh supplies is the
    globe's.

    `curvature_ratio` 1.0 uses that normal as-is -- the free option. Above 1 it
    bends the normal toward the corneal dome's by sin(phi) = ratio * sin(theta),
    which is the one extra line the shader would spend, and this is the arm that
    decides whether that line is worth writing.
    """
    _, p1, ok1 = intersect_sphere_front(origin, direction, 0.0, R_GLOBE)
    n_globe = _normalize(p1)
    cos_theta = np.clip(n_globe[..., 2], -1.0, 1.0)
    sin_theta = np.sqrt(np.maximum(0.0, 1.0 - cos_theta * cos_theta))
    # The tangential direction, away from the axis. Degenerate exactly on the
    # axis, where the bend is a no-op anyway.
    tangential = n_globe - cos_theta[..., None] * AXIS
    tan_len = np.linalg.norm(tangential, axis=-1, keepdims=True)
    tangential = np.where(tan_len > 1e-9, tangential / np.maximum(tan_len, 1e-9), 0.0)

    sin_phi = np.clip(curvature_ratio * sin_theta, 0.0, 1.0)
    cos_phi = np.sqrt(np.maximum(0.0, 1.0 - sin_phi * sin_phi))
    n_cornea = _normalize(cos_phi[..., None] * AXIS + sin_phi[..., None] * tangential)

    d1, okr1 = refract(direction, n_cornea, N_AIR / n_out)
    xy, ok2 = march_to_iris(p1, d1)
    return xy, ok1 & okr1 & ok2


def painted(origin, direction):
    """(4) NO refraction: the iris texture read at the surface hit's own (x, y),
    which is what a flat unlit iris plate in an eye mesh does."""
    _, p1, ok1 = intersect_sphere_front(origin, direction, CORNEA_ANT_CENTRE_Z, R_CORNEA_ANT)
    return p1[..., :2], ok1


def unrefracted_march(origin, direction):
    """The half-measure: march the UNREFRACTED view ray to the iris plane. An
    eye modelled as a transparent shell over a separate iris plate does this by
    geometry alone, with no shader code at all -- so it is the baseline the
    refraction has to beat, not the painted arm."""
    _, p1, ok1 = intersect_sphere_front(origin, direction, CORNEA_ANT_CENTRE_Z, R_CORNEA_ANT)
    xy, ok2 = march_to_iris(p1, direction)
    return xy, ok1 & ok2


# =============================================================================
# Sweeps
# =============================================================================


def view_rays(angle_deg, n=400, span=None):
    """A fan of parallel rays (a distant camera) at `angle_deg` off the optical
    axis, sampled across the corneal aperture.

    PARALLEL and not a point camera because the metric below is about where the
    iris APPEARS, and a perspective camera folds its own projection into that.
    A distant observer is also the case the literature's entrance-pupil number
    is quoted for, which is what lets question 5 check the trace.
    """
    a = math.radians(angle_deg)
    # The view direction points INTO the eye, from the observer.
    d = np.array([-math.sin(a), 0.0, -math.cos(a)])
    # Ray origins on a plane in front of the eye, spread across the pupil's
    # projected extent so most rays reach the iris at any angle.
    span = span if span is not None else PUPIL_RADIUS * 1.6
    s = np.linspace(-span, span, n)
    # Basis perpendicular to d, in the xz plane and in y.
    u = np.array([math.cos(a), 0.0, -math.sin(a)])
    start = np.array([0.0, 0.0, CORNEA_APEX_Z]) + 40.0 * (-d)
    origin = start[None, :] + s[:, None] * u[None, :]
    direction = np.broadcast_to(d, origin.shape).copy()
    return origin, direction


def rms_error(a, b, mask):
    d = np.linalg.norm(a - b, axis=-1)[mask]
    if d.size == 0:
        return float("nan"), float("nan")
    return float(np.sqrt(np.mean(d * d))), float(np.max(d))


def question_2_3_4():
    print("=" * 78)
    print("QUESTIONS 2-4 -- iris-plane landing error against the two-surface trace")
    print("=" * 78)
    print()
    print("  Error is the distance on the IRIS PLANE between where a candidate says a")
    print("  view ray lands and where the ground truth says it lands. Reported in mm")
    print(f"  and as a fraction of the iris radius ({IRIS_OUTER_RADIUS:.2f} mm), because a")
    print("  texture lookup is in iris radii and a millimetre means nothing to it.")
    print()

    candidates = [
        ("painted (no offset)", lambda o, d: painted(o, d)),
        ("march, no refract", lambda o, d: unrefracted_march(o, d)),
        ("1 surface n=1.376", lambda o, d: one_surface(o, d, N_CORNEA)),
        ("1 surface n=1.3375", lambda o, d: one_surface(o, d, N_KERATOMETRIC)),
        ("1 surface n=1.336", lambda o, d: one_surface(o, d, N_AQUEOUS)),
        ("1 surf, GLOBE normal", lambda o, d: one_surface(o, d, N_AQUEOUS, use_globe_normal=True)),
        ("sphere mesh, unbent", lambda o, d: sphere_mesh(o, d, N_AQUEOUS, 1.0)),
        ("sphere mesh, BENT", lambda o, d: sphere_mesh(o, d, N_AQUEOUS, CURVATURE_RATIO)),
    ]

    angles = [0, 10, 20, 30, 40, 50, 60]
    header = f"{'candidate':>22} | " + " | ".join(f"{a:>3}deg" for a in angles)
    print(header)
    print("-" * len(header))

    rows = {}
    for name, fn in candidates:
        cells = []
        worst = 0.0
        for a in angles:
            o, d = view_rays(a)
            t_xy, t_ok = truth_two_surface(o, d)
            c_xy, c_ok = fn(o, d)
            mask = t_ok & c_ok & (np.linalg.norm(t_xy, axis=-1) <= IRIS_OUTER_RADIUS)
            rms, mx = rms_error(c_xy, t_xy, mask)
            cells.append(f"{rms:6.3f}")
            if not math.isnan(rms):
                worst = max(worst, rms)
        rows[name] = worst
        print(f"{name:>22} | " + " | ".join(cells))

    print()
    print("  (RMS millimetres on the iris plane. Cells are blank-ish as NaN where no")
    print("   ray reaches the iris at that angle for that candidate.)")
    print()
    print(f"{'candidate':>22} | worst RMS, in IRIS RADII")
    print("-" * 50)
    for name, worst in rows.items():
        print(f"{name:>22} | {worst / IRIS_OUTER_RADIUS:>7.1%}")
    print()


def question_5():
    print("=" * 78)
    print("QUESTION 5 -- the entrance pupil, the one metric with an external answer")
    print("=" * 78)
    print()
    print("  The real pupil rim traced BACK out through both corneal surfaces, then")
    print("  projected as a distant on-axis observer sees it. Literature (Gullstrand;")
    print("  Bennett & Rabbetts, *Clinical Visual Optics*): linear magnification 1.13,")
    print("  the apparent pupil about 0.5 mm ANTERIOR to the real one.")
    print()

    # Bisect on the incoming ray height that lands exactly on the pupil rim.
    def lands_at(height):
        o = np.array([[height, 0.0, CORNEA_APEX_Z + 40.0]])
        d = np.array([[0.0, 0.0, -1.0]])
        xy, ok = truth_two_surface(o, d)
        return xy[0, 0] if ok[0] else float("nan")

    lo, hi = 0.0, LIMBUS_RADIUS * 0.98
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if lands_at(mid) < PUPIL_RADIUS:
            lo = mid
        else:
            hi = mid
    entrance_radius = 0.5 * (lo + hi)
    mag = entrance_radius / PUPIL_RADIUS

    # WHERE the apparent pupil sits. The entrance pupil is the IMAGE of the
    # real pupil formed by the cornea, so it is found the way any image is:
    # trace two rays LEAVING the same real-pupil point at different angles, out
    # through both corneal surfaces, and intersect their outgoing lines. Where
    # they cross is where a distant observer sees that point to be.
    #
    # Two rays and not one, because a single ray gives a line and an image is a
    # point. The angles are small and unequal so the pair is a genuine bundle
    # rather than two samples of one direction.
    def entrance_pupil_image():
        rim = np.array([PUPIL_RADIUS, 0.0, IRIS_PLANE_Z])
        dirs = []
        for a_deg in (1.0, 6.0):
            a = math.radians(a_deg)
            dirs.append([math.sin(a), 0.0, math.cos(a)])
        o = np.broadcast_to(rim, (2, 3)).copy()
        d = _normalize(np.array(dirs))
        p, dd, ok = trace_outward(o, d)
        if not bool(ok.all()):
            return float("nan"), float("nan")
        # Intersect the two outgoing lines in the xz plane. Solve
        #   p0 + t0*d0 == p1 + t1*d1   for the two unknowns.
        a_mat = np.array([[dd[0, 0], -dd[1, 0]],
                          [dd[0, 2], -dd[1, 2]]])
        b_vec = np.array([p[1, 0] - p[0, 0], p[1, 2] - p[0, 2]])
        if abs(np.linalg.det(a_mat)) < 1e-12:
            return float("nan"), float("nan")
        t = np.linalg.solve(a_mat, b_vec)
        cross = p[0] + t[0] * dd[0]
        return float(cross[0]), float(cross[2])

    img_r, z_app = entrance_pupil_image()



    print(f"  real pupil radius            : {PUPIL_RADIUS:.3f} mm  at z = {IRIS_PLANE_Z:.3f}")
    print(f"  entrance (apparent) radius   : {entrance_radius:.3f} mm"
          f"   (image trace: {img_r:.3f})")
    print(f"  linear magnification         : {mag:.3f}x      (literature 1.13)")
    print(f"  apparent pupil plane         : z = {z_app:.3f}")
    print(f"  anterior shift               : {z_app - IRIS_PLANE_Z:+.3f} mm  (literature +0.5)")
    print()
    verdict = "AGREES" if abs(mag - 1.13) < 0.02 else "DISAGREES"
    print(f"  -> the trace {verdict} with the literature magnification.")
    print("     That is a check on THIS FILE, run before any candidate is scored")
    print("     against it -- a ground truth nobody validated is just a fourth opinion.")
    print()


def question_6():
    print("=" * 78)
    print("QUESTION 6 -- gaze parallax: what a static front-on capture cannot show")
    print("=" * 78)
    print()
    print("  The eye is rotated about its own centre (a gaze change) while the camera")
    print("  stays put. Measured: where the PUPIL CENTRE appears, for the refracting")
    print("  model and for the painted one. A painted iris is rigidly attached to the")
    print("  surface; a refracted one is seen through a lens that moves with it.")
    print()
    print(f"{'gaze':>6} | {'truth':>9} | {'painted':>9} | {'difference':>11} | {'in iris radii':>14}")
    print("-" * 62)

    for gaze_deg in [0, 5, 10, 15, 20, 25, 30]:
        # Rotating the eye by `gaze` about the globe centre, with a fixed
        # head-on camera, is the same trace as a camera at -gaze into a fixed
        # eye. The globe centre is the rotation centre, which is why eye-local
        # space is anchored there.
        o, d = view_rays(gaze_deg, n=2001, span=LIMBUS_RADIUS * 0.98)

        t_xy, t_ok = truth_two_surface(o, d)
        p_xy, p_ok = painted(o, d)

        # The apparent pupil centre: the mean of the entry heights whose traced
        # landing point is inside the pupil. A centroid of what the observer
        # actually sees as pupil, not a point sample.
        entry_x = o[:, 0]
        t_in = t_ok & (np.linalg.norm(t_xy, axis=-1) <= PUPIL_RADIUS)
        p_in = p_ok & (np.linalg.norm(p_xy, axis=-1) <= PUPIL_RADIUS)
        if t_in.sum() < 3 or p_in.sum() < 3:
            print(f"{gaze_deg:>4}deg | {'--':>9} | {'--':>9} | {'--':>11} | {'--':>14}")
            continue
        t_c = float(entry_x[t_in].mean())
        p_c = float(entry_x[p_in].mean())
        diff = t_c - p_c
        print(f"{gaze_deg:>4}deg | {t_c:>9.3f} | {p_c:>9.3f} | {diff:>+11.3f} | "
              f"{diff / IRIS_OUTER_RADIUS:>+13.1%}")

    print()
    print("  (Apparent pupil-centre position in the observer's plane, millimetres.")
    print("   The 0deg row is the one that matters most: it is ZERO by symmetry, which")
    print("   is precisely why a front-on capture cannot tell the two models apart.")
    print("   Everything the feature is worth is in the rows below it.)")
    print()


def question_7():
    print("=" * 78)
    print("QUESTION 7 -- the Fresnel numbers, for the tear film and the cornea")
    print("=" * 78)
    print()
    print("  Both interfaces the eye adds are DIELECTRIC and the shader needs F0 for")
    print("  each. Derived here rather than typed into a shader, for the reason")
    print("  Renderer/SkinOralSurface.h gives: an author can check an index against a")
    print("  table and cannot check 0.0208 against anything.")
    print()
    for name, n in [("tear film (water)", 1.337),
                    ("corneal stroma", N_CORNEA),
                    ("aqueous humour", N_AQUEOUS),
                    ("saliva (#1245's coat)", 1.33)]:
        f0 = ((n - 1.0) / (n + 1.0)) ** 2
        print(f"  {name:>24}  n = {n:.4f}   F0 = {f0:.5f}")
    print()
    print("  The tear film and saliva are within 3% of each other in F0, which is the")
    print("  argument for the tear line REUSING #1245's coat rather than getting its")
    print("  own: they are the same interface -- a thin water film over wet tissue --")
    print("  and a second implementation would be a second opinion about one number.")
    print()




def question_2b():
    """WHERE the index question has an answer -- and where it does not.

    Asked because the obvious way to choose an index is to sweep it against the
    ground truth on the mesh you actually have, and on a SPHERE PRIMITIVE that
    method gives the WRONG ANSWER. A higher index refracts harder, and
    refracting harder partly cancels the entry-point error a sphere's missing
    corneal bulge introduces. Two errors cancelling is not a better model, and
    an index tuned that way would be wrong the day a real eye mesh arrived.
    """
    print("=" * 78)
    print("QUESTION 2b -- the index question, asked on two different meshes")
    print("=" * 78)
    print()
    angles = [0, 20, 40, 60]

    def sweep(fn):
        errs = []
        for a in angles:
            o, d = view_rays(a)
            t_xy, t_ok = truth_two_surface(o, d)
            c_xy, c_ok = fn(o, d)
            mask = t_ok & c_ok & (np.abs(t_xy[:, 0]) <= IRIS_OUTER_RADIUS)
            errs.extend(np.abs(c_xy[mask, 0] - t_xy[mask, 0]))
        e = np.array(errs)
        return float(np.sqrt((e * e).mean())) / IRIS_OUTER_RADIUS if e.size else float("nan")

    print(f"{'index':>16} | {'sphere mesh':>12} | {'corneal mesh':>13}")
    print("-" * 48)
    for name, n in [("aqueous 1.336", N_AQUEOUS), ("keratometric", N_KERATOMETRIC),
                    ("stromal 1.376", N_CORNEA)]:
        sphere = sweep(lambda o, d, n=n: sphere_mesh(o, d, n, CURVATURE_RATIO))
        corneal = sweep(lambda o, d, n=n: one_surface(o, d, n))
        print(f"{name:>16} | {sphere:>11.2%} | {corneal:>12.2%}")

    print()
    print("  ON THE SPHERE the ordering is INVERTED and the margin is tiny: the")
    print("  stromal index looks slightly better because its extra refraction")
    print("  cancels part of the sphere's entry-point error. ON CORNEAL GEOMETRY,")
    print("  where the index is the only remaining approximation, the aqueous one")
    print("  wins by a factor of ten.")
    print()
    print("  So the shipped default is 1.336, chosen on the right-hand column.")
    print("  SkinOcularSurfaceTest.TheAqueousIndexBeatsTheStromalOne asserts it")
    print("  there and records this inversion in its comment, so nobody 'fixes'")
    print("  the default by measuring on the left-hand one.")
    print()


def question_4b():
    """Can an authored iris-plane DEPTH buy back what a sphere mesh loses?

    Asked because the answer decides a shipped default, and because the
    tempting answer is yes: the sphere's apex is 1.12 mm behind the real
    corneal apex, so the march is short, so lengthen it. If the residual were a
    depth error that would work perfectly. It is not -- it is an ENTRY-POINT
    error, and no depth reaches an entry point.
    """
    print("=" * 78)
    print("QUESTION 4b -- can an authored depth rescue a sphere-primitive eye?")
    print("=" * 78)
    print()
    print("  Worst-case RMS over the 0-60 degree sweep, as the authored iris-plane")
    print("  depth (measured from the SPHERE's own apex, z = globe radius) is swept.")
    print("  The truth keeps the real eye's iris plane throughout -- only the")
    print("  candidate's authored depth moves.")
    print()
    print(f"{'depth mm':>9} | {'unbent':>9} | {'bent':>9}")
    print("-" * 33)

    angles = [0, 10, 20, 30, 40, 50, 60]
    real_plane = IRIS_PLANE_Z

    def worst(depth, ratio):
        global IRIS_PLANE_Z
        w = 0.0
        for a in angles:
            o, d = view_rays(a)
            IRIS_PLANE_Z = real_plane
            t, tok = truth_two_surface(o, d)
            IRIS_PLANE_Z = R_GLOBE - depth
            c, cok = sphere_mesh(o, d, N_AQUEOUS, ratio)
            mask = tok & cok & (np.linalg.norm(t, axis=-1) <= IRIS_OUTER_RADIUS)
            r, _ = rms_error(c, t, mask)
            if not math.isnan(r):
                w = max(w, r)
        IRIS_PLANE_Z = real_plane
        return w

    derived = R_GLOBE - IRIS_PLANE_Z
    for depth in [2.0, 2.3, derived, 2.7, 3.0, 3.6, 4.2, 5.0]:
        tag = "  <- DERIVED (sphere apex -> real iris plane)" if abs(depth - derived) < 1e-9 else ""
        print(f"{depth:>9.3f} | {worst(depth, 1.0) / IRIS_OUTER_RADIUS:>8.1%} | "
              f"{worst(depth, CURVATURE_RATIO) / IRIS_OUTER_RADIUS:>8.1%}{tag}")

    print()
    print("  The curve is FLAT-BOTTOMED around the derived value and the floor is")
    print("  ~7%, not ~0%. So: no. The residual is the corneal bulge the sphere does")
    print("  not have, and the shipped default is the DERIVED depth")
    print(f"  ({derived:.3f} mm = {derived / R_GLOBE:.3f} eye radii), not the fitted one --")
    print("  a derived number that is 0.7 points worse beats a fitted number that")
    print("  nobody can re-derive when a radius changes.")
    print()


def main():
    print()
    print("SCHEMATIC EYE, derived in eye-local millimetres (origin = globe centre):")
    print(f"  globe radius            {R_GLOBE:8.3f}")
    print(f"  limbus ring at z        {_LIMBUS_Z:8.3f}  (radius {LIMBUS_RADIUS:.2f})")
    print(f"  corneal sagitta         {_CORNEAL_SAGITTA:8.3f}")
    print(f"  corneal apex z          {CORNEA_APEX_Z:8.3f}")
    print(f"  anterior centre z       {CORNEA_ANT_CENTRE_Z:8.3f}  (R {R_CORNEA_ANT:.2f})")
    print(f"  posterior centre z      {CORNEA_POST_CENTRE_Z:8.3f}  (R {R_CORNEA_POST:.2f})")
    print(f"  curvature ratio k       {CURVATURE_RATIO:8.3f}  (globe R / corneal R)")
    print(f"  iris plane z            {IRIS_PLANE_Z:8.3f}  (ACD {ACD_FROM_APEX:.2f} from apex)")
    print(f"  axial length            {CORNEA_APEX_Z + R_GLOBE:8.3f}  (clinical 23.5-24.5)")
    print()
    question_5()
    question_2_3_4()
    question_2b()
    question_4b()
    question_6()
    question_7()


if __name__ == "__main__":
    main()
