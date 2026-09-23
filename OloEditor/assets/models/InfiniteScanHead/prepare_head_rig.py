"""Give the Infinite Scan head a neck and a head bone, and a clip to move them (issue #1223).

Run with Blender 5.x, headless:

    blender -b --python prepare_head_rig.py -- <this_dir> [<preview_dir>]

The groom epic needs a MOVING human hair subject, and the scan is a static bust.
The hair has to be carried by the same skinning -> binding -> simulation path
the animals use, so a rigid transform on the entity would not do: it would
test the entity transform, not the binding. This adds the smallest rig that
can turn a head:

    Chest (shoulders, never moves) -> Neck -> Head

with smooth weights across the neck, and one looping clip, "LookAround" (6 s):
a turn to each side, a nod, and a tilt. Loose hair swings on a turn and settles
on a hold, so the clip contains both.

Writes HeadRigged.gltf + HeadRigged.bin. The albedo stays the existing
Textures/lambertian.jpg (the glTF references it; it is not re-encoded).
Licence: CC BY 3.0, the same as the scan -- see LICENSE.md and README.md.
"""

import json
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
HERE = os.path.abspath(argv[0]) if argv else os.path.dirname(os.path.abspath(__file__))
PREVIEW_DIR = os.path.abspath(argv[1]) if len(argv) > 1 else None

BUST_HEIGHT = 0.43  # metres, shoulders to crown; puts the head at ~0.2 m wide
FPS = 30

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = FPS
bpy.ops.import_scene.fbx(filepath=os.path.join(HERE, "Head.fbx"))
body = bpy.data.objects["head"]

