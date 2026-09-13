#!/usr/bin/env python3
"""Generate the LONG-COATED quadruped stand-in for the reference fixtures (issue #1239).

WHY THIS EXISTS AT ALL, AND WHAT IT IS NOT
------------------------------------------
Issue #1239 asks for short- and long-coated animal benchmark fixtures. The
short-coated one is a real asset: `assets/models/Fox/Fox.gltf` (Khronos
glTF-Sample-Assets), skinned and animated, with the coat painted into the
albedo texture the way a shipping short-coated character does it.

There is NO long-coated animal asset available to this repository, and one
cannot be bought into it (the issue's own scope: "No asset purchase is
assumed"). So this script generates the honest stand-in: a quadruped whose
long coat is a SOLID OUTER SHELL offset from the body, which is exactly what
the engine can express today.

That shell is the measurement, not a placeholder for one. It is what a long
coat looks like with no groom system: a hard silhouette, no strand-level
detail, no anisotropic specular, no motion. Issues #1232 (groom curve assets)
and #1241 replace it, and the fixture's job is to be the "before" they are
measured against. Do NOT improve the coat here to make the capture look
better — that is the feature owners' work, and improving it in the fixture
would destroy the baseline.

Authoring conventions (match generate_vegetation.py, and stated in the
fixture manifests' provenance block):
  * metres, +Y up, FEET AT Y=0 so a scene places it by ground position;
  * facing +X (the head end), so a manifest camera at yaw 90 looks it in the
    face and yaw 0 gives the grazing side-on profile;
  * every vertex carries a UV. Assimp does not synthesise missing texture
    coordinates, and Model::ProcessMesh leaves Vertex::TexCoord at (0,0)
    without them — an untextured flat silhouette.

Byte-stable output: fixed seed, no timestamps, deterministic float
formatting, LF newlines. Regenerate deliberately and commit the result.

Run from anywhere:  python generate_reference_animals.py
"""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))

SEED = 1239


def noise(i, j, salt=0):
    """Deterministic [0,1) hash noise.

    A hand-rolled integer hash rather than `random`, so the output cannot move
    if a future CPython changes the Mersenne Twister's stream — the .obj is
    committed and a silent regeneration diff would be pure noise in review.
    """
    h = (i * 73856093) ^ (j * 19349663) ^ ((SEED + salt) * 83492791)
    h &= 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return h / 0x100000000


class Mesh:
    """Vertex/UV/face accumulator. OBJ indices are 1-based."""

    def __init__(self):
        self.verts = []
        self.uvs = []
        self.faces = []

    def v(self, x, y, z, u, w):
        self.verts.append((x, y, z))
        self.uvs.append((u, w))
        return len(self.verts)

    def tri(self, a, b, c):
        self.faces.append((a, b, c))

    def quad(self, a, b, c, d):
        self.tri(a, b, c)
        self.tri(a, c, d)

    def tube(self, rings, segments=16, u_scale=1.0, v0=0.0, v1=1.0, cap_start=True, cap_end=True):
        """Lathe a tube through `rings`, each (centre, radius_y, radius_z, jitter).

        Rings run along X (the animal's length axis), so a cross-section is an
        ellipse in the YZ plane — a quadruped body is wider than it is tall,
        and a circular section reads as a sausage.
        """
        ring_indices = []
        for ri, (centre, ry, rz, jitter) in enumerate(rings):
            v = v0 + (v1 - v0) * (ri / max(1, len(rings) - 1))
            idx = []
            for s in range(segments):
                ang = (s / segments) * 2.0 * math.pi
                wobble = 1.0 + jitter * (noise(ri, s) - 0.5) * 2.0 if jitter else 1.0
                y = centre[1] + math.cos(ang) * ry * wobble
                z = centre[2] + math.sin(ang) * rz * wobble
                u = (s / segments) * u_scale
                idx.append(self.v(centre[0], y, z, u, v))
            ring_indices.append(idx)

        for ri in range(len(ring_indices) - 1):
            a_ring, b_ring = ring_indices[ri], ring_indices[ri + 1]
            for s in range(segments):
                s2 = (s + 1) % segments
                self.quad(a_ring[s], a_ring[s2], b_ring[s2], b_ring[s])

        # Caps are fans to a centre vertex, so the silhouette closes — an open
        # tube end shows through as a hole under any backlit camera.
        if cap_start:
            c0 = rings[0][0]
            hub = self.v(c0[0], c0[1], c0[2], 0.5, v0)
            for s in range(segments):
                s2 = (s + 1) % segments
                self.tri(hub, ring_indices[0][s2], ring_indices[0][s])
        if cap_end:
            c1 = rings[-1][0]
            hub = self.v(c1[0], c1[1], c1[2], 0.5, v1)
            for s in range(segments):
                s2 = (s + 1) % segments
                self.tri(hub, ring_indices[-1][s], ring_indices[-1][s2])
        return ring_indices

    def write(self, path, name, notes):
        with open(path, "w", newline="\n", encoding="utf-8") as f:
            f.write(f"# {name} — generated by generate_reference_animals.py (issue #1239). Do not hand-edit.\n")
            for line in notes:
                f.write(f"# {line}\n")
            f.write(f"o {name}\n")
            for x, y, z in self.verts:
                f.write(f"v {x:.4f} {y:.4f} {z:.4f}\n")
            for u, w in self.uvs:
                f.write(f"vt {u:.4f} {w:.4f}\n")
            for a, b, c in self.faces:
                f.write(f"f {a}/{a} {b}/{b} {c}/{c}\n")
        tris = len(self.faces)
        print(f"  {os.path.basename(path)}: {len(self.verts)} verts, {tris} tris")
        return tris


