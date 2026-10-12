"""Author the InfiniteScanHead's centre-parted bob in Blender 5.2 (#1593).

Run with Blender's Python (bpy 5.2.2 also works):
    python build_human_hair.py --out <scratch>/Human.abc --blend <scratch>/Human.blend

The scan and rig are read only. Roots are area-sampled on the actual bind mesh;
locks follow the skull before hanging beside the face. All dimensions are metres.
Alembic contains root-first linear hair curves, diameters, guides and root UVs.
The historical Human groom space includes the GroomAnimals head placement
([0, 1.24, 3.6]); its binding and the fixture use the same space.
See README.md for provenance, reference photographs and cooking instructions.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

SEED = 1593
ORIGIN = np.array([0.0, 1.24, 3.6])
CENTRE = np.array([-0.004, 0.326, 0.005])
RADII = np.array([0.088, 0.106, 0.115])
POINTS = 32
DENSITY = 1_350_000  # per square metre of the accepted scalp surface


def mesh_data(path):
    """The committed glTF's one untransformed bind mesh, without DCC re-export."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    if len(doc["meshes"]) != 1 or len(doc["meshes"][0]["primitives"]) != 1:
        raise ValueError("expected the rigged scan's single primitive")
    primitive = doc["meshes"][0]["primitives"][0]
    buffers = [(path.parent / b["uri"]).read_bytes() for b in doc["buffers"]]

    def read(index):
        a = doc["accessors"][index]
        view = doc["bufferViews"][a["bufferView"]]
        dtype = {5126: np.float32, 5125: np.uint32, 5123: np.uint16}[a["componentType"]]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
        if "sparse" in a or view.get("byteStride", width * np.dtype(dtype).itemsize) != width * np.dtype(dtype).itemsize:
            raise ValueError("unsupported accessor layout")
        return np.frombuffer(buffers[view["buffer"]], dtype=dtype,
                             count=a["count"] * width,
                             offset=view.get("byteOffset", 0) + a.get("byteOffset", 0)).reshape(-1, width)

    attributes = primitive["attributes"]
    return (read(attributes["POSITION"]).astype(np.float64),
            read(attributes["NORMAL"]).astype(np.float64),
            read(attributes["TEXCOORD_0"]).astype(np.float64),
            read(primitive["indices"]).reshape(-1, 3).astype(np.int64))


def hairline(p):
    """Small irregularities and a centre dip keep the edge from forming a cap."""
    front = np.clip((p[..., 2] + 0.015) / 0.09, 0, 1)
    x = p[..., 0] + .004
    irregular = .0014 * np.sin(x * 470) + .0008 * np.sin(x * 1090 + 1.1)
    line = 0.245 + 0.126 * front ** 2 + irregular * front
    line -= .003 * np.exp(-(x / .009) ** 2) * front
    line = np.where(np.abs(p[..., 0] + 0.004) > 0.07, np.maximum(line, 0.322), line)
    return line


def scalp_mask(p):
    """A feathered frontal hairline, ear clearance and a narrow visible part."""
    line = hairline(p)
    part_centre = -.004 + .0007 * np.sin(p[..., 2] * 65)
    part = (np.abs(p[..., 0] - part_centre) < .00025) & (p[..., 2] > -0.05) & (p[..., 1] > 0.394)
    return (p[..., 1] > line) & ~part


def sample_roots(vertices, normals, uvs, triangles):
    rng = np.random.default_rng(SEED)
    ps = vertices[triangles]
    areas = 0.5 * np.linalg.norm(np.cross(ps[:, 1] - ps[:, 0], ps[:, 2] - ps[:, 0]), axis=1)
    possible = np.any(scalp_mask(ps), axis=1)
    centres = ps.mean(axis=1)
    part_band = (np.abs(centres[:, 0] - CENTRE[0]) < .025) & (centres[:, 1] > .37) & (centres[:, 2] > -.055)
    density = DENSITY * np.where(part_band, 2.5, 1.)
    counts = np.floor(areas * density + rng.random(len(areas))).astype(int)
    counts[~possible] = 0
    ids = np.repeat(np.arange(len(areas)), counts)
    a, b = rng.random((2, len(ids)))
    fold = a + b > 1
    a[fold], b[fold] = 1 - a[fold], 1 - b[fold]
    weights = np.stack([1 - a - b, a, b], axis=1)
    roots = np.einsum("ni,nij->nj", weights, ps[ids])
    ns = np.einsum("ni,nij->nj", weights, normals[triangles[ids]])
    uv = np.einsum("ni,nij->nj", weights, uvs[triangles[ids]])
    # Taper root density over the first three millimetres rather than stopping
    # the same dense long locks at a perfectly continuous forehead boundary.
    edge_density = np.clip((roots[:, 1] - hairline(roots)) / .003, 0, 1)
    accepted = scalp_mask(roots) & (ns[:, 1] > -0.25) & (rng.random(len(roots)) < edge_density)
    return roots[accepted], uv[accepted], rng


