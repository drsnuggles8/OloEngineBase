"""Generate the two static Drift harbour assets in metres with Blender in background mode."""

import math
import filecmp
import json
import os
from pathlib import Path
import random
import shutil
import sys
import tempfile

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from crane_surfaces import bake, bind_maps

TAU = math.tau
bpy.ops.wm.read_factory_settings(use_empty=True)
rng = random.Random(1717)


def finish(name, color, metal, rough):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1)
    shader.inputs["Metallic"].default_value = metal
    shader.inputs["Roughness"].default_value = rough
    return mat


paint = finish("Bottle green foundry enamel", (0.045, 0.133, 0.103), 0.45, 0.47)
oxide = finish("Red lead and oxide", (0.24, 0.069, 0.029), 0.26, 0.76)
steel = finish("Burnished working steel", (0.35, 0.36, 0.32), 0.85, 0.28)
grease = finish("Oiled black iron", (0.064, 0.065, 0.054), 0.74, 0.36)
brass = finish("Gunmetal bearing bronze", (0.36, 0.22, 0.082), 0.77, 0.36)
wood = finish("Sawn and salt bleached oak", (0.37, 0.25, 0.14), 0, 0.81)
soaked = finish("Tide darkened oak", (0.17, 0.13, 0.085), 0, 0.76)
endgrain = finish("Crosscut oak", (0.33, 0.23, 0.13), 0, 0.83)
rope = finish("Laid running wire", (0.26, 0.24, 0.19), 0.67, 0.38)
polished_brass = finish("Rubbed brass highlights", (0.50, 0.34, 0.15), 0.87, 0.24)


class Mesh:
    def __init__(self):
        self.verts = []
        self.faces = []
        self.uvs = []
        self.finishes = []
        self.indices = []
        self.smooth = []

    def ring(self, points):
        start = len(self.verts)
        self.verts.extend(points)
        return list(range(start, len(self.verts)))

    def face(self, indices, material, uv, smooth=False):
        if material not in self.finishes:
            self.finishes.append(material)
        self.indices.append(self.finishes.index(material))
        self.faces.append(indices)
        self.uvs.extend(uv)
        self.smooth.append(smooth)

    def loft(self, rings, material, smooth=False, closed=False, strip_materials=None):
        # Physical UVs: U runs around a section, V follows a timber or shaft.
        ids = [self.ring(row) for row in rings]
        u0, v0 = rng.random(), rng.random()
        # Crop inside one of the thirteen scanned boards on each long face.
        # Grain follows the timber's axis even when the jib slopes.
        wood_rows = [rng.randrange(13) for _ in rings[0]] if material in (wood, soaked) else []
        v = v0
        for i in range(len(ids) if closed else len(ids) - 1):
            k = (i + 1) % len(ids)
            u = u0
            dv = (rings[k][0] - rings[i][0]).length
            for j in range(len(ids[i])):
                jj = (j + 1) % len(ids[i])
                du = (rings[i][jj] - rings[i][j]).length
                chart = [
                    (u / 0.75, v / 0.75),
                    ((u + du) / 0.75, v / 0.75),
                    ((u + du) / 0.75, (v + dv) / 0.75),
                    (u / 0.75, (v + dv) / 0.75),
                ]
                if wood_rows:
                    centre = ((wood_rows[j] + 0.5) / 13 + 0.038) % 1
                    half_width = min(0.031, du / 5.5)
                    chart = [
                        (v / 4, centre - half_width),
                        (v / 4, centre + half_width),
                        ((v + dv) / 4, centre + half_width),
                        ((v + dv) / 4, centre - half_width),
                    ]
                elif material == rope:
                    # One repeat around the line, one along a 150 mm rope lay.
                    start, end = j / len(ids[i]), (j + 1) / len(ids[i])
                    chart = [
                        (start, v / 0.15),
                        (end, v / 0.15),
                        (end, (v + dv) / 0.15),
                        (start, (v + dv) / 0.15),
                    ]
                self.face(
                    [ids[i][j], ids[i][jj], ids[k][jj], ids[k][j]],
                    (strip_materials or {}).get(i, material),
                    chart,
                    smooth,
                )
                u += du
            v += dv
        if not closed:
            for row, indices in [(rings[0][::-1], ids[0][::-1]), (rings[-1], ids[-1])]:
                u = (row[1] - row[0]).normalized()
                normal = max((u.cross(p - row[0]) for p in row[2:]), key=lambda n: n.length_squared)
                v = normal.normalized().cross(u)
                if wood_rows and material == wood:
                    middle = sum(row, Vector()) / len(row)
                    self.face(
                        indices,
                        endgrain,
                        [
                            (
                                (p - middle).dot(u) * 1.35 + 0.25 + 0.5 * u0,
                                (p - middle).dot(v) * 1.35 + 0.25 + 0.5 * v0,
                            )
                            for p in row
                        ],
                    )
                else:
                    self.face(indices, material, [(p.dot(u) + u0, p.dot(v) + v0) for p in row])

    def export(self, directory, name):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(self.verts, [], self.faces)
        for mat in self.finishes:
            mesh.materials.append(mat)
        mesh.polygons.foreach_set("material_index", self.indices)
        mesh.polygons.foreach_set("use_smooth", self.smooth)
        layer = mesh.uv_layers.new(name="Metre chart")
        layer.uv.foreach_set("vector", [v for pair in self.uvs for v in pair])
        mesh.update()
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.export_scene.gltf(
            filepath=str(directory / (name + ".gltf")),
            export_format="GLTF_SEPARATE",
            use_selection=True,
            export_yup=True,
            export_animations=False,
            export_tangents=False,
            export_extras=False,
        )
        obj.select_set(False)
        bind_maps(
            directory / (name + ".gltf"),
            {
                paint.name: ("Foundry", (1, 1, 1)),
                wood.name: ("QuayOak", (1, 1, 1)),
                soaked.name: ("QuayOak", (0.39, 0.46, 0.40)),
                endgrain.name: ("OakEnd", (1, 1, 1)),
                rope.name: ("RunningWire", (1, 1, 1)),
                grease.name: ("WroughtIron", (0.50, 0.55, 0.53)),
                steel.name: ("ToolSteel", (1, 1, 1)),
                brass.name: ("WorkedBronze", (1, 1, 1)),
                oxide.name: ("WroughtIron", (0.86, 0.37, 0.20)),
            },
        )


