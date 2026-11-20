"""Hair-bearing skin and undercoat. These strips keep one topology at all body LODs."""

from math import cos, sin, sqrt, pi, exp
import numpy as np

from keeper_face import EYE_X, EYE_Y, EYE_Z, EYE_R, head_point, jaw_influence, lid_margin
from keeper_mesh import Surface, Gltf

REGIONS = ("beard", "moustache", "scalp", "eyebrow", "eyelash")


def root_uv(region, side, u, v):
    # Separate the ten patches in a 4x4 atlas. Leave a gutter wider than
    # a card cell: mirrored cheeks must never choose the same representative.
    chart = REGIONS.index(region) * 2 + int(side > 0)
    scale = 0.24
    return ((chart % 4) * 0.25 + 0.005 + scale * u, (chart // 4) * 0.25 + 0.005 + scale * v)


def hair_coordinates(region, side, u, v):
    if region == "beard":
        a = side * (0.001 + 1.96 * u)
        # Rise to the sideburns. The bare patch below the lip and notched
        # cheek line break up the abrupt straight edge across a smiling face.
        # High side whiskers sweep into the jaw; leave the nasolabial
        # triangle and the space directly beneath the lip exposed.
        top = 1.555 + 0.049 * u**1.25 + 0.038 * exp(-(((abs(a) - 1.285) / 0.29) ** 2))
        top += 0.0014 * sin(15 * u + 0.53 * side) - 0.007 * exp(-(((u - 0.12) / 0.085) ** 2))
        y = 1.520 + v * (top - 1.520)
    elif region == "moustache":
        a = side * (0.0015 + 0.435 * u)
        top = 1.5813 - 0.0076 * u - 0.0022 * exp(-((u / 0.13) ** 2))
        base = 1.5655 - 0.0052 * u**1.7 + 0.0016 * exp(-((u / 0.12) ** 2))
        y = base + v * (top - base) + 0.00045 * side
    elif region == "scalp":
        a = side * (0.81 + (pi - 0.81) * u)
        # A continuous outline turns above the ear from the recession at the
        # temple to low occipital hair. A rectangular cut leaves a visible notch.
        bottom = np.interp(
            abs(a),
            [0.81, 1.08, 1.29, 1.49, 1.71, 1.91, 2.27, pi],
            [1.682, 1.656, 1.627, 1.650, 1.656, 1.638, 1.607, 1.595],
        )
        bottom += 0.0014 * sin(12 * a)
        # The firm sweatband folds down over the top few millimetres of
        # hair. Its sloped edge conceals the roots at both temples.
        ceiling = 1.6715 + 0.004 * cos(a) - 0.003 * sin(a)
        bottom = min(bottom, ceiling - 0.0015)
        y = bottom + v * (ceiling - bottom)
    else:
        a = side * (0.155 + 0.565 * u)
        y = 1.6425 + 0.0054 * sin(pi * u) - 0.0019 * u + 0.0018 * side * (1 - u)
        y += (2 * v - 1) * (0.0042 - 0.0021 * u) * (0.45 + 0.55 * sin(pi * u) ** 0.65)
    return a, y


def follicle(region, side, u, v):
    if region == "eyelash":
        a = 0.07 + (pi - 0.14) * u
        margin = lid_margin(side, a, pad=0.0012 * v)
        margin[2] += 0.00009
        return margin
    a, y = hair_coordinates(region, side, u, v)
    root = head_point(a, y)
    lift = 0.0008 * sin(pi * v) * sin(pi * u)
    # Bury the edge of the pigmented short undercoat. Individual guard hairs
    # emerge through the skin over a few millimetres along the cheek line.
    lift += 0.00025 - 0.0025 * abs(2 * v - 1) ** 3 - 0.0013 * abs(2 * u - 1) ** 5
    root += np.array((sin(a), 0, cos(a))) * lift
    return root


def export(out):
    model = Surface()
    palette = {
        "KeeperUndercoat": ((0.17, 0.114, 0.086), 0.90, 0),
        "KeeperLashLine": ((0.145, 0.070, 0.050), 0.49, 0),
    }
    for region in REGIONS:
        for side in (-1, 1):
            cols = 46 if region in ("beard", "scalp") else 24
            rows = 12 if region in ("beard", "scalp") else 4
            for j in range(rows + 1):
                for i in range(cols + 1):
                    u, v = i / cols, j / rows
                    p = follicle(region, side, u, v)
                    tu, tv = root_uv(region, side, u, v)
                    index = model.vertex(p, (tu, 1 - tv), "head")
                    if region == "eyelash":
                        dy = EYE_Y - 0.0015 + side * 0.00105 * cos(0.07 + (pi - 0.14) * u) - p[1]
                        r = EYE_R + 0.00055
                        dz = (
                            EYE_Z
                            + sqrt(max(0, r * r - (p[0] - side * EYE_X) ** 2 - (p[1] + dy - EYE_Y) ** 2))
                            - p[2]
                        )
                        model.shapes["BlinkL" if side == 1 else "BlinkR"][index] = (0, dy, dz)
                        model.shapes["Squint"][index] = (0, 0.55 * dy, 0.55 * dz)
                    elif region == "eyebrow":
                        model.shapes["BrowRaise"][index] = (0, 0.004, 0)
                    elif region in ("beard", "moustache"):
                        lower = jaw_influence(p)
                        for name, gap in (("JawOpen", 0.018), ("AA", 0.010), ("OO", 0.004)):
                            model.shapes[name][index] = (0, -gap * lower, -gap * 0.22 * lower)
                        model.shapes["Smile"][index] = (0.002 * side * v, 0.002 * u * v, 0)
                    if i and j:
                        a, b, c, d = index - cols - 2, index - cols - 1, index, index - 1
                        # Lash loops travel the opposite way around the sphere.
                        if region == "eyelash" or side < 0:
                            b, d = d, b
                        model.quad("KeeperLashLine" if region == "eyelash" else "KeeperUndercoat", a, b, c, d)
    file = Gltf()
    file.mesh(model, palette, expressions=True)
    file.doc["meshes"][0]["name"] = "KeeperHairFoundation"
    for mat in file.doc["materials"]:
        mat["doubleSided"] = True
    file.doc["nodes"] = [dict(name="KeeperHairFoundation", mesh=0)]
    file.write(out / "KeeperHairFoundation.gltf")