def lock_path(roots, noise):
    """Surface arc with lateral sweep, then a jaw-length hanging section."""
    q = (roots - CENTRE) / RADII
    theta = np.arccos(np.clip(q[:, 1], -1, 1))
    phi = np.arctan2(q[:, 0], q[:, 2])
    side = np.where(roots[:, 0] >= CENTRE[0], 1., -1.)
    # Front locks sweep to the temples. Back locks retain their azimuth.
    target_phi = side * np.maximum(np.abs(phi), 1.12 + noise * .14)
    # At the crown comb across the part, including its rear end. Radial flow
    # there would expose a star-shaped scalp whorl instead of a narrow part.
    crown = np.clip((.8 - theta) / .6, 0, 1)
    lateral = side * (1.57 - .45 * np.clip(q[:, 2], -1, 1))
    target_phi = target_phi * (1 - crown) + lateral * crown
    t = np.linspace(0, 1, 128)[None, :]
    arc = np.clip(t / 0.64, 0, 1)
    smooth = arc * arc * (3 - 2 * arc)
    # Nape roots already lie below the ear: never comb those upwards. The
    # varying exit latitude prevents a common ring where every lock drops.
    exit_theta = np.maximum(theta + .28, 1.54 + noise * .18)
    angles = theta[:, None] + (exit_theta - theta)[:, None] * arc
    sweep = 1 - (1 - arc) ** 3
    azimuth = phi[:, None] + (target_phi - phi)[:, None] * sweep
    lock_lift = .010 + noise * .007
    lift = lock_lift[:, None] * np.sin(math.pi * arc / 2) + .003 * np.sin(math.pi * arc)
    # Coherent bends belong to the lock, rather than independent noisy fibres.
    # They break the common spherical highlight without changing root coverage.
    lift += .004 * np.sin(2 * math.pi * arc + noise[:, None] * math.pi) * np.sin(math.pi * arc)
    radii = RADII[None, None, :] + lift[..., None]
    direction = np.stack([np.sin(angles) * np.sin(azimuth), np.cos(angles),
                          np.sin(angles) * np.cos(azimuth)], axis=-1)
    path = CENTRE + radii * direction
    # Preserve the actual scanned root, fading its ellipsoid residual over the arc.
    path += (roots - path[:, 0])[:, None, :] * (1 - smooth)[..., None]
    free = np.clip((t - .64) / .36, 0, 1)
    exit_point = path[:, -1].copy()
    tip = exit_point.copy()
    tip[:, 1] = .192 + noise * .018
    tip[:, 0] -= side * (.012 + noise * .008)
    tip[:, 2] -= .003 + noise * .006
    # Hermite continuation takes the scalp tangent at departure. Arc-length
    # resampling below makes the segment lengths continuous through the join.
    exit_direction = np.stack([np.sin(exit_theta) * np.sin(target_phi), np.cos(exit_theta),
                               np.sin(exit_theta) * np.cos(target_phi)], axis=-1)
    theta_derivative = np.stack([np.cos(exit_theta) * np.sin(target_phi), -np.sin(exit_theta),
                                np.cos(exit_theta) * np.cos(target_phi)], axis=-1)
    tangent = ((RADII[None, :] + lock_lift[:, None]) * theta_derivative * (exit_theta - theta)[:, None]
               - .003 * math.pi * exit_direction)
    tangent /= np.linalg.norm(tangent, axis=1)[:, None]
    distance = np.maximum(exit_point[:, 1] - tip[:, 1], .025)
    m0 = tangent * distance[:, None]
    m1 = np.stack([-side * distance * (.55 + noise * .3), -distance,
                   distance * (.08 + noise * .2)], axis=-1)
    f = free[..., None]
    hanging = ((2*f**3 - 3*f**2 + 1) * exit_point[:, None] +
               (f**3 - 2*f**2 + f) * m0[:, None] +
               (-2*f**3 + 3*f**2) * tip[:, None] + (f**3 - f**2) * m1[:, None])
    path = np.where((t > .64)[..., None], hanging, path)
    result = np.empty((len(roots), POINTS, 3))
    cumulative = np.concatenate([np.zeros((len(roots), 1)),
                                 np.cumsum(np.linalg.norm(np.diff(path, axis=1), axis=2), axis=1)], axis=1)
    for i in range(len(roots)):
        # More controls close to the scalp prevent the first straight segment
        # from cutting through its curvature; retain the full hanging section.
        samples = np.linspace(0, 1, POINTS) ** 1.9 * cumulative[i, -1]
        for axis in range(3):
            result[i, :, axis] = np.interp(samples, cumulative[i], path[i, :, axis])
    return result