model = Mesh()


def axes(direction):
    axis = Vector(direction).normalized()
    ref = Vector((0, 1, 0)) if abs(axis.y) < 0.92 else Vector((1, 0, 0))
    u = (ref - axis * axis.dot(ref)).normalized()
    return u, axis.cross(u)


def turning(start, direction, profile, material, sides=40, closed=False, strips=None):
    axis = Vector(direction).normalized()
    u, v = axes(axis)
    rays = [u * math.cos(TAU * j / sides) + v * math.sin(TAU * j / sides) for j in range(sides)]
    model.loft(
        [[Vector(start) + axis * d + r * ray for ray in rays] for d, r in profile],
        material,
        sides >= 16,
        closed,
        strips,
    )


def bar(a, b, r, mat, sides=16):
    delta = Vector(b) - Vector(a)
    turning(a, delta, [(0, r), (delta.length, r)], mat, sides)


def timber(a, b, width, depth, mat=wood, bevel=0.012):
    u, v = axes(Vector(b) - Vector(a))
    w, h = width / 2, depth / 2
    section = [
        (w - bevel, h),
        (-w + bevel, h),
        (-w, h - bevel),
        (-w, -h + bevel),
        (-w + bevel, -h),
        (w - bevel, -h),
        (w, -h + bevel),
        (w, h - bevel),
    ]
    model.loft([[Vector(end) + x * u + y * v for x, y in section] for end in (a, b)], mat)


def plate(outline, y, thick, mat):
    # A counter-clockwise X/Z outline has its normal toward -Y.
    area = sum(x * z2 - x2 * z for (x, z), (x2, z2) in zip(outline, outline[1:] + outline[:1]))
    points = outline if area < 0 else outline[::-1]
    # A narrow draft and edge radius catch light on each cast cheek. The
    # outlines can be concave, so inset each edge along its own bisector.
    bevel = min(0.010, thick * 0.14) if mat == paint else min(0.0025, thick * 0.07)
    rim = [Vector(p) for p in points]
    inner = []
    for i, p in enumerate(rim):
        side1 = (p - rim[i - 1]).normalized()
        side2 = (rim[(i + 1) % len(rim)] - p).normalized()
        n1, n2 = Vector((side1.y, -side1.x)), Vector((side2.y, -side2.x))
        bisect = (n1 + n2).normalized()
        inner.append(p + bisect * bevel / max(0.40, bisect.dot(n1)))
    section = [
        (inner, y - thick / 2),
        (rim, y - thick / 2 + bevel),
        (rim, y + thick / 2 - bevel),
        (inner, y + thick / 2),
    ]
    model.loft([[Vector((x, side, z)) for x, z in edge] for edge, side in section], mat)


