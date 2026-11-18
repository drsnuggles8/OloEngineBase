"""Generate the Stacks light. Run in factory-startup Blender, in background mode."""

import hashlib
import math
import sys
from itertools import pairwise
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

# Blender's --python does not put the script directory on sys.path.
SOURCE = Path(__file__).resolve().parent
sys.path.insert(0, str(SOURCE))
from lighthouse_package import (
    create_staging_directory,
    finalize_descriptor,
    publish_package,
)
from lighthouse_surfaces import HOOD, HOOD_GUTTER_FRACTION, HOOD_V, Surfaces

DESTINATION = Path(
    sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else Path(__file__).parent
).resolve()
# The exporter can fail after writing a buffer or some of its images.
# Stage outside the editor project; publication does its own local file replacements.
staging = create_staging_directory()
ROOT = Path(staging.name)
bpy.ops.wm.read_factory_settings(use_empty=True)
TAU = math.tau


def material(name, color, roughness, metal=0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    surface = mat.node_tree.nodes.get("Principled BSDF")
    surface.inputs["Base Color"].default_value = (*color, 1)
    surface.inputs["Metallic"].default_value = metal
    surface.inputs["Roughness"].default_value = roughness
    return mat


surfaces = Surfaces(ROOT)
surfaces.build(SOURCE / "SurfaceSources.zip")
stone = surfaces.finish(
    "Salt-washed ashlar", "StacksMasonry", "StacksMasonryNormal", "StacksMasonryORM"
)
red = surfaces.finish(
    "Oxide daymark", "StacksOxide", "StacksMasonryNormal", "StacksMasonryORM"
)
stained = surfaces.finish(
    "Gallery rain wash", "StacksRunoff", "StacksMasonryNormal", "StacksMasonryORM"
)
base_stone = surfaces.finish(
    "Quarried plinth", "StacksBasalt", "StacksGraniteNormal", "StacksGraniteORM"
)
plinth = surfaces.finish(
    "Exposed foundation ashlar",
    "StacksPlinth",
    "StacksMasonryNormal",
    "StacksMasonryORM",
)
tread = surfaces.finish(
    "Boot-polished granite", "StacksTreads", "StacksGraniteNormal", "StacksTreadsORM"
)
deck = surfaces.finish("Tread plate", "StacksDeck", "StacksDeckNormal", "StacksDeckORM")
trim = surfaces.finish(
    "Dressed buff granite", "StacksFreestone", "StacksGraniteNormal", "StacksGraniteORM"
)
iron = surfaces.finish(
    "Graphite marine enamel", "StacksEnamel", "StacksMetalNormal", "StacksEnamelORM"
)
hood_sheet = surfaces.finish(
    "Folded copper sheet", "StacksHood", "StacksMetalNormal", "StacksHoodORM"
)
copper = surfaces.finish(
    "Weathered copper", "StacksCopper", "StacksMetalNormal", "StacksCopperORM"
)
brass = surfaces.finish(
    "Oiled bronze", "StacksBronze", "StacksMetalNormal", "StacksBronzeORM"
)
door = surfaces.finish("Tarred oak", "StacksOak", "StacksOakNormal", "StacksOakORM")
mortar = material("Recessed lime mortar", (0.17, 0.155, 0.13), 0.96)
pane = material("Bottle glass in shadow", (0.037, 0.099, 0.113), 0.16, 0.25)
glazing = material("Crown glass bevels", (0.22, 0.37, 0.35), 0.14)


def mesh(name, verts, faces, mat, uvs=None, smooth=False):
    data = bpy.data.meshes.new(name)
    data.from_pydata(verts, [], faces)
    data.materials.append(mat)
    if uvs:
        layer = data.uv_layers.new(name="Surface")
        for face, coords in zip(data.polygons, uvs, strict=True):
            for index, coord in zip(face.loop_indices, coords, strict=True):
                layer.data[index].uv = coord
    for face in data.polygons:
        face.use_smooth = smooth and len(face.vertices) == 4
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    return obj


def polar(radius, angle, z):
    return (radius * math.cos(angle), radius * math.sin(angle), z)


def revolve(name, rings, mat, sides=72, smooth=False, caps=True):
    wrap_radius = max(r for _, r in rings)
    verts = [polar(r, TAU * i / sides, z) for z, r in rings for i in range(sides)]
    faces, uvs = [], []
    # Unroll the profile. A horizontal rim still has a radial span to texture.
    length = rings[0][0]
    for row, (lower, upper) in enumerate(pairwise(rings)):
        bottom, top = length, length + math.dist(lower, upper)
        length = top
        for i in range(sides):
            j = (i + 1) % sides
            start = row * sides
            faces.append([start + i, start + j, start + sides + j, start + sides + i])
            uvs.append(
                [
                    (u * wrap_radius * TAU / 1.35, z / 1.35)
                    for u, z in [
                        (i / sides, bottom),
                        ((i + 1) / sides, bottom),
                        ((i + 1) / sides, top),
                        (i / sides, top),
                    ]
                ]
            )
    ends = [(0, range(sides - 1, -1, -1)), (len(rings) - 1, range(sides))]
    for row, indices in ends if caps else ():
        faces.append([row * sides + i for i in indices])
        uvs.append(
            [
                (
                    0.5 + 0.3 * math.cos(TAU * i / sides),
                    0.5 + 0.3 * math.sin(TAU * i / sides),
                )
                for i in indices
            ]
        )
    return mesh(name, verts, faces, mat, uvs, smooth)


def bar(name, p, q, radius, mat, sides=10):
    p, q = Vector(p), Vector(q)
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=sides, radius=radius, depth=(q - p).length, location=(p + q) * 0.5
    )
    obj = bpy.context.object
    obj.name = name
    obj.rotation_euler = (q - p).to_track_quat("Z", "Y").to_euler()
    obj.data.materials.append(mat)


