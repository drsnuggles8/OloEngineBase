"""Generate the Stacks light. Run in factory-startup Blender, in background mode."""

import math
from pathlib import Path
import sys

import bpy
import numpy as np
from mathutils import Vector


ROOT = Path(
    sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else Path(__file__).parent
).resolve()
ROOT.mkdir(parents=True, exist_ok=True)
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


def masonry():
    """One full wrap. Mortar courses, salt bloom and drips are located in metres."""
    size = 2048
    rng = np.random.default_rng(5501)
    z, u = np.indices((size, size), dtype=np.float32)
    u /= size
    z = -14 + (z + 0.5) * (32 / size)
    row = np.floor(z / 0.43).astype(int)
    blocks = u * 20 + 0.5 * (row % 2)
    block = np.floor(blocks).astype(int) % 20
    stones = rng.uniform(-1, 1, (96, 20))[row % 96, block]
    grit = rng.normal(0, 1, z.shape).astype("f")
    wave = np.sin(TAU * u * 4 + 0.43 * z) + 0.5 * np.sin(TAU * u * 11 - z * 0.8)
    seam = np.minimum(
        np.minimum(blocks % 1, 1 - blocks % 1) * 0.83,
        np.minimum(z % 0.43, 0.43 - z % 0.43),
    )
    shoulder = np.clip((seam + 0.003 * wave + 0.0015 * grit) / 0.022, 0, 1)
    lower = z < 1.15
    # A forked red daymark echoes the split sea stacks. Its low points face
    # opposite sailing approaches. A straight cap band survives foreshortening.
    chevron = 2.65 * abs(2 * ((u * 2 + 0.12) % 1) - 1)
    paint = ((z > 8.7 + chevron) & (z < 11.45 + chevron)) | ((z > 15.1) & (z < 15.68))
    base = np.where(
        lower[..., None],
        [0.33, 0.34, 0.33],
        np.where(paint[..., None], [0.64, 0.208, 0.117], [0.84, 0.818, 0.711]),
    )
    value = (
        stones * np.where(lower, 0.105, 0.025)
        + 0.019 * wave
        + 0.012 * grit
        - (1 - shoulder) * np.where(lower, 0.105, 0.038)
    )
    # Water shed by the gallery and ironwork leaves tapering tracks down the limewash.
    for center, length, width in rng.uniform([0, 0.8, 0.001], [1, 5, 0.009], (38, 3)):
        drop = 17.45 - z
        width_at_z = width * np.clip(1 - drop / length, 0.04, 1)
        distance = abs((u - center + 0.001 * np.sin(z) + 0.5) % 1 - 0.5)
        value -= (
            0.072
            * np.exp(-((distance / width_at_z) ** 2))
            * np.clip(1 - drop / length, 0, 1)
            * (drop >= 0)
        )
    value += lower * 0.07 * np.exp(-(((z + 0.5 + 0.35 * wave) / 0.8) ** 2))
    height = shoulder * np.where(lower, 0.013, 0.005) + grit * 0.0005 + 0.0015 * wave
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * size / (2 * 17)
    dy = np.gradient(height, axis=0) * size / 32
    normal = np.stack([-dx, -dy, np.ones_like(z)], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    mat = material("Salt-washed ashlar", (1, 1, 1), 0.84)
    tree = mat.node_tree
    for name, pixels, is_normal in [
        ("StacksMasonry", np.clip(base + value[..., None], 0, 1), False),
        ("StacksMasonryNormal", normal * 0.5 + 0.5, True),
    ]:
        # Write display-encoded RGB bytes directly. The importer assigns the base
        # colour sRGB and the tangent-space normal linear.
        img = bpy.data.images.new(name, size, size, alpha=False)
        img.colorspace_settings.name = "Non-Color"
        rgba = np.ones((size, size, 4), dtype="f")
        rgba[:, :, :3] = pixels
        img.pixels.foreach_set(rgba.ravel())
        img.filepath_raw = str(ROOT / (name + ".png"))
        img.file_format = "PNG"
        img.save()
        if not is_normal:
            img.colorspace_settings.name = "sRGB"
        tex = tree.nodes.new("ShaderNodeTexImage")
        tex.image = img
        target = tree.nodes["Principled BSDF"].inputs["Base Color"]
        if is_normal:
            bump = tree.nodes.new("ShaderNodeNormalMap")
            tree.links.new(
                bump.outputs["Normal"], tree.nodes["Principled BSDF"].inputs["Normal"]
            )
            target = bump.inputs["Color"]
        tree.links.new(tex.outputs["Color"], target)
    return mat


stone = masonry()
trim = material("Dressed buff granite", (0.47, 0.414, 0.32), 0.79)
iron = material("Graphite marine enamel", (0.045, 0.064, 0.065), 0.41, 0.37)
copper = material("Weathered copper", (0.105, 0.255, 0.201), 0.57, 0.51)
brass = material("Oiled bronze", (0.42, 0.236, 0.075), 0.33, 0.75)
door = material("Tarred oak", (0.075, 0.044, 0.023), 0.72)
pane = material("Bottle glass in shadow", (0.037, 0.099, 0.113), 0.16, 0.25)
glazing = material("Lantern crown glass", (0.94, 0.984, 0.97), 0.055)
glazing.node_tree.nodes["Principled BSDF"].inputs[
    "Transmission Weight"
].default_value = 1


def mesh(name, verts, faces, mat, uvs=None, smooth=False):
    data = bpy.data.meshes.new(name)
    data.from_pydata(verts, [], faces)
    data.materials.append(mat)
    if uvs:
        layer = data.uv_layers.new(name="Masonry wrap")
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


def revolve(name, rings, mat, sides=72, smooth=False, openings=()):
    verts = [polar(r, TAU * i / sides, z) for z, r in rings for i in range(sides)]
    faces, uvs = [], []
    for row, ((bottom, _), (top, _)) in enumerate(zip(rings, rings[1:])):
        for i in range(sides):
            if any(
                bottom >= sill
                and top <= head
                and abs(((i + 0.5) * 360 / sides - angle + 180) % 360 - 180) < half
                for angle, half, sill, head in openings
            ):
                continue
            j = (i + 1) % sides
            start = row * sides
            faces.append([start + i, start + j, start + sides + j, start + sides + i])
            uvs.append(
                [
                    (u, (z + 14) / 32)
                    for u, z in [
                        (i / sides, bottom),
                        ((i + 1) / sides, bottom),
                        ((i + 1) / sides, top),
                        (i / sides, top),
                    ]
                ]
            )
    for row, indices in [(0, range(sides - 1, -1, -1)), (len(rings) - 1, range(sides))]:
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
    return mesh(name, verts, faces, mat, uvs if mat == stone else None, smooth)


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
        edge.width = bevel
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
        wear = math.exp(-((lateral - 0.04 * math.sin(index)) / 0.43) ** 2)
        profile = [
            (-0.20, -0.24), (0.20, -0.24),
            (0.20, -0.035 - 0.022 * wear), (0.191 - 0.012 * wear, -0.012 - 0.021 * wear),
            (0.170 - 0.018 * wear, -0.018 * wear), (-0.11, -0.003 * wear), (-0.20, 0.0),
        ]
        verts.extend(frame(angle, radius + depth, lateral, top + height) for depth, height in profile)
    n = len(profile)
    faces = [tuple(range(n)), tuple(range(len(verts) - 1, len(verts) - n - 1, -1))]
    for start in range(0, len(verts) - n, n):
        for i in range(n):
            j = (i + 1) % n
            faces.append((start + i, start + n + i, start + n + j, start + j))
    mesh("Foot-worn granite tread", verts, faces, trim)


def stair_carriage(angle):
    # The return arch carries the stair between the pier and its outboard toe.
    outline = [(2.69, -9), (3.28, -9), (3.28, -2.98)]
    for t in np.linspace(math.pi, 0, 13)[1:]:
        outline.append((3.94 + 0.66 * math.cos(t), -2.98 + 0.66 * math.sin(t)))
    outline += [(4.60, -9), (5.05, -9), (5.05, -1.34), (2.69, 0.01)]
    verts = [frame(angle, r, s, z) for s in (-0.73, 0.73) for r, z in outline]
    n = len(outline)
    faces = [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
    side_uv = [(r / 17, (z + 10) / 32) for r, z in outline]
    uvs = [side_uv, side_uv[::-1]]
    for i in range(n):
        j = (i + 1) % n
        faces.append([i, n + i, n + j, j])
        uvs.append(
            [
                (s / 17, (outline[k][1] + 10) / 32)
                for k, s in ((i, 0), (i, 1.46), (j, 1.46), (j, 0))
            ]
        )
    mesh("Masonry stair arch", verts, faces, stone, uvs)


def tower():
    cuts = [
        (60, 15, 0.12, 2.40),
        (-60, 10, 4.7, 5.95),
        (140, 10, 8.55, 9.8),
        (-40, 10, 12.4, 13.65),
        (60, 10, 15.75, 17.10),
    ]
    heights = sorted({0, 17.48, *(h for _, _, s, t in cuts for h in (s, t))})
    revolve(
        "Battered masonry pier",
        [(-14, 3.28), (-1.1, 3.12), (-0.2, 3.12), (0.05, 3.05)],
        stone,
    )
    revolve(
        "Limewashed granite tower",
        [(z, shaft_radius(z)) for z in heights],
        stone,
        72,
        True,
        cuts,
    )
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
            block(
                "Stone reveal",
                a,
                depth - 0.012,
                side * width / 2,
                mid,
                0.43,
                0.16,
                head - sill,
                trim,
            )
        for z in (sill, head):
            block("Drip sill or lintel", a, depth, 0, z, 0.50, width + 0.41, 0.17, trim)
        if entry:
            for i in range(1, 7):
                block(
                    "Board joint",
                    a,
                    depth - 0.10,
                    width * (i / 7 - 0.5),
                    mid,
                    0.018,
                    0.014,
                    head - sill - 0.18,
                    iron,
                    0,
                )
            for z in (0.66, 1.82):
                block(
                    "Strap hinge",
                    a,
                    depth - 0.084,
                    -width * 0.19,
                    z,
                    0.045,
                    width * 0.48,
                    0.067,
                    iron,
                )
            block(
                "Latch plate",
                a,
                depth - 0.07,
                width * 0.30,
                1.05,
                0.055,
                0.085,
                0.23,
                brass,
            )
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


def lantern():
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
        mesh(
            "Crown glass",
            [
                polar(1.75, a, 19.19),
                polar(1.75, b, 19.19),
                polar(1.75, b, 21.30),
                polar(1.75, a, 21.30),
            ],
            [(0, 1, 2, 3)],
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
        revolve("Lens retaining ring", [(z - 0.022, r), (z + 0.022, r)], brass, 48)
    for i in range(6):
        a = TAU * i / 6
        bar("Optic tie rod", polar(0.52, a, 19.61), polar(0.52, a, 20.94), 0.023, brass)
    hood = [
        (21.28, 1.84),
        (21.36, 2.07),
        (21.49, 2.14),
        (21.58, 2.10),
        (21.66, 1.88),
        (22.14, 1.50),
        (22.52, 0.91),
        (22.74, 0.33),
        (22.89, 0.22),
    ]
    revolve("Standing seam copper hood", hood, copper, 48, True)
    for i in range(12):
        a = i * TAU / 12
        for (z, r), (zz, rr) in zip(hood[4:], hood[5:]):
            bar(
                "Batten seam",
                polar(r + 0.012, a, z),
                polar(rr + 0.012, a, zz),
                0.025,
                copper,
                8,
            )
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
lantern()
# One node at identity. The exporter groups disconnected parts by material.
bpy.ops.object.select_all(action="SELECT")
bpy.context.view_layer.objects.active = bpy.data.objects["Limewashed granite tower"]
bpy.ops.object.join()
bpy.context.object.name = "Stacks Light"
# The untextured fittings inherit Blender's primitive UVMap. Bevel interpolation
# varies by a float ULP with worker scheduling. Only the masonry chart is used.
layers = bpy.context.object.data.uv_layers
for layer in list(layers):
    if layer.name != "Masonry wrap":
        layers.remove(layer)
triangulate = bpy.context.object.modifiers.new("Export triangulation", "TRIANGULATE")
bpy.ops.object.modifier_apply(modifier=triangulate.name)
bpy.ops.export_scene.gltf(
    filepath=str(ROOT / "StacksLight.gltf"),
    export_format="GLTF_SEPARATE",
    export_yup=True,
    export_cameras=False,
    export_lights=False,
    export_animations=False,
    export_tangents=True,
)