def axle(x, z, y0, y1, radius, mat, n=36):
    bar((x, y0, z), (x, y1, z), radius, mat, n)


def forged_stay(a, b, radius):
    """Upset shoulders and forged eyes seat the rod on both transverse pins."""
    a, b = Vector(a), Vector(b)
    axis = (b - a).normalized()
    length = (b - a).length
    turning(
        a,
        axis,
        [
            (0.053, radius * 1.46),
            (0.11, radius * 1.34),
            (0.21, radius),
            (length - 0.21, radius),
            (length - 0.11, radius * 1.34),
            (length - 0.053, radius * 1.46),
        ],
        grease,
        24,
    )
    for p in (a, b):
        facing = 1 if p.y > 0 else -1
        turning(
            p,
            (0, facing, 0),
            [
                (-0.020, 0.046),
                (-0.020, 0.073),
                (-0.015, 0.080),
                (0.012, 0.080),
                (0.020, 0.072),
                (0.020, 0.046),
            ],
            grease,
            36,
            True,
        )


def builder_plate(a, b):
    # A curved-corner builder's cartouche sits on the operator-facing side
    # of the oak jib. The raised letters and screw slots survive relighting.
    run = (b - a).normalized()
    up = Vector((-run.z, 0, run.x))
    out = Vector((0, -1, 0))
    origin = a + (b - a) * 0.37 + out * 0.134

    def project(x, y, depth):
        return origin + run * x + up * y + out * depth

    def rim(w, h, r, depth):
        points = []
        for sx, sy, start in [(1, 1, 0), (-1, 1, 90), (-1, -1, 180), (1, -1, 270)]:
            for j in range(7):
                angle = math.radians(start + 90 * j / 6)
                points.append(
                    project(
                        sx * (w / 2 - r) + r * math.cos(angle),
                        sy * (h / 2 - r) + r * math.sin(angle),
                        depth,
                    )
                )
        return points

    model.loft(
        [rim(0.68, 0.218, 0.034, 0.001), rim(0.68, 0.218, 0.034, 0.009), rim(0.67, 0.21, 0.032, 0.014)],
        brass,
    )
    model.loft([rim(0.651, 0.191, 0.027, 0.014), rim(0.651, 0.191, 0.027, 0.015)], grease)

    def inscription(label, height, baseline):
        font = bpy.data.curves.new("Foundry die", "FONT")
        font.body, font.align_x, font.size = label, "CENTER", height
        font.extrude, font.bevel_depth, font.bevel_resolution = 0.0015, 0.00035, 1
        font.resolution_u = 4
        obj = bpy.data.objects.new("Lettering pattern", font)
        bpy.context.collection.objects.link(obj)
        obj_eval = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        glyphs = obj_eval.to_mesh()
        glyphs.calc_loop_triangles()
        points = [v.co.copy() for v in glyphs.vertices]
        ids = model.ring([project(p.x, p.y + baseline, 0.017 + p.z) for p in points])
        for tri in glyphs.loop_triangles:
            # The extruded letter walls are edge-on in the face chart. Project
            # each triangle on its dominant plane so the importer can build
            # a tangent frame even when the plaque is assigned a new material.
            dominant = max(range(3), key=lambda axis: abs(tri.normal[axis]))
            u, v = (dominant + 1) % 3, (dominant + 2) % 3
            model.face(
                [ids[j] for j in tri.vertices],
                polished_brass,
                [(points[j][u], points[j][v]) for j in tri.vertices],
            )
        obj_eval.to_mesh_clear()
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.curves.remove(font)

    inscription("RIDGEBACK", 0.090, -0.002)
    inscription("H A R B O U R   W O R K S", 0.032, -0.051)
    for x in (-0.295, 0.295):
        p = project(x, 0, 0.015)
        turning(p, out, [(0, 0.013), (0.003, 0.013), (0.007, 0.009)], brass, 20)
        bar(
            p + out * 0.007 - run * 0.007 - up * 0.003,
            p + out * 0.007 + run * 0.007 + up * 0.003,
            0.0012,
            grease,
            6,
        )


