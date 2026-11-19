"""Full body beneath the work clothes, evaluated in metres in the rig's rest pose."""

from math import atan2, pi
from pathlib import Path
import sys

import numpy as np

# Reuse the mesher used for the showcase dog. Only its generic field operators
# are shared; the human landmarks and the field below are authored here.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Dog"))
from build_dog import sd_ellipsoid, sd_round_cone, smin, surface_nets

# Transverse sections from pelvic floor to the base of the skull. The rear of
# the pelvis and the upper thorax sit behind the bearing line of the ankles.
SECTIONS = np.array(
    [
        (0.793, 0.012, 0.030, -0.007),
        (0.810, 0.074, 0.075, -0.013),
        (0.849, 0.119, 0.091, -0.015),
        (0.904, 0.141, 0.104, -0.014),
        (0.955, 0.142, 0.105, -0.009),
        (1.010, 0.133, 0.106, -0.002),
        (1.065, 0.128, 0.113, 0.002),
        (1.108, 0.133, 0.114, 0.001),
        (1.167, 0.143, 0.111, -0.003),
        (1.221, 0.151, 0.115, -0.005),
        (1.278, 0.167, 0.120, -0.005),
        (1.326, 0.173, 0.119, -0.005),
        (1.373, 0.178, 0.109, -0.007),
        (1.408, 0.171, 0.091, -0.010),
        (1.429, 0.139, 0.079, -0.012),
        (1.446, 0.097, 0.067, -0.011),
        (1.470, 0.065, 0.059, -0.008),
        (1.506, 0.060, 0.057, -0.006),
        (1.538, 0.054, 0.051, -0.007),
        (1.559, 0.029, 0.032, -0.008),
        (1.567, 0.002, 0.003, -0.008),
    ]
)


def section_values(stations, heights):
    # Cubic interpolation with the adjacent section slopes keeps the surface
    # normal continuous through the patella, elbow and iliac crest.
    knots = stations[:, 0]
    y = np.clip(heights, knots[0], knots[-1])
    i = np.clip(np.searchsorted(knots, y) - 1, 0, len(knots) - 2)
    span = (knots[i + 1] - knots[i])[:, None]
    t = (y - knots[i])[:, None] / span
    slope = np.gradient(stations[:, 1:], knots, axis=0)
    return (
        (1 + 2 * t) * (1 - t) ** 2 * stations[i, 1:]
        + t * (1 - t) ** 2 * span * slope[i]
        + t * t * (3 - 2 * t) * stations[i + 1, 1:]
        + t * t * (t - 1) * span * slope[i + 1]
    ).T


# height, lateral and anterior centre, coronal and sagittal radii
ARM = np.array(
    [
        (0.924, 0.252, 0.053, 0.013, 0.022),
        (0.942, 0.252, 0.051, 0.020, 0.028),
        (0.980, 0.251, 0.046, 0.024, 0.029),
        (1.025, 0.248, 0.041, 0.030, 0.033),
        (1.082, 0.246, 0.032, 0.039, 0.041),
        (1.115, 0.244, 0.026, 0.041, 0.039),
        (1.150, 0.239, 0.015, 0.038, 0.035),
        (1.170, 0.235, 0.013, 0.035, 0.034),
        (1.215, 0.228, 0.009, 0.037, 0.042),
        (1.275, 0.214, 0.005, 0.045, 0.051),
        (1.321, 0.207, 0.001, 0.047, 0.052),
        (1.365, 0.188, -0.003, 0.054, 0.060),
        (1.393, 0.177, -0.004, 0.052, 0.059),
        (1.417, 0.166, -0.006, 0.039, 0.045),
        (1.435, 0.156, -0.009, 0.008, 0.012),
    ]
)
LEG = np.array(
    [
        (0.060, 0.111, -0.010, 0.030, 0.036),
        (0.092, 0.110, -0.006, 0.029, 0.034),
        (0.132, 0.111, -0.008, 0.027, 0.034),
        (0.196, 0.113, -0.008, 0.033, 0.039),
        (0.253, 0.115, -0.012, 0.042, 0.049),
        (0.313, 0.112, -0.019, 0.051, 0.058),
        (0.356, 0.110, -0.015, 0.053, 0.058),
        (0.404, 0.110, 0.001, 0.045, 0.051),
        (0.446, 0.108, 0.013, 0.043, 0.045),
        (0.472, 0.106, 0.018, 0.048, 0.048),
        (0.497, 0.106, 0.016, 0.050, 0.048),
        (0.531, 0.106, 0.009, 0.051, 0.056),
        (0.582, 0.103, 0.000, 0.061, 0.066),
        (0.653, 0.100, -0.005, 0.070, 0.073),
        (0.733, 0.093, -0.009, 0.077, 0.082),
        (0.811, 0.086, -0.014, 0.081, 0.092),
        (0.879, 0.080, -0.019, 0.081, 0.094),
        (0.932, 0.077, -0.019, 0.063, 0.072),
        (0.961, 0.073, -0.019, 0.024, 0.032),
    ]
)


def limb(points, stations):
    x, y, z = points.T
    cx, cz, w, d = section_values(stations, y)
    radial = (np.hypot((x - cx) / w, (z - cz) / d) - 1) * np.minimum(w, d)
    return np.maximum(radial, np.maximum(stations[0, 0] - y, y - stations[-1, 0])).astype(np.float32)


