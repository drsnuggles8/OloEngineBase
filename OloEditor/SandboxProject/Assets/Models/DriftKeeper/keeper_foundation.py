"""Hair-bearing skin and undercoat. These strips keep one topology at all body LODs."""

from math import cos, sin, sqrt, pi
import numpy as np

from keeper_face import EYE_X, EYE_Y, EYE_Z, EYE_R, head_point
from keeper_mesh import Surface, Gltf

REGIONS = ("beard", "moustache", "scalp", "eyebrow", "eyelash")


def root_uv(region, side, u, v):
    # Separate the ten patches in a 4x4 atlas. Leave a gutter wider than
    # a card cell: mirrored cheeks must never choose the same representative.
    chart = REGIONS.index(region) * 2 + int(side > 0)
    scale = 0.24
    return ((chart % 4) * 0.25 + 0.005 + scale * u, (chart // 4) * 0.25 + 0.005 + scale * v)


def follicle(region, side, u, v):
    if region == "eyelash":
        a = 0.07 + (pi - 0.14) * u
        dx = 0.0119 * cos(a)
        dy = (0.0048 + 0.0014 * v) * sin(a) + 0.0006 * side * cos(a)
        r = EYE_R + 0.00053
        return np.array((side * EYE_X + dx, EYE_Y + dy, EYE_Z + sqrt(max(0, r * r - dx * dx - dy * dy))))
    if region == "beard":
        a = side * (0.001 + 1.84 * u)
        # Rise to the sideburns. The bare patch below the lip and notched
        # cheek line break up the abrupt straight edge across a smiling face.
        top = 1.554 + 0.079 * u**1.47 + 0.003 * sin(22 * u + 0.3 * side) * u
        top += 0.0025 * sin(48 * u + 0.45 * side) + 0.0017 * sin(77 * u)
        top -= 0.008 * np.exp(-(((u - 0.095) / 0.095) ** 2))
        y = 1.521 + v * (top - 1.521)
    elif region == "moustache":
        a = side * (0.003 + 0.39 * u)
        y = 1.568 + 0.010 * v - 0.008 * u * u - 0.003 * abs(2 * v - 1) * u
    elif region == "scalp":
        a = side * (0.88 + (pi - 0.88) * u)
        bottom = 1.620 + 0.029 * np.exp(-(((abs(a) - 1.66) / 0.31) ** 2))
        y = bottom + v * (1.684 - bottom)
    else:
        a = side * (0.16 + 0.54 * u)
        y = 1.646 + 0.007 * sin(u * pi) - 0.003 * u + 0.0018 * side * (1 - u)
        y += (2 * v - 1) * (0.0024 - 0.0016 * u) * (0.20 + 0.80 * sin(pi * u) ** 0.65)
    root = head_point(a, y)
    lift = 0.0008 * sin(pi * v) * sin(pi * u)
    # Bury the edge of the pigmented short undercoat. Individual guard hairs
    # emerge through the skin over a few millimetres along the cheek line.
    lift += 0.00045 - 0.0018 * v**5 if region == "beard" else 0.00035
    root += np.array((sin(a), 0, cos(a))) * lift
    return root


def export(out):
    model = Surface()
    palette = {
        "KeeperUndercoat": ((0.095, 0.078, 0.059), 0.92, 0),
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
                        dy = -(0.0048 + 0.0014 * v) * sin(0.07 + (pi - 0.14) * u) - 0.00045
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
                        lower = np.exp(-((p[0] / 0.061) ** 2 + ((p[1] - 1.534) / 0.019) ** 2))
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