def keeper(x, y, z, side):
    """Key a pin into its boss with a washer, hex and protruding stud."""
    turning((x, y, z), (0, side, 0), [(0, 0.068), (0.012, 0.068), (0.016, 0.059)], grease, 32)
    turning((x, y + side * 0.016, z), (0, side, 0), [(0, 0.050), (0.032, 0.050), (0.035, 0.044)], oxide, 6)
    turning((x, y + side * 0.049, z), (0, side, 0), [(0, 0.027), (0.014, 0.027), (0.017, 0.023)], steel, 20)


def ratchet():
    x, y, z = -1.375, 0.447, 1.22
    rim = []
    for j in range(18):
        for fraction, rad in [(0, 0.122), (0.77, 0.166), (0.94, 0.166)]:
            a = TAU * (j + fraction) / 18
            rim.append((x + rad * math.cos(a), z + rad * math.sin(a)))
    plate(rim, y, 0.035, grease)
    plate(
        [
            (-1.50, 1.34),
            (-1.46, 1.37),
            (-1.30, 1.435),
            (-1.22, 1.431),
            (-1.21, 1.396),
            (-1.27, 1.388),
            (-1.415, 1.351),
            (-1.45, 1.318),
        ],
        y,
        0.030,
        steel,
    )
    plate([(-1.32, 1.33), (-1.26, 1.45), (-1.19, 1.445), (-1.18, 1.32)], 0.384, 0.063, paint)
    axle(-1.24, 1.411, 0.345, 0.483, 0.026, brass)


def pipe(points, radius, mat, sides=12):
    rows = []
    for i, pt in enumerate(points):
        tangent = Vector(points[min(i + 1, len(points) - 1)]) - Vector(points[max(i - 1, 0)])
        u, v = axes(tangent)
        r = radius[i] if isinstance(radius, list) else radius
        rows.append(
            [
                Vector(pt) + r * (u * math.cos(TAU * j / sides) + v * math.sin(TAU * j / sides))
                for j in range(sides)
            ]
        )
    model.loft(rows, mat, True)


def wheel(x, z, y, r, teeth, phase):
    # Open web with a continuous toothed rim. Both gears use the same circular pitch.
    rows = [[] for _ in range(4)]
    for tooth in range(teeth):
        for fraction, dr in [(0, -0.016), (0.16, -0.016), (0.32, 0.015), (0.61, 0.015), (0.79, -0.016)]:
            a = TAU * (tooth + fraction) / teeth + phase
            dx, dz = math.cos(a), -math.sin(a)
            for row, (rad, side) in zip(
                rows,
                [(r + dr, y - 0.035), (r + dr, y + 0.035), (r * 0.83, y + 0.035), (r * 0.83, y - 0.035)],
            ):
                row.append(Vector((x + dx * rad, side, z + dz * rad)))
    model.loft(rows, grease, closed=True, strip_materials={0: steel})
    # Flared cast arms flow into the rim and hub. The small pinion has
    # a solid web; a second set of six spokes left little metal at its root.
    if teeth < 20:
        axle(x, z, y - 0.022, y + 0.022, r * 0.89, grease)
    else:
        for j in range(6):
            a = TAU * j / 6
            radial = Vector((math.cos(a), 0, math.sin(a)))
            tangent = Vector((-math.sin(a), 0, math.cos(a)))
            rings = []
            for t, w, offset in [
                (0.13, 0.039, 0),
                (0.27, 0.034, 0.008),
                (0.49, 0.022, 0.014),
                (0.69, 0.021, 0.008),
                (0.78, 0.027, 0.003),
                (0.86, 0.041, 0),
            ]:
                centre = Vector((x, y, z)) + radial * (r * t) + tangent * offset
                rings.append(
                    [
                        centre
                        + tangent * (w * math.cos(TAU * k / 12))
                        + Vector((0, -0.022 * math.sin(TAU * k / 12), 0))
                        for k in range(12)
                    ]
                )
            model.loft(rings, paint, True)
    turning(
        (x, y, z),
        (0, 1, 0),
        [(-0.061, 0.058), (-0.055, 0.074), (-0.035, 0.078), (0.035, 0.078), (0.055, 0.074), (0.061, 0.058)],
        grease,
        36,
    )