# Body profile along +X: (x, radius_y, radius_z). Shoulder is thicker than
# hindquarters, the chest drops lower than the back line.
BODY_PROFILE = [
    (-0.62, 0.055, 0.050),  # tail root
    (-0.50, 0.130, 0.120),
    (-0.30, 0.180, 0.165),  # hindquarters
    (-0.10, 0.185, 0.175),
    (0.10, 0.195, 0.185),  # barrel
    (0.30, 0.205, 0.190),  # shoulder
    (0.46, 0.165, 0.150),
    (0.56, 0.110, 0.100),  # base of neck
]

BODY_CENTRE_Y = 0.52  # belly clears the ground; legs make up the rest


def body_rings(scale=1.0, jitter=0.0, y_lift=0.0):
    return [
        ((x, BODY_CENTRE_Y + y_lift, 0.0), ry * scale, rz * scale, jitter)
        for (x, ry, rz) in BODY_PROFILE
    ]


def leg(mesh, x, z, top_y, foot_y=0.0, top_r=0.058, foot_r=0.036):
    """One tapered leg. Rings run along X in `tube`, so a leg is built by hand."""
    segments = 8
    rings = 4
    ring_indices = []
    for r in range(rings):
        t = r / (rings - 1)
        y = top_y + (foot_y - top_y) * t
        rad = top_r + (foot_r - top_r) * t
        idx = []
        for s in range(segments):
            ang = (s / segments) * 2.0 * math.pi
            idx.append(
                mesh.v(x + math.cos(ang) * rad, y, z + math.sin(ang) * rad, s / segments, 1.0 - t)
            )
        ring_indices.append(idx)
    for r in range(rings - 1):
        a_ring, b_ring = ring_indices[r], ring_indices[r + 1]
        for s in range(segments):
            s2 = (s + 1) % segments
            mesh.quad(a_ring[s], a_ring[s2], b_ring[s2], b_ring[s])
    hoof = mesh.v(x, foot_y, z, 0.5, 0.0)
    for s in range(segments):
        s2 = (s + 1) % segments
        mesh.tri(hoof, ring_indices[-1][s], ring_indices[-1][s2])