def author(path):
    vertices, normals, uvs, triangles = mesh_data(path)
    roots, uv, rng = sample_roots(vertices, normals, uvs, triangles)
    # Stable Voronoi locks, separated at the part so no lock crosses the face.
    lock_ids = np.zeros(len(roots), dtype=int)
    anchors = []
    for sign in (-1, 1):
        members = np.flatnonzero(np.where(roots[:, 0] >= CENTRE[0], 1, -1) == sign)
        chosen = members[::150]
        offset = len(anchors)
        anchors.extend(roots[chosen])
        lock_ids[members] = offset + cKDTree(roots[chosen]).query(roots[members])[1]
    anchors = np.asarray(anchors)
    lock_noise = rng.uniform(-1, 1, len(anchors))
    base = lock_path(roots, lock_noise[lock_ids])
    centreline = lock_path(anchors, lock_noise)[lock_ids]
    t = np.linspace(0, 1, POINTS)[None, :, None]
    # Each lock gathers gradually; retain its fine fibres and a little tip spread.
    attraction = (.72 - .25 * t ** 5) * np.sin(math.pi * t / 2) ** 1.5
    points = base * (1 - attraction) + centreline * attraction
    points += (roots - points[:, 0])[:, None, :] * (1 - t) ** 5
    phases = rng.uniform(0, 2 * math.pi, (len(roots), 1))
    ripple = .00045 * np.sin(np.linspace(0, 2.5 * math.pi, POINTS)[None, :] + phases)
    points[..., 0] += ripple * t[..., 0]
    points[..., 2] += ripple * .6 * t[..., 0]
    # Break up the end of each lock without adding independent noisy kinks.
    # Shortening along its own tangent keeps the authored shoulder clearance.
    tip_tangent = points[:, -1] - points[:, -2]
    tip_tangent /= np.linalg.norm(tip_tangent, axis=1)[:, None]
    points -= tip_tangent[:, None] * rng.uniform(0, .012, (len(roots), 1, 1)) * t ** 6
    # Short emergence hairs soften the frontal boundary. They follow the same
    # swept curve, ending on the scalp rather than hanging in front of the eyes.
    emergence = ((roots[:, 1] - hairline(roots) < .007) &
                 (roots[:, 2] > .035) & (rng.random(len(roots)) < .6))
    for index in np.flatnonzero(emergence):
        strand = points[index]
        distances = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(strand, axis=0), axis=1))])
        samples = np.linspace(0, rng.uniform(.012, .028), POINTS)
        points[index] = np.stack([np.interp(samples, distances, strand[:, axis]) for axis in range(3)], axis=1)
    # Ellipsoid flow is a styling scaffold, not the scanned scalp. Keep the
    # sampled roots exact and project interior non-root controls onto the
    # actual surface with root volume, especially around the part. The solver
    # follows a capsule fit of the head: a nearly flush rest arc can sag through
    # the scanned scalp while its root remains correctly bound. Keep the first
    # control on the surface and give the combed section room to move.
    import bpy
    from mathutils.bvhtree import BVHTree
    from mathutils import Vector
    surface = BVHTree.FromPolygons(vertices.tolist(), triangles.tolist(), all_triangles=True)
    # Short regrowth lies underneath the long combed locks at the part. Comb
    # these fine fibres backwards along the scanned scalp; the long locks alone
    # diverge laterally and leave an unnaturally broad transparent root stripe.
    regrowth_ids = np.flatnonzero((np.abs(roots[:, 0] - CENTRE[0]) < .014) &
                                 (roots[:, 1] > .37) & (roots[:, 2] > -.055))
    short_roots = roots[regrowth_ids]
    short = np.repeat(short_roots[:, None], POINTS, axis=1)
    short_t = np.linspace(0, 1, POINTS)[None, :]
    short[..., 2] -= rng.uniform(.018, .035, (len(short), 1)) * short_t
    short[..., 0] += rng.uniform(-.004, .004, (len(short), 1)) * short_t
    for strand in short:
        for i in range(1, POINTS):
            hit, normal, _, _ = surface.find_nearest(tuple(strand[i]))
            strand[i] = np.asarray(hit) + np.asarray(normal) * .006 * min(i / 3, 1)
    projected = 0
    max_correction = 0.
    for strand in points:
        for i in range(1, POINTS):
            p = strand[i]
            if p[1] < .24:
                continue
            hit, normal, _, _ = surface.find_nearest(tuple(p))
            clearance = (Vector(p) - hit).dot(normal)
            desired = .006 if p[1] > .34 else .001
            if clearance < desired:
                correction = desired - clearance
                strand[i] += np.asarray(normal) * correction
                projected += 1
                max_correction = max(max_correction, correction)
    diameters = rng.uniform(.000065, .000095, (len(roots), 1))
    taper = 1 - .72 * np.linspace(0, 1, POINTS)[None, :] ** 3
    widths = diameters * taper
    # One guide per lock, in source order. Do not choose guides after cooking.
    guides = np.zeros(len(roots), dtype=np.int32)
    _, first = np.unique(lock_ids, return_index=True)
    guides[first] = 1
    short_guides = np.zeros(len(short), dtype=np.int32)
    short_guides[::40] = 1
    short_widths = rng.uniform(.00006, .000085, (len(short), 1)) * (1 - .8 * short_t ** 2)
    points = np.concatenate([points, short])
    widths = np.concatenate([widths, short_widths])
    uv = np.concatenate([uv, uv[regrowth_ids]])
    guides = np.concatenate([guides, short_guides])
    if not np.isfinite(points).all() or not np.isfinite(widths).all():
        raise ValueError("non-finite authored curve")
    return points, widths, uv, guides, len(anchors), {"controlsProjected": projected, "maxCorrection": max_correction,
                                                     "regrowthStrands": len(short)}