def tackle():
    # Common external tangents of the drum, head guide and jib sheave.
    circles = [(-0.82, 1.22, 0.210), (0, 3.47, 0.164), (4.62, 4.82, 0.236)]
    normals = []
    for (x, z, r), (xx, zz, rr) in zip(circles, circles[1:]):
        normals.append(math.atan2(zz - z, xx - x) + math.acos((r - rr) / math.hypot(xx - x, zz - z)))

    def point(c, a, y=0):
        x, z, r = c
        return (x + r * math.cos(a), y, z + r * math.sin(a))

    turns = 10
    path = [
        point(circles[0], normals[0] + TAU * turns * (1 - t / 360), -0.25 * (1 - t / 360))
        for t in range(361)
    ]
    for i, end in [(1, normals[1]), (2, 0)]:
        start = normals[i - 1]
        path.extend(point(circles[i], start + (end - start) * t / 24) for t in range(25))
    path.append((4.856, 0, 0.76))
    pipe(path, 0.0125, rope)
    for x, z, r in circles[1:]:
        # Lower running surface between raised lips; the line lies on the groove.
        profile = [
            (-0.046, r + 0.021),
            (-0.028, r + 0.021),
            (-0.013, r - 0.008),
            (0, r - 0.013),
            (0.013, r - 0.008),
            (0.028, r + 0.021),
            (0.046, r + 0.021),
            (0.046, r * 0.71),
            (-0.046, r * 0.71),
        ]
        turning((x, 0, z), (0, 1, 0), profile, grease, 48, True, {2: steel, 3: steel})
        for j in range(6):
            a = TAU * j / 6
            bar((x, 0, z), (x + r * 0.8 * math.cos(a), 0, z + r * 0.8 * math.sin(a)), r * 0.084, grease)
        reach = 0.23 if x > 0 else 0.17
        axle(x, z, -reach, reach, 0.048, steel)
        for side in (-1, 1):
            keeper(x, side * reach, z, side)
    x = 4.856
    turning((x, 0, 0.57), (0, 0, 1), [(0, 0.035), (0.045, 0.047), (0.18, 0.05), (0.22, 0.022)], grease)
    # A forged hook with a deep throat, swollen bowl and tapered return.
    pts = [
        (x, 0, 1.36),
        (x, 0, 1.25),
        (x - 0.034, 0, 1.18),
        (x - 0.108, 0, 1.10),
        (x - 0.14, 0, 1.02),
        (x - 0.123, 0, 0.95),
        (x - 0.065, 0, 0.904),
        (x + 0.014, 0, 0.90),
        (x + 0.089, 0, 0.95),
        (x + 0.125, 0, 1.035),
        (x + 0.122, 0, 1.097),
    ]
    pipe(
        [(px, py, pz - 0.77) for px, py, pz in pts],
        [0.029, 0.036, 0.043, 0.052, 0.055, 0.054, 0.05, 0.044, 0.032, 0.019, 0.004],
        steel,
        20,
    )