# -- metres, the bust's base on the origin -------------------------------------
# Baked into the vertices here (a mesh with no parent and no armature yet, so
# this is a plain data transform, not transform_apply on a rig).
bpy.context.view_layer.update()
world = [body.matrix_world @ v.co for v in body.data.vertices]
lo = Vector((min(p.x for p in world), min(p.y for p in world), min(p.z for p in world)))
hi = Vector((max(p.x for p in world), max(p.y for p in world), max(p.z for p in world)))
k = BUST_HEIGHT / (hi.z - lo.z)
centre = Vector(((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, lo.z))
mw = body.matrix_world.copy()
for v in body.data.vertices:
    v.co = ((mw @ v.co) - centre) * k
body.matrix_world = body.matrix_world.Identity(4)
bpy.context.view_layer.update()
H = BUST_HEIGHT
print("[prepare_head_rig] bust", [round(x, 3) for x in ((hi - lo) * k)])

# -- the rig ---------------------------------------------------------------------
arm_data = bpy.data.armatures.new("HeadRig")
arm = bpy.data.objects.new("HeadRig", arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
with bpy.context.temp_override(active_object=arm, object=arm, edit_object=arm):
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    chest = eb.new("Chest")
    chest.head = Vector((0.0, 0.0, 0.0))
    chest.tail = Vector((0.0, 0.0, 0.30 * H))
    neck = eb.new("Neck")
    neck.head = chest.tail.copy()
    neck.tail = Vector((0.0, 0.01 * H, 0.52 * H))
    neck.parent = chest
    neck.use_connect = True
    head = eb.new("Head")
    head.head = neck.tail.copy()
    head.tail = Vector((0.0, 0.0, 0.95 * H))
    head.parent = neck
    head.use_connect = True
    bpy.ops.object.mode_set(mode="OBJECT")


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


groups = {name: body.vertex_groups.new(name=name) for name in ("Chest", "Neck", "Head")}
for v in body.data.vertices:
    z = v.co.z / H
    to_neck = smoothstep(0.26, 0.36, z)  # shoulders -> neck
    to_head = smoothstep(0.46, 0.58, z)  # neck -> skull
    # The jaw hangs BELOW that band at the front: height alone leaves the
    # chin on the neck, and a turn shears the mouth sideways. Anything in
    # front of the neck column (the face is toward -Y) above the collar
    # belongs to the skull.
    y = v.co.y / H
    jaw = smoothstep(-0.13, -0.19, y) * smoothstep(0.34, 0.40, z)
    to_head = max(to_head, jaw)
    w_head = to_head
    w_neck = to_neck * (1.0 - to_head)
    w_chest = 1.0 - to_neck
    for name, w in (("Chest", w_chest), ("Neck", w_neck), ("Head", w_head)):
        if w > 1.0e-4:
            groups[name].add([v.index], w, "REPLACE")

body.parent = arm
mod = body.modifiers.new("Armature", "ARMATURE")
mod.object = arm
mod.use_vertex_groups = True
mod.use_bone_envelopes = False

# -- the clip -----------------------------------------------------------------------
pb = arm.pose.bones
for b in pb:
    b.rotation_mode = "XYZ"

SECONDS = 6.0
frames = int(SECONDS * FPS)
action = bpy.data.actions.new("LookAround")
arm.animation_data_create()
arm.animation_data.action = action


def ease(x):
    return 0.5 - 0.5 * math.cos(math.pi * min(max(x, 0.0), 1.0))


# Key poses as (time fraction, yaw deg, nod deg, tilt deg); holds between
# turns, so the hair both swings and settles within one loop.
KEYS = [
    (0.00, 0.0, 0.0, 0.0),
    (0.12, 38.0, -4.0, 5.0),
    (0.30, 38.0, -4.0, 5.0),
    (0.45, -40.0, 3.0, -6.0),
    (0.62, -40.0, 3.0, -6.0),
    (0.74, 0.0, 14.0, 0.0),
    (0.86, 0.0, -8.0, 0.0),
    (1.00, 0.0, 0.0, 0.0),
]


def pose_at(t):
    for (t0, *a), (t1, *b) in zip(KEYS, KEYS[1:]):
        if t0 <= t <= t1:
            u = ease((t - t0) / (t1 - t0))
            return [x + (y - x) * u for x, y in zip(a, b)]
    return KEYS[-1][1:]


for f in range(frames + 1):
    yaw, nod, tilt = pose_at(f / frames)
    # Bone Y runs up the neck, so yaw is about local Y, nod about X, tilt about Z.
    # The neck carries 40% of the motion and the head the rest.
    for name, share in (("Neck", 0.4), ("Head", 0.6)):
        pb[name].rotation_euler = (math.radians(nod * share), math.radians(yaw * share), math.radians(tilt * share))
        pb[name].keyframe_insert("rotation_euler", frame=f + 1)
track = arm.animation_data.nla_tracks.new()
track.name = "LookAround"
track.strips.new("LookAround", 1, action)
arm.animation_data.action = None
for b in pb:
    b.rotation_euler = (0.0, 0.0, 0.0)

# -- material: the scan's own albedo, by reference ------------------------------
mat = bpy.data.materials.new("HeadSkin")
mat.use_nodes = True
bsdf = mat.node_tree.nodes["Principled BSDF"]
bsdf.inputs["Roughness"].default_value = 0.5
tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
tex.image = bpy.data.images.load(os.path.join(HERE, "Textures", "lambertian.jpg"), check_existing=True)
mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
body.data.materials.clear()
body.data.materials.append(mat)

# -- previews ---------------------------------------------------------------------
if PREVIEW_DIR:
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = 500
    scene.render.resolution_y = 500
    cam = bpy.data.objects.new("PreviewCam", bpy.data.cameras.new("PreviewCam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.location = Vector((0.35, -1.1, 0.33))
    cam.rotation_euler = (Vector((0.0, 0.0, 0.28)) - cam.location).to_track_quat("-Z", "Y").to_euler()
    arm.animation_data.action = action
    track.mute = True
    for i, t in enumerate((0.0, 0.2, 0.52, 0.74, 0.86)):
        scene.frame_set(int(t * frames) + 1)
        scene.render.filepath = os.path.join(PREVIEW_DIR, f"head_{i}.png")
        bpy.ops.render.render(write_still=True)
    arm.animation_data.action = None
    track.mute = False
    bpy.data.objects.remove(cam, do_unlink=True)

# -- export -------------------------------------------------------------------------
for obj in bpy.data.objects:
    obj.select_set(obj in (arm, body))
out = os.path.join(HERE, "HeadRigged.gltf")
bpy.ops.export_scene.gltf(
    filepath=out,
    export_format="GLTF_SEPARATE",
    use_selection=True,
    export_yup=True,
    export_animations=True,
    export_animation_mode="NLA_TRACKS",
    export_skins=True,
    export_image_format="NONE",
)

# The exporter was told not to write images; point the material back at the
# scan's existing albedo rather than shipping a second 8.6 MB copy of it.
with open(out, encoding="utf-8") as f:
    gltf = json.load(f)
gltf["images"] = [{"uri": "Textures/lambertian.jpg", "mimeType": "image/jpeg"}]
gltf["samplers"] = gltf.get("samplers") or [{}]
gltf["textures"] = [{"source": 0, "sampler": 0}]
for m in gltf.get("materials", []):
    pbr = m.setdefault("pbrMetallicRoughness", {})
    pbr["baseColorTexture"] = {"index": 0}
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(gltf, f, indent=1)
print("[prepare_head_rig] wrote", out)