def export(points, widths, uv, guides, output, blend, regrowth):
    import bpy
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("HumanHair")
    bpy.context.scene.collection.children.link(collection)
    for name, role, rows in [("Scalp_longhair", 4, slice(0, len(points) - regrowth)),
                             ("Part_regrowth", 1, slice(len(points) - regrowth, None))]:
        cv = bpy.data.hair_curves.new(name)
        cv.add_curves([POINTS] * len(points[rows]))
        # Blender (Z up, face -Y), converted to Y-up by its Alembic exporter.
        world = points[rows] + ORIGIN
        blender = np.stack([world[..., 0], -world[..., 2], world[..., 1]], axis=-1)
        cv.attributes["position"].data.foreach_set("vector", blender.reshape(-1).astype(np.float32))
        radius = cv.attributes.new("radius", "FLOAT", "POINT")
        radius.data.foreach_set("value", (.5 * widths[rows]).reshape(-1).astype(np.float32))
        cv.set_types(type="POLY")
        guide = cv.attributes.new("groom_guide", "INT", "CURVE")
        guide.data.foreach_set("value", guides[rows])
        root_uv = cv.attributes.new("surface_uv_coordinate", "FLOAT2", "CURVE")
        root_uv.data.foreach_set("vector", np.stack([uv[rows, 0], 1 - uv[rows, 1]], axis=-1).reshape(-1).astype(np.float32))
        cv["groom_role"] = role
        cv["groom_tint"] = [1., 1., 1.]
        cv["groom_clump"] = 0.  # locks are already authored into the curves
        cv["groom_stiffness"] = 1.
        cv["groom_guide"] = guides[rows].tolist()
        cv["groom_root_uv"] = uv[rows].reshape(-1).tolist()
        cv["groom_card_cell_size"] = .007
        cv["groom_card_points"] = 12
        ob = bpy.data.objects.new(name, cv)
        collection.objects.link(ob)
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.alembic_export(filepath=str(output.resolve()), collection=collection.name,
                              selected=False, start=1, end=1, export_hair=True,
                              export_particles=False, export_custom_properties=True,
                              uvs=False, normals=False, vcolors=False, orcos=False,
                              face_sets=False, flatten=False, evaluation_mode="RENDER")
    if blend:
        blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(blend.resolve()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--blend", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    mesh = repo / "OloEditor/assets/models/InfiniteScanHead/HeadRigged.gltf"
    points, widths, uv, guides, locks, clearance = author(mesh)
    export(points, widths, uv, guides, args.out, args.blend, clearance["regrowthStrands"])
    import bpy
    import scipy
    buffers = json.loads(mesh.read_text(encoding="utf-8"))["buffers"]
    report = {"recipe": SEED, "strands": len(points), "pointsPerStrand": POINTS,
              "tools": {"python": platform.python_version(), "blender": bpy.app.version_string,
                        "numpy": np.__version__, "scipy": scipy.__version__},
              "locks": locks, "guides": int(guides.sum()),
              "scalpClearance": clearance,
              "generatorSha256": hashlib.sha256(Path(__file__).read_text(encoding="utf-8").encode("utf-8")).hexdigest(),
              "meshSha256": hashlib.sha256(mesh.read_bytes()).hexdigest(),
              "bufferSha256": {b["uri"]: hashlib.sha256((mesh.parent / b["uri"]).read_bytes()).hexdigest()
                               for b in buffers},
              "curveSha256": hashlib.sha256(points.astype('<f4').tobytes() + widths.astype('<f4').tobytes()).hexdigest(),
              "boundsMin": points.min(axis=(0, 1)).tolist(), "boundsMax": points.max(axis=(0, 1)).tolist()}
    args.out.with_suffix(".recipe.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