def crane():
    turning(
        (0, 0, 0),
        (0, 0, 1),
        [
            (0, 0.62),
            (0.07, 0.62),
            (0.10, 0.59),
            (0.12, 0.39),
            (0.22, 0.35),
            (0.28, 0.32),
            (0.33, 0.29),
            (2.92, 0.178),
            (3.10, 0.205),
            (3.15, 0.205),
        ],
        paint,
        64,
    )
    for j in range(8):
        a = TAU * (j + 0.5) / 8
        x, y = 0.48 * math.cos(a), 0.48 * math.sin(a)
        bar((x, y, 0.097), (x, y, 0.112), 0.062, oxide, 24)
        bar((x, y, 0.112), (x, y, 0.156), 0.048, grease, 6)
    turning((0, 0, 0), (0, 0, 1), [(0.29, 0.337), (0.32, 0.36), (0.44, 0.36), (0.47, 0.337)], grease)
    turning((0, 0, 0), (0, 0, 1), [(0.47, 0.35), (0.50, 0.37), (0.63, 0.37), (0.66, 0.35)], paint)
    turning(
        (0, 0, 0),
        (0, 0, 1),
        [(2.98, 0.196), (2.99, 0.217), (3.025, 0.222), (3.17, 0.218), (3.19, 0.205)],
        paint,
    )
    axle(0, 3.10, -0.267, 0.267, 0.049, steel)
    for side in (-1, 1):
        keeper(0, side * 0.252, 3.10, side)
    # Paired frame castings support both shafts and land at the rotating collar.
    for s in (-1, 1):
        y = s * 0.345
        outline = [
            (-1.55, 1.30),
            (-1.48, 1.385),
            (-0.75, 1.385),
            (0.55, 0.68),
            (0.52, 0.43),
            (0.07, 0.43),
            (-0.83, 0.92),
            (-1.46, 1.02),
            (-1.55, 1.12),
        ]
        plate(outline, y, 0.073, paint)
        timber((-1.43, y, 1.09), (-0.07, y, 0.55), 0.10, 0.094, paint)
        plate([(-1.34, 1.31), (-1.31, 1.46), (-1.27, 1.49), (-1.21, 1.48), (-1.13, 1.32)], y, 0.067, paint)
        axle(-1.264, 1.417, y - s * 0.022, y + s * 0.097, 0.045, steel)
        forged_stay((-1.264, y + s * 0.059, 1.417), (0, s * 0.213, 3.10), 0.031)
        forged_stay((0, s * 0.252, 3.10), (4.62, s * 0.192, 4.82), 0.035)
        keeper(-1.264, y + s * 0.082, 1.417, s)
        for x, z in [(-1.375, 1.22), (-0.82, 1.22), (0.47, 0.58)]:
            axle(x, z, y - 0.062, y + 0.062, 0.088, paint)
            axle(x, z, y - 0.073, y + 0.073, 0.059, brass)
        # Grease cups thread into the two pillow blocks. Brown oxide tails
        # lie on the casting beneath each cover bolt, following gravity.
        for x in (-1.375, -0.82):
            turning(
                (x, y, 1.32),
                (0, 0, 1),
                [(0, 0.019), (0.055, 0.019), (0.06, 0.033), (0.083, 0.032), (0.091, 0.022)],
                brass,
                20,
            )
        for x, z in [(-1.46, 1.21), (-0.99, 1.22), (-0.56, 1.09), (0.34, 0.585)]:
            length = rng.uniform(0.043, 0.078)
            plate(
                [(x - 0.017, z), (x + 0.022, z), (x + 0.003, z - length), (x - 0.003, z - 0.065)],
                y + s * 0.037,
                0.002,
                oxide,
            )
            axle(x, z, y + s * 0.035, y + s * 0.046, 0.029, grease, 24)
            axle(x, z, y + s * 0.043, y + s * 0.064, 0.022, oxide, 6)
        plate(
            [(-0.18, 2.94), (0.16, 2.94), (0.13, 3.49), (0.06, 3.56), (-0.075, 3.56), (-0.155, 3.47)],
            s * 0.112,
            0.044,
            paint,
        )
    for x, z, r in [(-1.375, 1.22, 0.044), (-0.82, 1.22, 0.06), (0.47, 0.58, 0.061)]:
        axle(x, z, -0.59, 0.57, r, steel)
    turning(
        (-0.82, 0, 1.22),
        (0, 1, 0),
        [(-0.305, 0.242), (-0.282, 0.25), (-0.266, 0.198), (0.246, 0.198), (0.259, 0.25), (0.282, 0.25)],
        grease,
    )
    wheel(-0.82, 1.22, -0.464, 0.41625, 48, 0)
    wheel(-1.375, 1.22, -0.464, 0.13875, 16, TAU * 0.5 / 16)
    keeper(-0.82, -0.59, 1.22, -1)
    for s in (-1, 1):
        keeper(0.47, s * 0.58, 0.58, s)
    ratchet()
    # A lever compresses the anchored split friction band around the drum shaft.
    turning(
        (-0.82, 0, 1.22),
        (0, 1, 0),
        [
            (0.415, 0.071),
            (0.425, 0.117),
            (0.420, 0.182),
            (0.415, 0.196),
            (0.421, 0.215),
            (0.505, 0.215),
            (0.513, 0.207),
            (0.513, 0.181),
            (0.496, 0.168),
            (0.493, 0.102),
            (0.515, 0.085),
            (0.530, 0.069),
        ],
        grease,
        64,
        strips={4: steel, 5: steel, 6: steel},
    )
    for i in range(4):
        angle = TAU * (i + 0.125) / 4
        axle(-0.82 + 0.132 * math.cos(angle), 1.22 + 0.132 * math.sin(angle), 0.491, 0.505, 0.020, steel, 6)
    band = [
        (-0.82 + 0.224 * math.cos(a), 0.47, 1.22 + 0.224 * math.sin(a))
        for a in [math.radians(111 + 319 * i / 48) for i in range(49)]
    ]
    # A flat band bears on the width of the machined brake drum. Two ears
    # carry the fixed anchor and the moving end of the short bell crank.
    rows = []
    for px, py, pz in band:
        radial = Vector((px + 0.82, 0, pz - 1.22)).normalized()
        rows.append(
            [
                Vector((px, py + dy, pz)) + radial * dr
                for dy, dr in [(-0.038, -0.009), (0.038, -0.009), (0.038, 0.005), (-0.038, 0.005)]
            ]
        )
    model.loft(rows, oxide)
    pivot = (-0.976, 0.48, 1.474)
    anchor = (-1.079, 0.47, 1.38)
    bar(band[0], anchor, 0.021, grease)
    axle(anchor[0], anchor[2], 0.325, 0.515, 0.031, brass)
    plate(
        [(-1.10, 1.32), (-1.03, 1.51), (-0.985, 1.54), (-0.945, 1.516), (-0.90, 1.324)], 0.380, 0.069, paint
    )
    axle(pivot[0], pivot[2], 0.345, 0.525, 0.037, brass)
    keeper(pivot[0], 0.521, pivot[2], 1)
    tip = (-0.714, 0.48, 1.433)
    bar(band[-1], tip, 0.018, grease)
    plate(
        [
            (-1.575, 1.615),
            (-1.565, 1.656),
            (-0.973, 1.505),
            (-0.707, 1.456),
            (-0.703, 1.412),
            (-0.988, 1.449),
        ],
        0.48,
        0.042,
        grease,
    )
    axle(tip[0], tip[2], 0.445, 0.52, 0.024, brass)
    turning(
        (-1.54, 0.48, 1.623),
        (-1, 0, 0.25),
        [(0, 0.025), (0.015, 0.032), (0.16, 0.034), (0.178, 0.028), (0.18, 0.020)],
        wood,
        28,
    )
    # Offset the crank from the gear face and the opposite brake lever.
    timber((-1.375, -0.58, 1.22), (-1.585, -0.58, 0.997), 0.053, 0.064, paint, 0.009)
    turning(
        (-1.585, -0.59, 0.997),
        (0, -1, 0),
        [
            (0, 0.027),
            (0.021, 0.035),
            (0.025, 0.033),
            (0.033, 0.039),
            (0.065, 0.037),
            (0.146, 0.031),
            (0.177, 0.038),
            (0.181, 0.036),
            (0.184, 0.039),
            (0.196, 0.029),
        ],
        brass,
        32,
        strips={3: polished_brass, 4: polished_brass, 5: polished_brass},
    )
    # The wood stops short of the wheel envelope. The two strapped iron
    # cheeks continue past its end, leaving an open throat for rope and rim.
    a, b = Vector((0.47, 0, 0.58)), Vector((4.39, 0, 4.40))
    timber(a, b, 0.268, 0.34, wood, 0.040)
    builder_plate(a, b)
    for t, length in [(0.06, 0.105), (0.80, 0.036), (0.938, 0.070)]:
        half = (b - a).normalized() * length
        timber(a + (b - a) * t - half, a + (b - a) * t + half, 0.294, 0.365, paint, 0.04)
    for s in (-1, 1):
        plate(
            [
                (4.015, 4.117),
                (4.13, 4.045),
                (4.46, 4.36),
                (4.75, 4.71),
                (4.76, 4.85),
                (4.67, 4.925),
                (4.55, 4.915),
                (4.47, 4.80),
                (4.25, 4.48),
            ],
            s * 0.149,
            0.035,
            paint,
        )
        for x, z in [(4.15, 4.17), (4.34, 4.36)]:
            axle(x, z, s * 0.132, s * 0.19, 0.034, grease, 24)
            axle(x, z, s * 0.184, s * 0.205, 0.028, brass, 6)
    tackle()


