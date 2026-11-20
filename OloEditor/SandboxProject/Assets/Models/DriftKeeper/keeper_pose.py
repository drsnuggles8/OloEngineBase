"""Settle the rig and its clothing over the load-bearing boot before exporting.

The glTF joints keep the engine's existing local axes. Geometry and joint
centres are both fitted to the stance, so the authored clips can rotate the
new pose without translating a boot or separating the hand from the bail.
"""

import numpy as np
from math import cos, radians, sin, sqrt


def rotation(pitch=0, yaw=0, roll=0):
    x, y, z = map(radians, (pitch, yaw, roll))
    cx, cy, cz, sx, sy, sz = cos(x), cos(y), cos(z), sin(x), sin(y), sin(z)
    return np.array(
        [
            [cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
            [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
            [-sy, cy * sx, cy * cx],
        ]
    )


def align(original, posed):
    a = original / np.linalg.norm(original)
    b = posed / np.linalg.norm(posed)
    x, y, z = np.cross(a, b)
    cross = np.array([[0, -z, y], [z, 0, -x], [-y, x, 0]])
    return np.eye(3) + cross + cross @ cross / (1 + np.dot(a, b))


def settle(surface, joints):
    centres = {name: np.array(point, dtype=float) for name, (_, point) in joints.items()}
    frames = {name: (np.eye(3), origin) for name, origin in centres.items()}

    def moved(bone, point):
        basis, centre = frames[bone]
        return centre + basis @ (point - centres[bone])

    # The pelvis travels towards the right boot, with the free hip lower.
    # Chest counter-turn and the low left shoulder carry the weight of the
    # lantern. Keep the head upright to give the look controller its full arc.
    frames["pelvis"] = (rotation(2, 7, -6), centres["pelvis"] + [-0.054, -0.017, -0.015])
    frames["spine"] = (rotation(4, 0, -3), centres["spine"] + [-0.043, -0.012, -0.009])
    frames["chest"] = (rotation(4, -7, -4), centres["chest"] + [-0.023, -0.010, 0.005])
    frames["neck"] = (rotation(3, -2, 0), centres["neck"] + [-0.008, -0.006, 0.003])
    for side in ("L", "R"):
        arm, elbow, hand = [part + "_" + side for part in ("upperarm", "forearm", "hand")]
        left = side == "L"
        frames[arm] = (rotation(0 if left else 7, -5, 1 if left else -7), moved("chest", centres[arm]))
        frames[elbow] = (
            rotation(-8 if left else -30, -6 if left else 10, 1 if left else 35),
            moved(arm, centres[elbow]),
        )
        frames[hand] = (
            rotation(-5 if left else -8, -6 if left else 68, 2 if left else 26),
            moved(elbow, centres[hand]),
        )

        thigh, shin, boot = [part + "_" + side for part in ("thigh", "shin", "boot")]
        hip = moved("pelvis", centres[thigh])
        ankle = centres[boot] + ([0.020, 0, 0.058] if left else [0, 0, 0])
        lower = centres[boot] - centres[shin]
        upper = centres[shin] - centres[thigh]
        line = ankle - hip
        length = np.linalg.norm(line)
        axis = line / length
        along = (np.dot(upper, upper) - np.dot(lower, lower) + length**2) / (2 * length)
        # Both knees bend towards the tread's front. The free knee bends
        # further and turns out, with the entire sole planted on the granite.
        pole = np.array([0.28 if left else -0.09, 0, 1.0])
        pole -= axis * np.dot(axis, pole)
        reach = np.dot(upper, upper) - along**2
        if reach < 0:
            raise ValueError("Rest stance exceeds the leg's reach")
        knee = hip + along * axis + sqrt(reach) * pole / np.linalg.norm(pole)
        frames[thigh] = (align(upper, knee - hip), hip)
        frames[shin] = (align(lower, ankle - knee), knee)
        frames[boot] = (rotation(yaw=10 if left else -5), ankle)

    frames["lantern"] = (np.eye(3), moved("hand_L", centres["lantern"]))
    points = np.asarray(surface.points)
    linear = np.zeros((len(points), 3, 3))
    offset = np.zeros_like(points)
    shifts = {name: centre - basis @ centres[name] for name, (basis, centre) in frames.items()}
    for i, weights in enumerate(surface.bones):
        for name, weight in weights.items():
            linear[i] += weight * frames[name][0]
            offset[i] += weight * shifts[name]
    surface.points = (np.einsum("ijk,ik->ij", linear, points) + offset).tolist()
    for deltas in surface.shapes.values():
        for i, delta in deltas.items():
            deltas[i] = linear[i] @ delta
    return {name: (parent, frames[name][1]) for name, (parent, _) in joints.items()}