def frame(angle, radius, lateral, z):
    return (
        radius * math.cos(angle) - lateral * math.sin(angle),
        radius * math.sin(angle) + lateral * math.cos(angle),
        z,
    )


def block(name, angle, radius, offset, z, depth, width, height, mat, bevel=0.018):
    bpy.ops.mesh.primitive_cube_add(size=1, location=frame(angle, radius, offset, z))
    obj = bpy.context.object
    obj.name = name
    obj.rotation_euler.z = angle
    obj.dimensions = (depth, width, height)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        edge = obj.modifiers.new("Worn arris", "BEVEL")
        edge.width = min(bevel, 0.43 * min(depth, width, height))
        edge.segments = 2
        bpy.ops.object.modifier_apply(modifier=edge.name)
    obj.data.materials.append(mat)


def shaft_radius(z):
    return 2.79 - 0.036 * z


def stair_tread(angle, index):
    # Passing boots wear a shallow hollow across the front half of each step.
    # The granite under the rail keeps its dressed edge.
    radius = 2.88 + index * 0.285
    top = 0.10 - index * 0.175
    verts = []
    for lateral in np.linspace(-0.84, 0.84, 15):
        wear = math.exp(-(((lateral - 0.04 * math.sin(index)) / 0.43) ** 2))
        profile = [
            (-0.20, -0.24),
            (0.20, -0.24),
            (0.20, -0.035 - 0.022 * wear),
            (0.191 - 0.012 * wear, -0.012 - 0.021 * wear),
            (0.170 - 0.018 * wear, -0.018 * wear),
            (-0.11, -0.003 * wear),
            (-0.20, 0.0),
        ]
        verts.extend(
            frame(angle, radius + depth, lateral, top + height)
            for depth, height in profile
        )
    n = len(profile)
    faces = [tuple(range(n)), tuple(range(len(verts) - 1, len(verts) - n - 1, -1))]
    for start in range(0, len(verts) - n, n):
        for i in range(n):
            j = (i + 1) % n
            faces.append((start + i, start + n + i, start + n + j, start + j))
    obj = mesh("Foot-worn granite tread", verts, faces, trim)
    project_surface(obj)
    data = obj.data
    data.materials.append(tread)
    chart = data.uv_layers["Surface"]
    for face in data.polygons:
        if face.normal.z < 0.35:
            continue
        face.material_index = 1
        for loop in face.loop_indices:
            x, y, _ = data.vertices[data.loops[loop].vertex_index].co
            lateral = -x * math.sin(angle) + y * math.cos(angle)
            radial = x * math.cos(angle) + y * math.sin(angle) - radius
            chart.data[loop].uv = (
                0.5 + lateral / 1.68,
                (index + (radial + 0.2) / 0.40) / 8,
            )