def footing():
    # Nine metres of fendered face. Long piles penetrate the measured underwater slope.
    for y in (-3.68, -1.84, 0, 1.84, 3.68):
        for x in (-2.46, 0.02, 1.64):
            wet_line = -1.10 + rng.uniform(-0.09, 0.09)
            timber((x, y, -8.65), (x, y, wet_line), 0.32, 0.34, soaked, 0.038)
            timber((x, y, wet_line), (x, y, -0.652), 0.32, 0.34, wood, 0.038)
        timber((-2.97, y, -0.497), (1.85, y, -0.497), 0.26, 0.31)
    for x in (-2.55, -1.13, 0.25, 1.64):
        timber((x, -4.4, -0.232), (x, 4.4, -0.232), 0.18, 0.22)
    for j, y in enumerate((-3.68, -1.84, 0, 1.84)):
        start, end = (y, y + 1.84) if j % 2 == 0 else (y + 1.84, y)
        timber((1.49, start, -1.69), (1.49, end, -0.55), 0.123, 0.167, soaked, 0.012)
    # Each board spans the stringers from the shore toward the fender rail.
    for j in range(40):
        y = (j - 19.5) * 0.221
        timber(
            (-3.06, y, -0.061), (1.86 + rng.uniform(-0.012, 0.012), y, -0.061), 0.210, 0.122, wood, 0.010
        )
        for x in (-2.52, -1.13, 0.25, 1.62):
            bar((x, y, -0.001), (x, y, 0.003), 0.016, grease, 10)
    for z in (-0.39, -0.93):
        timber((1.86, -4.43, z), (1.86, 4.43, z), 0.19, 0.24)
    for y in (-3.8, -1.92, 0, 1.92, 3.8):
        timber((1.96, y, -1.72), (1.96, y, -0.13), 0.166, 0.18, wood, 0.025)
        for z in (-0.39, -0.93):
            bar((2.033, y, z), (2.061, y, z), 0.045, oxide, 16)
            bar((2.061, y, z), (2.083, y, z), 0.030, grease, 6)
    for y in (-3.65, 3.65):
        turning(
            (0.97, y, 0),
            (0, 0, 1),
            [
                (0, 0.221),
                (0.048, 0.221),
                (0.075, 0.157),
                (0.12, 0.114),
                (0.32, 0.107),
                (0.36, 0.149),
                (0.392, 0.154),
                (0.410, 0.130),
            ],
            grease,
        )
    # Bedplate sits flush with the deck, through-bolted over the central pile bent.