def build_long_coated():
    """Body + head + legs + tail, wrapped in the long-coat shell."""
    mesh = Mesh()

    # ---- Under-body (the animal inside the coat) ------------------------
    mesh.tube(body_rings(), segments=16, v0=0.0, v1=1.0)

    # ---- Neck and head, facing +X ---------------------------------------
    # The neck rises out of the shoulder and the head angles DOWN from it. An
    # earlier revision ran the head out horizontally at the top of the neck,
    # which read as a beak in the frontal capture — wrong silhouette for a
    # long-coated quadruped, and the silhouette is what this fixture measures.
    neck = [
        ((0.56, 0.62, 0.0), 0.085, 0.080, 0.0),
        ((0.66, 0.70, 0.0), 0.075, 0.070, 0.0),
        ((0.74, 0.76, 0.0), 0.070, 0.066, 0.0),
    ]
    mesh.tube(neck, segments=12, v0=0.0, v1=0.4, cap_start=False)
    head = [
        ((0.74, 0.77, 0.0), 0.078, 0.072, 0.0),
        ((0.84, 0.74, 0.0), 0.070, 0.062, 0.0),
        ((0.93, 0.68, 0.0), 0.042, 0.038, 0.0),  # muzzle
        ((0.98, 0.64, 0.0), 0.026, 0.024, 0.0),
    ]
    mesh.tube(head, segments=12, v0=0.4, v1=1.0, cap_start=False)

    # ---- Legs (front pair under the shoulder, rear under the haunch) ----
    for x, z in ((0.30, 0.115), (0.30, -0.115), (-0.28, 0.120), (-0.28, -0.120)):
        leg(mesh, x, z, top_y=BODY_CENTRE_Y - 0.02)

    # ---- Tail ------------------------------------------------------------
    tail = [
        ((-0.62, 0.56, 0.0), 0.040, 0.038, 0.0),
        ((-0.74, 0.48, 0.0), 0.032, 0.030, 0.10),
        ((-0.84, 0.36, 0.0), 0.026, 0.024, 0.16),
        ((-0.90, 0.24, 0.0), 0.018, 0.017, 0.22),
    ]
    mesh.tube(tail, segments=10, v0=0.0, v1=1.0, cap_start=False)

    # ---- THE LONG COAT ---------------------------------------------------
    # An outer shell at ~1.45x the body section with a ragged lower edge. This
    # is the whole point of the fixture: the coat is CLOSED OPAQUE GEOMETRY,
    # so it has a hard silhouette against the sky, no strand translucency, and
    # no response to wind. A groom replaces exactly this shell.
    coat_rings = []
    for ri, (x, ry, rz) in enumerate(BODY_PROFILE):
        if ri == 0:
            continue  # the coat stops short of the tail root
        # Skirt: the coat hangs BELOW the body, so the shell centre drops and
        # its vertical radius grows faster than its horizontal one.
        drop = 0.085 * math.sin(math.pi * (ri / (len(BODY_PROFILE) - 1)))
        coat_rings.append(
            (
                (x, BODY_CENTRE_Y - drop, 0.0),
                ry * 1.45 + drop * 0.9,
                rz * 1.40,
                0.16,  # ragged edge — a coat is not a smooth extrusion
            )
        )
    mesh.tube(coat_rings, segments=20, v0=0.0, v1=1.0, cap_start=True, cap_end=True)

    # A ruff around the neck: the second place a long coat reads as volume.
    ruff = [
        ((0.48, 0.58, 0.0), 0.22, 0.21, 0.20),
        ((0.58, 0.65, 0.0), 0.19, 0.18, 0.22),
        ((0.68, 0.71, 0.0), 0.14, 0.13, 0.18),
    ]
    mesh.tube(ruff, segments=16, v0=0.0, v1=1.0, cap_start=False, cap_end=False)

    return mesh


def main():
    out_dir = HERE
    os.makedirs(out_dir, exist_ok=True)
    print("generate_reference_animals.py (issue #1239)")

    mesh = build_long_coated()
    mesh.write(
        os.path.join(out_dir, "longcoat-quadruped.obj"),
        "longcoat-quadruped",
        [
            "Long-coated quadruped STAND-IN for the #1239 reference fixtures.",
            "The coat is a solid offset shell — that is the current-limitation",
            "baseline the groom work (#1232/#1241) is measured against, not a",
            "modelling shortcut to be tidied up later.",
            "Units: metres. Up: +Y. Facing: +X. Feet at Y=0.",
        ],
    )


if __name__ == "__main__":
    main()