def stair_carriage(angle):
    # The return arch carries the stair between the pier and its outboard toe.
    outline = [(2.69, -9), (3.28, -9), (3.28, -2.98)]
    for t in np.linspace(math.pi, 0, 13)[1:]:
        outline.append((3.94 + 0.66 * math.cos(t), -2.98 + 0.66 * math.sin(t)))
    outline += [(4.60, -9), (5.05, -9), (5.05, -1.34), (2.69, 0.01)]
    verts = [frame(angle, r, s, z) for s in (-0.73, 0.73) for r, z in outline]
    n = len(outline)
    faces = [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
    side_uv = [(r / 3, z / 3) for r, z in outline]
    uvs = [side_uv, side_uv[::-1]]
    distance = 0.0
    for i in range(n):
        j = (i + 1) % n
        faces.append([i, n + i, n + j, j])
        end = distance + math.dist(outline[i], outline[j])
        uvs.append(
            [
                (0, distance / 3),
                (1.46 / 3, distance / 3),
                (1.46 / 3, end / 3),
                (0, end / 3),
            ]
        )
        distance = end
    mesh("Masonry stair arch", verts, faces, stone, uvs)


def project_surface(obj):
    """Metric UVs for the fittings. The shaft and spun profiles carry their own wrap."""
    data = obj.data
    if data.uv_layers.get("Surface"):
        return
    for layer in list(data.uv_layers):
        data.uv_layers.remove(layer)
    uv = data.uv_layers.new(name="Surface")
    seed = hashlib.blake2s(obj.name.encode(), digest_size=4).digest()
    du, dv = (
        int.from_bytes(seed[:2], "little") / 65536,
        int.from_bytes(seed[2:], "little") / 65536,
    )
    is_plank = obj.name.startswith("Oak leaf plank")
    for face in data.polygons:
        n = face.normal
        axis = max(range(3), key=lambda i: abs(n[i]))
        for loop in face.loop_indices:
            x, y, z = data.vertices[data.loops[loop].vertex_index].co
            if is_plank and axis == 0:
                # In the source photograph the grain follows image X. Each
                # plank samples the interior of one board, independently.
                u, v = z / 2.25 + du, (int(obj.name[-2:]) + 0.5) / 13 + y * 0.27
            else:
                u, v = ((y, z), (x, z), (x, y))[axis]
                u, v = u / 1.28 + du, v / 1.28 + dv
            uv.data[loop].uv = (u, v)


def clip_height(poly, boundary, keep_above):
    """Clip an unwrapped wall panel against a linear paint boundary."""
    result = []
    for i, (u, z) in enumerate(poly):
        v, h = poly[i - 1]
        before, after = h - boundary(v), z - boundary(u)
        if not keep_above:
            before, after = -before, -after
        if (before < 0) != (after < 0):
            t = before / (before - after)
            result.append((v + t * (u - v), h + t * (z - h)))
        if after >= 0:
            result.append((u, z))
    return result


def painted_shaft(heights, cuts):
    # Geometry carries the daymark outline; all finishes sample the SAME
    # continuous 3 m scan. Paint boundaries never break a masonry course.
    def zig(u):
        return 8.7 + 2.65 * abs(2 * ((2 * u + 0.12) % 1) - 1)

    bands = [
        (plinth, lambda u: 0, lambda u: 1.15),
        (stone, lambda u: 1.15, zig),
        (red, zig, lambda u: zig(u) + 2.75),
        (stone, lambda u: zig(u) + 2.75, lambda u: 15.10),
        (red, lambda u: 15.10, lambda u: 15.68),
        (stained, lambda u: 15.68, lambda u: 17.48),
    ]
    palette = [stone, red, plinth, stained]
    verts, faces, uvs, materials, lookup = [], [], [], [], {}
    angles = sorted({i / 144 for i in range(145)} | {0.19, 0.44, 0.69, 0.94})
    heights = sorted(set(heights) | {3, 6, 9, 12, 15})
    # Shift complete lifts across a photographed bed joint, keeping the daysign
    # fixed on the tower while breaking the repetition of the split faces.
    offsets = (0.0, 0.39, 0.73, 0.18, 0.56, 0.0)
    for z0, z1 in pairwise(heights):
        for left, right in pairwise(angles):
            mid = (left + right) * 180
            if any(
                z0 >= sill
                and z1 <= head
                and abs((mid - angle + 180) % 360 - 180) < half
                for angle, half, sill, head in cuts
            ):
                continue
            panel = [(left, z0), (right, z0), (right, z1), (left, z1)]
            for mat, lower, upper in bands:
                poly = clip_height(clip_height(panel, lower, True), upper, False)
                poly = list(dict.fromkeys((round(u, 9), round(z, 8)) for u, z in poly))
                if (
                    len(poly) < 3
                    or abs(
                        sum(
                            u * poly[i - 1][1] - z * poly[i - 1][0]
                            for i, (u, z) in enumerate(poly)
                        )
                    )
                    < 1e-10
                ):
                    continue
                face = []
                for u, z in poly:
                    key = (u % 1, z)
                    if key not in lookup:
                        lookup[key] = len(verts)
                        verts.append(polar(shaft_radius(z), TAU * u, z))
                    face.append(lookup[key])
                faces.append(face)
                uvs.append(
                    [
                        (6 * u + offsets[int(z0 // 3)], z / 3 + surfaces.bed_joint_v)
                        for u, z in poly
                    ]
                )
                materials.append(palette.index(mat))
    obj = mesh("Limewashed granite tower", verts, faces, stone, uvs)
    for mat in palette[1:]:
        obj.data.materials.append(mat)
    for poly, index in zip(obj.data.polygons, materials, strict=True):
        poly.material_index = index
        poly.use_smooth = True
    obj.data.normals_split_custom_set_from_vertices(
        [
            Vector((x / shaft_radius(z), y / shaft_radius(z), 0.036)).normalized()
            for x, y, z in verts
        ]
    )


def pier():
    # The dark bed is visible between the individually battered, chipped stones.
    revolve(
        "Mortar bed of the pier", [(-14, 3.25), (-1.1, 3.065), (0.05, 3.065)], mortar
    )
    rng = np.random.default_rng(878)
    verts, faces, uvs = [], [], []
    levels = np.linspace(-13.98, 0.035, 30)
    for row, (bottom, top) in enumerate(pairwise(levels)):
        widths = rng.uniform(0.78, 1.22, 19)
        angles = np.r_[0, np.cumsum(widths)] * TAU / widths.sum() + 0.167 * (row % 2)
        for a, b in pairwise(angles):
            radius = 3.127 + max(0, -top - 1.1) * 0.012 + rng.uniform(-0.008, 0.012)
            cuts = rng.uniform(0.012, 0.047, 4)
            face_width = radius * (b - a)
            high = top - bottom - 0.012
            w = face_width - 0.014
            outline = [
                (cuts[0], 0),
                (w - cuts[1], 0),
                (w, cuts[1]),
                (w, high - cuts[2]),
                (w - cuts[2], high),
                (cuts[3], high),
                (0, high - cuts[3]),
                (0, cuts[0]),
            ]
            start = len(verts)
            ox, oy = rng.random(2)
            for inset, bevel in ((-0.01, 0.08), (0.0, 0.018), (0.013, 0.0)):
                for x, y in outline:
                    xx = x + (1 - 2 * x / w) * inset
                    yy = y + (1 - 2 * y / high) * inset
                    verts.append(
                        polar(
                            radius - bevel,
                            a + (xx + 0.007) / radius,
                            bottom + 0.006 + yy,
                        )
                    )
            for ring, outer, inner in ((0, 0.084, 0.022), (8, 0.022, 0.0)):
                for i in range(8):
                    j = (i + 1) % 8
                    faces.append(tuple(start + ring + q for q in (i, j, j + 8, i + 8)))
                    # Extend the face chart outward, perpendicular to this
                    # edge. A fixed diagonal offset collapses chipped corners.
                    dx = outline[j][0] - outline[i][0]
                    dy = outline[j][1] - outline[i][1]
                    span = math.hypot(dx, dy)
                    uvs.append(
                        [
                            (
                                ox + (outline[k][0] + d * dy / span) / 1.28,
                                oy + (outline[k][1] - d * dx / span) / 1.28,
                            )
                            for k, d in ((i, outer), (j, outer), (j, inner), (i, inner))
                        ]
                    )
            faces.append(tuple(start + 16 + i for i in range(8)))
            uvs.append([(ox + x / 1.28, oy + y / 1.28) for x, y in outline])
    mesh("Split stone courses", verts, faces, base_stone, uvs)


def jamb_courses(a, radius, side, sill, head):
    count = max(3, round((head - sill) / 0.38))
    course = (head - sill) / count
    for i in range(count):
        width = 0.24 if i % 2 else 0.29
        block(
            "Bonded door and window jamb",
            a,
            radius - 0.012,
            side + math.copysign((width - 0.16) / 2, side),
            sill + (i + 0.5) * course,
            0.44 + 0.012 * (i % 2),
            width,
            course - 0.008,
            trim,
            0.012,
        )


def keeper_entry(a, r, width, sill, head):
    mid = (head + sill) / 2
    board = (width - 0.07) / 7
    for i in range(7):
        block(
            f"Oak leaf plank {i:02d}",
            a,
            r - 0.085,
            (i - 3) * board,
            mid,
            0.041,
            board - 0.006,
            head - sill - 0.15,
            door,
            0.006,
        )
    for z in (0.61, 1.65):
        block(
            "Forged strap hinge",
            a,
            r - 0.042,
            -width * 0.16,
            z,
            0.047,
            width * 0.55,
            0.065,
            iron,
            0.012,
        )
        for x in (-0.38, -0.23, -0.07, 0.08):
            bar(
                "Strap bolt",
                frame(a, r - 0.024, x * width, z),
                frame(a, r - 0.004, x * width, z),
                0.021,
                brass,
                8,
            )
    # A keeper can actually grasp the handle; the lower mount sits below it.
    latch = width * 0.32
    block("Latch backplate", a, r - 0.036, latch, 1.04, 0.042, 0.13, 0.31, iron)
    for z in (0.957, 1.123):
        bar(
            "Pull stand-off",
            frame(a, r - 0.02, latch, z),
            frame(a, r + 0.082, latch, z),
            0.022,
            brass,
        )
    bar(
        "Bronze pull",
        frame(a, r + 0.08, latch, 0.957),
        frame(a, r + 0.08, latch, 1.123),
        0.022,
        brass,
    )
    # Cast hinge barrels turn on continuous vertical pins at the hanging stile.
    for z in (0.61, 1.65):
        for dz in (-0.054, 0.054):
            side = -width * 0.425
            bar(
                "Hinge knuckle",
                frame(a, r - 0.013, side, z + dz - 0.040),
                frame(a, r - 0.013, side, z + dz + 0.040),
                0.036,
                iron,
                14,
            )
        bar(
            "Hinge pin",
            frame(a, r - 0.013, side, z - 0.12),
            frame(a, r - 0.013, side, z + 0.12),
            0.016,
            brass,
            12,
        )

    fanlight(a, r, width, head)
    station_plate(a, r)


def fanlight(a, r, width, head):
    # Slender leadwork radiates from the fanlight sill to the arch intrados.
    spring = head + 0.04
    trace = [
        frame(a, r + 0.111, width * 0.633 * math.cos(t), spring + 0.252 * math.sin(t))
        for t in np.linspace(0, math.pi, 37)
    ]
    mesh("Fanlight crown glass", trace, [tuple(range(len(trace)))], pane)
    for t in np.linspace(0, math.pi, 7):
        bar(
            "Fanlight lead",
            frame(a, r + 0.124, 0, spring),
            frame(
                a, r + 0.124, width * 0.63 * math.cos(t), spring + 0.25 * math.sin(t)
            ),
            0.010,
            brass,
            8,
        )
    for i in range(11):
        t0, t1 = math.pi * i / 11 + 0.005, math.pi * (i + 1) / 11 - 0.005
        key = i == 5
        vs = [
            frame(a, radius, width_scale * math.cos(t), spring + rise * math.sin(t))
            for radius in (r - 0.07, r + 0.17 + 0.037 * key)
            for width_scale, rise, t in (
                (width * 0.63, 0.25, t0),
                (width * 0.74, 0.49 + 0.045 * key, t0),
                (width * 0.74, 0.49 + 0.045 * key, t1),
                (width * 0.63, 0.25, t1),
            )
        ]
        wedge = mesh(
            "Keystone" if key else "Arch voussoir",
            vs,
            [
                (3, 2, 1, 0),
                (4, 5, 6, 7),
                (0, 1, 5, 4),
                (1, 2, 6, 5),
                (2, 3, 7, 6),
                (3, 0, 4, 7),
            ],
            trim,
        )
        bpy.context.view_layer.objects.active = wedge
        edge = wedge.modifiers.new("Chiselled arch edges", "BEVEL")
        edge.width, edge.segments = 0.011, 2
        bpy.ops.object.modifier_apply(modifier=edge.name)


def station_plate(a, r):
    # Cream vitreous letters stay legible inside the shadow of the lintel.
    z = 1.99
    block("Cast station plaque", a, r - 0.027, 0, z, 0.070, 1.03, 0.295, brass, 0.028)
    block(
        "Enamel lettering field", a, r + 0.006, 0, z, 0.023, 0.985, 0.256, iron, 0.012
    )
    for x in (-0.455, 0.455):
        for dz in (-0.089, 0.089):
            bar(
                "Plaque screw",
                frame(a, r + 0.017, x, z + dz),
                frame(a, r + 0.031, x, z + dz),
                0.014,
                brass,
                10,
            )
    ceramic = material("Ivory enamel lettering", (0.87, 0.79, 0.56), 0.34)
    for text, height, center in (
        ("S T A C K S", 0.149, z + 0.040),
        ("L I G H T   S T A T I O N", 0.050, z - 0.070),
    ):
        curve = bpy.data.curves.new("Foundry lettering", "FONT")
        curve.body = text
        curve.align_x, curve.align_y = "CENTER", "CENTER"
        curve.size, curve.extrude, curve.resolution_u = height, 0.0018, 3
        obj = bpy.data.objects.new("Enamel station legend", curve)
        bpy.context.collection.objects.link(obj)
        obj.location = frame(a, r + 0.019, 0, center)
        obj.rotation_euler = (math.pi / 2, 0, a + math.pi / 2)
        curve.materials.append(ceramic)
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.convert(target="MESH")


def drain_tube(path, angle):
    """Miter a hollow copper leader through bends in one vertical plane."""
    centers = [Vector(p) for p in path]
    directions = [(b - a).normalized() for a, b in pairwise(centers)]
    side = Vector((-math.sin(angle), math.cos(angle), 0))
    verts, faces, uvs = [], [], []
    count = 16
    distances = [0.0]
    for a, b in pairwise(centers):
        distances.append(distances[-1] + (b - a).length)
    for k, p in enumerate(centers):
        incoming = directions[max(0, k - 1)]
        outgoing = directions[min(k, len(directions) - 1)]
        tangent = (incoming + outgoing).normalized()
        # All segments share the bend plane. Stretch the section in that
        # plane to intersect the two circular cylinders at the same ring.
        miter = tangent.cross(side) / tangent.dot(incoming)
        for radius in (0.066, 0.055):
            for i in range(count):
                t = TAU * i / count
                verts.append(
                    tuple(p + radius * (math.cos(t) * side + math.sin(t) * miter))
                )
    for k in range(len(centers) - 1):
        for shell, radius in enumerate((0.066, 0.055)):
            a = (2 * k + shell) * count
            for i in range(count):
                j = (i + 1) % count
                face = (a + i, a + j, a + 2 * count + j, a + 2 * count + i)
                uv = [
                    (t * TAU * radius / 1.28, distances[n] / 1.28)
                    for t, n in (
                        (i / count, k),
                        ((i + 1) / count, k),
                        ((i + 1) / count, k + 1),
                        (i / count, k + 1),
                    )
                ]
                faces.append(face[::-1] if shell else face)
                uvs.append(uv[::-1] if shell else uv)
    for k in (0, len(centers) - 1):
        a = 2 * k * count
        for i in range(count):
            j = (i + 1) % count
            face = (a + i, a + j, a + count + j, a + count + i)
            uv = [
                (r * math.cos(TAU * n / count), r * math.sin(TAU * n / count))
                for n, r in ((i, 0.066), (j, 0.066), (j, 0.055), (i, 0.055))
            ]
            faces.append(face if k else face[::-1])
            uvs.append(uv if k else uv[::-1])
    mesh("Mitered copper conductor", verts, faces, copper, uvs, True)


def tower():
    cuts = [
        (60, 15, 0.12, 2.40),
        (-60, 10, 4.7, 5.95),
        (140, 10, 8.55, 9.8),
        (-40, 10, 12.4, 13.65),
        (60, 10, 15.75, 17.10),
    ]
    heights = sorted({0, 17.48, *(h for _, _, s, t in cuts for h in (s, t))})
    pier()
    painted_shaft(heights, cuts)
    for height, radius in [(0.05, 3.14), (17.30, 2.38)]:
        revolve(
            "Drip course",
            [
                (height - 0.18, radius - 0.13),
                (height - 0.10, radius),
                (height, radius),
                (height + 0.05, radius - 0.08),
            ],
            trim,
        )
    # The keeper's-side scupper feeds a rain conductor, kicked clear of the
    # projecting plinth at its foot. Brackets are pinned into the masonry.
    a = math.radians(23)

    def pipe_point(z):
        return polar(shaft_radius(z) + 0.105, a, z)

    path = [
        polar(2.91, a, 18.07),
        polar(2.91, a, 17.89),
        pipe_point(17.45),
        pipe_point(0.43),
        polar(3.23, a, 0.13),
        polar(3.24, a, -0.71),
        polar(3.48, a, -0.88),
    ]
    drain_tube(path, a)
    for z in np.arange(0.70, 17.4, 2.3):
        r = shaft_radius(z)
        block("Conductor anchor", a, r + 0.011, 0, z, 0.065, 0.27, 0.115, iron, 0.016)
        bar(
            "Pipe socket",
            pipe_point(z - 0.035),
            pipe_point(z + 0.045),
            0.081,
            brass,
            20,
        )
    for degrees, half, sill, head in cuts:
        a = math.radians(degrees)
        mid = (sill + head) / 2
        depth = shaft_radius(mid) * math.cos(math.radians(half))
        width = 2 * shaft_radius(sill) * math.sin(math.radians(half))
        entry = sill < 1
        block(
            "Oak entrance" if entry else "Stair light",
            a,
            depth - 0.17,
            0,
            mid,
            0.12,
            width + 0.05,
            head - sill + 0.08,
            door if entry else pane,
        )
        for side in (-1, 1):
            jamb_courses(a, depth, side * width / 2, sill, head)
        for z in (sill, head):
            block("Drip sill or lintel", a, depth, 0, z, 0.50, width + 0.41, 0.17, trim)
        if entry:
            keeper_entry(a, depth, width, sill, head)
            for i in range(8):
                stair_tread(a, i)
            stair_carriage(a)
            for side in (-1, 1):
                for i in (0, 4, 7):
                    r = 2.88 + i * 0.285
                    z = 0.10 - i * 0.175
                    bar(
                        "Entrance handrail newel",
                        frame(a, r, side * 0.76, z),
                        frame(a, r, side * 0.76, z + 0.94),
                        0.035,
                        iron,
                    )
                bar(
                    "Entrance handrail",
                    frame(a, 2.88, side * 0.76, 1.04),
                    frame(a, 2.88 + 7 * 0.285, side * 0.76, 1.04 - 7 * 0.175),
                    0.045,
                    iron,
                )
        else:
            block(
                "Vertical glazing bar",
                a,
                depth - 0.083,
                0,
                mid,
                0.035,
                0.045,
                head - sill,
                iron,
                bevel=0.010,
            )
            block("Transom", a, depth - 0.075, 0, mid + 0.10, 0.05, width, 0.05, iron)


def gallery():
    revolve(
        "Corbelled gallery",
        [
            (17.43, 2.18),
            (17.55, 2.4),
            (17.7, 2.43),
            (17.77, 2.68),
            (17.93, 2.70),
            (18.10, 3.02),
            (18.31, 3.02),
        ],
        trim,
    )
    revolve("Gallery nosing", [(18.28, 3.07), (18.37, 3.07), (18.39, 2.98)], iron)
    for i in range(12):
        # Replacement cast plates have expansion joints and countersunk pins.
        a0, a1 = TAU * i / 12 + 0.0015, TAU * (i + 1) / 12 - 0.0015
        angles = np.linspace(a0, a1, 7)
        verts = [polar(r, a, 18.405) for a in angles for r in (1.83, 2.965)]
        faces = [
            (2 * k, 2 * k + 1, 2 * k + 3, 2 * k + 2) for k in range(len(angles) - 1)
        ]
        chart = [
            [(verts[k][0] / 0.54, verts[k][1] / 0.54) for k in face] for face in faces
        ]
        mesh("Cast gallery floor", verts, faces, deck, chart)
        for rad in (2.02, 2.79):
            for a in (a0 + 0.036, a1 - 0.036):
                bar(
                    "Gallery plate pin",
                    polar(rad, a, 18.403),
                    polar(rad, a, 18.414),
                    0.026,
                    brass,
                    10,
                )
    for i in range(24):
        a = TAU * i / 24
        if i % 2 == 0:
            bar(
                "Gallery knee",
                polar(2.13, a, 16.59),
                polar(2.87, a, 18.07),
                0.087,
                iron,
            )
            block("Knee anchor", a, 2.20, 0, 16.67, 0.17, 0.20, 0.36, iron)
        bar("Stanchion", polar(2.90, a, 18.35), polar(2.90, a, 19.46), 0.037, iron)
        for z, r in [(18.86, 0.025), (19.44, 0.047)]:
            bar(
                "Gallery rail", polar(2.90, a, z), polar(2.90, a + TAU / 24, z), r, iron
            )


def lantern_frame():
    revolve(
        "Lantern apron",
        [
            (18.35, 1.86),
            (18.44, 1.94),
            (18.55, 1.81),
            (19.05, 1.81),
            (19.13, 1.90),
            (19.20, 1.84),
        ],
        copper,
        12,
    )
    for i in range(12):
        a = TAU * i / 12
        b = TAU * (i + 1) / 12
        # Represent the polished bevels of a clear sheet. Its colourless flat
        # centre has an unobstructed sightline to the optic on every render path.
        corners = [
            Vector(polar(1.75, a, 19.19)),
            Vector(polar(1.75, b, 19.19)),
            Vector(polar(1.75, b, 21.30)),
            Vector(polar(1.75, a, 21.30)),
        ]
        middle = sum(corners, Vector()) * 0.25
        bevels = []
        for point in corners:
            inner = point.lerp(middle, 0.042)
            inner.x *= 0.993
            inner.y *= 0.993
            bevels.append(inner)
        mesh(
            "Glazing bevel",
            corners + bevels,
            [(edge, (edge + 1) % 4, (edge + 1) % 4 + 4, edge + 4) for edge in range(4)],
            glazing,
        )
        bar(
            "Lantern astragal",
            polar(1.78, a, 19.18),
            polar(1.78, a, 21.32),
            0.055,
            iron,
        )
        bar(
            "Storm brace",
            polar(1.78, a, 19.19),
            polar(1.78, a + TAU / 12, 21.30),
            0.020,
            brass,
        )


def optic():
    revolve(
        "Lens pedestal",
        [(19.1, 0.42), (19.27, 0.47), (19.35, 0.26), (19.58, 0.26), (19.62, 0.56)],
        brass,
        36,
    )
    for z, r in [
        (19.64, 0.50),
        (19.84, 0.56),
        (20.01, 0.67),
        (20.60, 0.67),
        (20.77, 0.55),
        (20.94, 0.49),
    ]:
        inner = r - 0.026
        revolve(
            "Lens retaining ring",
            [
                (z - 0.022, inner),
                (z - 0.022, r),
                (z + 0.022, r),
                (z + 0.022, inner),
                (z - 0.022, inner),
            ],
            brass,
            48,
            caps=False,
        )
    for i in range(6):
        a = TAU * i / 6
        bar("Optic tie rod", polar(0.52, a, 19.61), polar(0.52, a, 20.94), 0.023, brass)
    # Narrow polished steps around the central lamp catch the sky between
    # highlights. The spaces between steps leave the night signal exposed.
    optical_glass = material("Prismatic flint glass", (0.38, 0.53, 0.49), 0.095, 0.40)
    optical_glass.node_tree.nodes["Principled BSDF"].inputs[
        "Emission Color"
    ].default_value = (0.66, 0.46, 0.23, 1)
    optical_glass.node_tree.nodes["Principled BSDF"].inputs[
        "Emission Strength"
    ].default_value = 0.55
    for step in (-6, -5, -4, -2, -1, 1, 2, 4, 5, 6):
        dz = step * 0.091
        z = 20.30 + dz
        r = 0.50 + 0.195 * math.sqrt(1 - (dz / 0.70) ** 2)
        revolve(
            "Annular optical prism",
            [
                (z - 0.014, r - 0.023),
                (z, r + 0.013),
                (z + 0.017, r - 0.021),
                (z - 0.014, r - 0.023),
            ],
            optical_glass,
            72,
            caps=False,
        )


def apron_fittings():
    for i in range(12):
        a = TAU * (i + 0.5) / 12
        # Pressed sheet lap, batten clips and two rows of copper fasteners.
        block("Apron lap", a, 1.758, 0.0, 18.79, 0.025, 0.050, 0.49, copper, 0.005)
        for z in (18.58, 18.97):
            for x in (-0.31, 0.0, 0.31):
                bar(
                    "Apron rivet",
                    frame(a, 1.752, x, z),
                    frame(a, 1.771, x, z),
                    0.028,
                    brass,
                    10,
                )
        for j in range(3):
            x = (j - 1) * 0.122
            block(
                "Vent louvre shadow",
                a,
                1.767,
                x,
                18.80,
                0.025,
                0.086,
                0.16,
                iron,
                0.009,
            )
            for z in (18.75, 18.80, 18.85):
                block(
                    "Pressed vent lip",
                    a,
                    1.788,
                    x,
                    z,
                    0.045,
                    0.102,
                    0.022,
                    copper,
                    0.005,
                )


def copper_hood():
    eave = [(21.28, 1.84), (21.36, 2.07), (21.49, 2.14), (21.58, 2.10), HOOD[0]]
    revolve("Rolled copper drip edge", eave, copper, 72, True)
    # Each developed sheet retains its width in metres on the tapered dome.
    # Sampling a narrowing strip avoids stretching patina around the crown.
    verts, faces, uvs = [], [], []
    divisions = 6
    for panel in range(12):
        start = len(verts)
        tile = (panel * 3 + panel // 4) % 4
        for z, r in HOOD:
            for k in range(divisions + 1):
                verts.append(polar(r, TAU * (panel + k / divisions) / 12, z))
        for row in range(len(HOOD) - 1):
            for k in range(divisions):
                i = start + row * (divisions + 1) + k
                faces.append((i, i + 1, i + divisions + 2, i + divisions + 1))
                coords = []
                for col, h in ((k, row), (k + 1, row), (k + 1, row + 1), (k, row + 1)):
                    across = 0.5 + (col / divisions - 0.5) * HOOD[h][1] / HOOD[0][1]
                    # Both atlas resolutions share this normalized gutter.
                    extent = 1 - 2 * HOOD_GUTTER_FRACTION
                    u = tile % 2 + HOOD_GUTTER_FRACTION + extent * across
                    v = tile // 2 + HOOD_GUTTER_FRACTION + extent * HOOD_V[h]
                    coords.append((u / 2, v / 2))
                uvs.append(coords)
    mesh("Rolled copper roof gores", verts, faces, hood_sheet, uvs, True)
    for i in range(12):
        a = i * TAU / 12
        for (z, r), (zz, rr) in pairwise(HOOD):
            bar(
                "Batten seam",
                polar(r + 0.020, a, z),
                polar(rr + 0.020, a, zz),
                0.034,
                copper,
                8,
            )
    for i in range(8):
        a = TAU * i / 8
        block("Ventilator slot", a, 0.305, 0, 23.025, 0.024, 0.100, 0.087, iron, 0.010)
    revolve(
        "Ventilator",
        [
            (22.85, 0.20),
            (22.93, 0.32),
            (23.10, 0.30),
            (23.16, 0.39),
            (23.35, 0.23),
            (23.43, 0.07),
            (24.12, 0.012),
        ],
        brass,
        32,
    )


tower()
gallery()
lantern_frame()
optic()
apron_fittings()
copper_hood()
for part in bpy.context.collection.objects:
    project_surface(part)
# One node at identity. The exporter groups disconnected parts by material.
bpy.ops.object.select_all(action="SELECT")
bpy.context.view_layer.objects.active = bpy.data.objects["Limewashed granite tower"]
bpy.ops.object.join()
bpy.context.object.name = "Stacks Light"
triangulate = bpy.context.object.modifiers.new("Export triangulation", "TRIANGULATE")
bpy.ops.object.modifier_apply(modifier=triangulate.name)
# The normal mapper needs a two-dimensional chart on every textured triangle,
# including the thin bevels underneath the masonry and horizontal optic rims.
data = bpy.context.object.data
chart = data.uv_layers["Surface"].data
for face in data.polygons:
    finish = data.materials[face.material_index]
    if finish.name in surfaces.finishes:
        a, b, c = (chart[i].uv for i in face.loop_indices)
        if abs((b - a).cross(c - a)) < 1e-12:
            raise ValueError(
                f"Collapsed UV triangle in {finish.name}, face {face.index}"
            )
bpy.ops.export_scene.gltf(
    filepath=str(ROOT / "StacksLight.gltf"),
    export_format="GLTF_SEPARATE",
    export_yup=True,
    export_cameras=False,
    export_lights=False,
    export_animations=False,
    # The static importer derives the tangent basis from these normals and UVs.
    # Omitting Blender's parallel tangent reduction also makes the buffer reproducible.
    export_tangents=False,
)

package = finalize_descriptor(ROOT, surfaces.finishes)
publish_package(ROOT, DESTINATION, package)
staging.cleanup()