def publish(staged, destination):
    # Validate both descriptors before publishing any file to the editor's
    # watched project. Publish the descriptors after their dependencies.
    descriptors = sorted(staged.glob("*.gltf"))
    names = set()
    for descriptor in descriptors:
        doc = json.loads(descriptor.read_text(encoding="utf-8"))
        for buffer in doc["buffers"]:
            path = staged / buffer["uri"]
            if path.stat().st_size != buffer["byteLength"]:
                raise ValueError(f"Partial buffer: {path}")
            names.add(path.name)
        for image in doc["images"]:
            path = staged / image["uri"]
            if not path.is_file():
                raise ValueError(f"Missing texture: {path}")
            names.add(path.name)
    destination.mkdir(parents=True, exist_ok=True)
    recovery = destination / ".harbour-publish"
    recovery.mkdir()  # One publisher per destination, including across processes.
    changes = []
    replaced = []
    try:
        for name in sorted(names) + [p.name for p in descriptors]:
            src, dst = staged / name, destination / name
            if dst.is_symlink() or (dst.exists() and not dst.is_file()):
                raise ValueError(f"Expected a regular output file: {dst}")
            if dst.is_file():
                if filecmp.cmp(src, dst, shallow=False):
                    continue
                shutil.copy2(dst, recovery / (name + ".previous"))
            shutil.copyfile(src, recovery / (name + ".pending"))
            changes.append(name)
        # The plan is complete before any destination file changes. Both
        # backups and pending payloads have extensions the editor ignores.
        (recovery / "plan.pending").write_text(json.dumps(changes), encoding="utf-8")
        os.replace(recovery / "plan.pending", recovery / "transaction.plan")
        for name in changes:
            os.replace(recovery / (name + ".pending"), destination / name)
            replaced.append(name)
    except BaseException:
        for name in reversed(replaced):
            backup = recovery / (name + ".previous")
            if backup.exists():
                # Keep the backups available if rollback itself is interrupted.
                restored = recovery / (name + ".restore")
                shutil.copy2(backup, restored)
                os.replace(restored, destination / name)
            else:
                (destination / name).unlink(missing_ok=True)
        (recovery / "transaction.plan").unlink(missing_ok=True)
        shutil.rmtree(recovery)
        raise
    (recovery / "transaction.plan").unlink()
    shutil.rmtree(recovery)


with tempfile.TemporaryDirectory(prefix="drift-harbour-crane-") as scratch:
    stage = Path(scratch)
    bake(stage)
    crane()
    model.export(stage, "HarbourCrane")
    model = Mesh()
    footing()
    model.export(stage, "HarbourFooting")
    publish(stage, Path(sys.argv[sys.argv.index("--") + 1]).resolve())