def trunk_field(points):
    x, y, z = points.T
    width, depth, mid = section_values(SECTIONS, y)

    def muscle(cx, cy, sx, sy):
        return np.exp(-(((x - cx) / sx) ** 2) - ((y - cy) / sy) ** 2)

    anterior = np.clip((z - mid) / depth, 0, 1)
    posterior = np.clip((mid - z) / depth, 0, 1)
    relief = (
        0.011 * muscle(0.082, 1.317, 0.076, 0.063)
        + 0.010 * muscle(0.042, 1.085, 0.076, 0.080)
        - 0.0028 * muscle(0, 1.275, 0.014, 0.110)
        - 0.0035 * muscle(0, 1.059, 0.009, 0.006)
        + 0.004 * muscle(0.083, 1.415 - 0.13 * x, 0.076, 0.010)
    )
    back = (
        0.020 * muscle(0.067, 0.881, 0.059, 0.066)
        + 0.009 * muscle(0.090, 1.332, 0.064, 0.076)
        - 0.007 * muscle(0, 1.21, 0.012, 0.162)
    )
    depth += anterior * relief + posterior * back
    core = (np.hypot(x / width, (z - mid) / depth) - 1) * np.minimum(width, depth)
    return np.maximum(core, np.maximum(SECTIONS[0, 0] - y, y - SECTIONS[-1, 0])).astype(np.float32)


def body_field(points):
    # surface_nets requests the positive-x half, and mirrors it before meshing.
    y = points[:, 1]
    core = trunk_field(points)

    def round_part(centre, radii, join):
        nonlocal core
        core = smin(core, sd_ellipsoid(points, centre, radii), join)

    def taper(start, end, r0, r1, join):
        nonlocal core
        core = smin(core, sd_round_cone(points, start, end, r0, r1), join)

    # The limbs, thorax and pelvis meet in one continuous skin surface.
    # Small bony landmarks sit below it instead of separate muscle shells.
    core = smin(core, limb(points, ARM), 0.034)
    core = smin(core, limb(points, LEG), 0.039)
    round_part((0.108, 0.474, 0.051), (0.027, 0.032, 0.018), 0.010)
    round_part((0.132, 0.079, -0.005), (0.010, 0.017, 0.014), 0.006)
    round_part((0.089, 0.086, 0.001), (0.009, 0.016, 0.014), 0.006)
    round_part((0.111, 0.046, -0.026), (0.034, 0.044, 0.053), 0.010)
    round_part((0.112, 0.043, 0.034), (0.041, 0.035, 0.079), 0.012)
    round_part((0.114, 0.026, 0.095), (0.049, 0.024, 0.050), 0.010)
    for toe, (dx, tip, radius) in enumerate(
        [
            (-0.032, 0.171, 0.013),
            (-0.011, 0.174, 0.0096),
            (0.005, 0.165, 0.0086),
            (0.021, 0.152, 0.0080),
            (0.035, 0.137, 0.0074),
        ]
    ):
        taper(
            (0.112 + dx, 0.020, 0.105), (0.112 + dx + 0.003 * toe, 0.015, tip), radius + 0.002, radius, 0.0038
        )
    return np.maximum(core, -y)


def underlying_body(surface, trunk_weights, mix_weights):
    pts, quads = surface_nets(body_field, 0.0045, lo=(-0.345, -0.018, -0.175), hi=(0.345, 1.585, 0.218))
    start = len(surface.points)
    half = pts.copy()
    half[:, 0] = np.abs(half[:, 0])
    core = trunk_field(half)
    # The derivative of the smooth union distributes weight on its junction.
    # At the armpit this separates a rib from an upper arm at the same height.
    arms = np.clip(0.5 + 0.5 * (core - limb(half, ARM)) / 0.034, 0, 1)
    legs = np.clip(0.5 + 0.5 * (core - limb(half, LEG)) / 0.039, 0, 1)
    for (x, y, z), arm_fraction, leg_fraction in zip(pts, arms, legs):
        side = "L" if x > 0 else "R"
        torso = trunk_weights((x, y, z)) if y < 1.435 else mix_weights(y, 1.435, 1.476, "chest", "neck")
        if arm_fraction > leg_fraction:
            fraction = arm_fraction
            if y < 0.965:
                limb_weights = mix_weights(y, 0.931, 0.965, "hand_" + side, "forearm_" + side)
            else:
                limb_weights = mix_weights(y, 1.105, 1.192, "forearm_" + side, "upperarm_" + side)
        else:
            fraction = leg_fraction
            if y < 0.14:
                fraction = 1
                limb_weights = mix_weights(y, 0.085, 0.14, "boot_" + side, "shin_" + side)
            else:
                limb_weights = mix_weights(y, 0.420, 0.526, "shin_" + side, "thigh_" + side)
        weights = {bone: w * (1 - fraction) for bone, w in torso.items() if w * (1 - fraction) > 0}
        weights.update({bone: w * fraction for bone, w in limb_weights.items() if w * fraction > 0})
        surface.vertex((x, y, z), (0.5 + atan2(x, z) / (2 * pi), 1 - y / 1.75), weights)
    for a, b, c, d in quads + start:
        surface.quad("KeeperSkin", a, b, c, d)
