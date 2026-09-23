"""Turn the CC0 "Rigged Horse" .blend into the engine's reference horse (issue #1223).

Run with Blender 5.x, headless:

    blender -b riggedHorse.blend --python prepare_horse.py -- <out_dir> [<preview_dir>]

Source: https://opengameart.org/content/rigged-horse (CC0; model by Lyndon Daniels,
rig by ChadM). The .blend is not committed -- download it, run this, and commit
what it writes. See README.md in this directory for what is changed and why.

What this script does, in order:

1. Deletes the polygon mane and tail cards. They are the thing the groom
   replaces: a groom mane and tail on a card mane and tail is two manes.
2. Joins both eyes into the body, rigidly weighted to the head bone. They
   were unparented spheres, so the head left them behind in every pose.
3. Folds the "Bone.005" vertex group (a secondary neck weight with no bone
   behind it) into the neck bone, so the exported weights sum as authored.
4. Rebuilds the material as a Principled BSDF from the packed albedo and
   normal maps (the 2012 material is Blender-Internal-shaped and exports
   as a flat colour).
5. Scales to metres (a draft horse, ~2.2 m to the top of the head) and puts
   the hooves on the ground, the head toward -Y (glTF +Z).
6. Authors three in-place clips on the existing 19-bone rig -- Idle, Walk,
   Trot -- because the source has none. Leg swing directions are MEASURED
   from the rig rather than assumed, so a bone roll cannot silently reverse
   a gait.
7. Exports glTF (separate .bin + images).
"""

import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
OUT_DIR = os.path.abspath(argv[0]) if argv else os.path.dirname(bpy.data.filepath)
PREVIEW_DIR = os.path.abspath(argv[1]) if len(argv) > 1 else None
os.makedirs(OUT_DIR, exist_ok=True)

SCALE = 0.23  # source units -> metres
FPS = 30

scene = bpy.context.scene
scene.render.fps = FPS

# -- 1. strip everything that is not the horse ------------------------------
for name in [o.name for o in bpy.data.objects]:
    obj = bpy.data.objects[name]
    if name.startswith("BezierCurve") or obj.type in {"LIGHT", "CAMERA"}:
        bpy.data.objects.remove(obj, do_unlink=True)

arm = bpy.data.objects["Armature"]
body = bpy.data.objects["Plane"]
eyes = [bpy.data.objects["Sphere"], bpy.data.objects["Sphere.002"]]

# -- 3a. deform exactly as the engine will -----------------------------------
# Every chain carries a targetless IK constraint, which Blender evaluates and
# the glTF exporter would bake into the clips; and the armature modifier also
# deforms by bone ENVELOPE, which no runtime reads. Both go, so the preview
# renders below deform by vertex weights alone -- what the engine skins with.
for b in arm.pose.bones:
    for c in list(b.constraints):
        b.constraints.remove(c)
for m in body.modifiers:
    if m.type == "ARMATURE":
        m.use_bone_envelopes = False
        m.use_vertex_groups = True

# -- 3. the orphan weight group ---------------------------------------------
orphan = body.vertex_groups.get("Bone.005")
neck = body.vertex_groups["Bone.001"]
if orphan is not None:
    for v in body.data.vertices:
        for g in v.groups:
            if g.group == orphan.index and g.weight > 0.0:
                current = 0.0
                for h in v.groups:
                    if h.group == neck.index:
                        current = h.weight
                neck.add([v.index], current + g.weight, "REPLACE")
    body.vertex_groups.remove(orphan)

# -- 4. materials -------------------------------------------------------------
def packed_image(prefix):
    for img in bpy.data.images:
        if img.name.startswith(prefix) and img.packed_file is not None:
            return img
    raise RuntimeError(f"no packed image starting with {prefix}")


def save_image(img, path, fmt):
    img.filepath_raw = path
    img.file_format = fmt
    img.save()
    return bpy.data.images.load(path, check_existing=True)


albedo = save_image(packed_image("HorseMain4k00.png"), os.path.join(OUT_DIR, "HorseAlbedo.jpg"), "JPEG")
normal = save_image(packed_image("HorseMain4k00Norm00"), os.path.join(OUT_DIR, "HorseNormal.png"), "PNG")
normal.colorspace_settings.name = "Non-Color"

skin = bpy.data.materials.new("HorseSkin")
skin.use_nodes = True
nt = skin.node_tree
bsdf = nt.nodes["Principled BSDF"]
bsdf.inputs["Roughness"].default_value = 0.72
tex = nt.nodes.new("ShaderNodeTexImage")
tex.image = albedo
nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
ntex = nt.nodes.new("ShaderNodeTexImage")
ntex.image = normal
nmap = nt.nodes.new("ShaderNodeNormalMap")
nt.links.new(ntex.outputs["Color"], nmap.inputs["Color"])
nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])

eye = bpy.data.materials.new("HorseEye")
eye.use_nodes = True
eb = eye.node_tree.nodes["Principled BSDF"]
eb.inputs["Base Color"].default_value = (0.035, 0.022, 0.015, 1.0)
eb.inputs["Roughness"].default_value = 0.08

body.data.materials.clear()
body.data.materials.append(skin)

# -- 2. eyes into the body, rigid to the head --------------------------------
for e in eyes:
    e.data.materials.clear()
    e.data.materials.append(eye)
    e.vertex_groups.clear()
    g = e.vertex_groups.new(name="Bone.002")
    g.add([v.index for v in e.data.vertices], 1.0, "REPLACE")

with bpy.context.temp_override(
    active_object=body,
    object=body,
    selected_objects=[body, *eyes],
    selected_editable_objects=[body, *eyes],
):
    bpy.ops.object.join()
body.name = "Horse"
body.data.name = "Horse"

# -- 5. metres, feet on the ground -------------------------------------------
# BAKED into the vertices and the bones, with every object transform left at
# the identity -- and baked by hand, not with transform_apply.
#
# Both halves of that were learned the hard way. Left on the armature node as
# a scale, the transform is applied TWICE by the engine's importer (the mesh
# arrives in metres and every skinning matrix still carries the 0.23), so the
# coat bound to this body deformed into a tiny clump near the origin. And
# transform_apply on this rig left the bone pivots and the skinned mesh
# disagreeing, so the first bent knee tore the forearm into a metre-long band
# while the rest pose looked perfect. For a uniform scale plus a translation
# the hand bake below is exact.
# The source was saved mid-pose; bake nothing of it.
for b in arm.pose.bones:
    b.location = (0.0, 0.0, 0.0)
    b.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    b.rotation_euler = (0.0, 0.0, 0.0)
    b.scale = (1.0, 1.0, 1.0)
assert body.parent == arm, "expected the body to be parented to the armature"
bpy.context.view_layer.update()

# World-space rest positions, before anything moves.
arm_world = arm.matrix_world.copy()
body_world = body.matrix_world.copy()
world = [body_world @ v.co for v in body.data.vertices]
lo = Vector((min(p.x for p in world), min(p.y for p in world), min(p.z for p in world)))
hi = Vector((max(p.x for p in world), max(p.y for p in world), max(p.z for p in world)))
offset = Vector(((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, lo.z))


def to_metres(p):
    return (p - offset) * SCALE


# The mesh: vertices into metres, the object at the identity, still parented.
for v, w in zip(body.data.vertices, world):
    v.co = to_metres(w)
body.parent = None
body.matrix_world = Matrix.Identity(4)

# The bones: heads and tails into the same metres; rolls are unaffected by a
# uniform scale and a translation.
bpy.context.view_layer.objects.active = arm
with bpy.context.temp_override(active_object=arm, object=arm, edit_object=arm):
    bpy.ops.object.mode_set(mode="EDIT")
    # Snapshot EVERY joint before writing any: a connected child's head IS
    # its parent's tail, so writing the parent's tail moves the child's head,
    # and reading it back afterwards transforms that joint twice. That tore
    # the legs exactly the way a broken bake does.
    rest = {eb.name: (arm_world @ eb.head, arm_world @ eb.tail) for eb in arm.data.edit_bones}
    for eb in arm.data.edit_bones:
        head, tail = rest[eb.name]
        eb.head = to_metres(head)
        eb.tail = to_metres(tail)
    bpy.ops.object.mode_set(mode="OBJECT")
arm.matrix_world = Matrix.Identity(4)
body.parent = arm
body.matrix_parent_inverse = Matrix.Identity(4)
bpy.context.view_layer.update()
print("[prepare_horse] size (m)", [round(x, 3) for x in ((hi - lo) * SCALE)])

# -- 6. the clips ---------------------------------------------------------------
pb = arm.pose.bones
for b in pb:
    b.rotation_mode = "XYZ"


def tail_after(bone, axis, degrees):
    """World-space tail of `bone` after rotating it by `degrees` about a local axis."""
    saved = tuple(bone.rotation_euler)
    euler = list(saved)
    euler[axis] = math.radians(degrees)
    bone.rotation_euler = euler
    bpy.context.view_layer.update()
    tail = arm.matrix_world @ bone.tail
    bone.rotation_euler = saved
    bpy.context.view_layer.update()
    return tail


def sign_toward(bone, axis, want):
    """+1 if a positive rotation about `axis` moves the tail along `want`, else -1."""
    plus = tail_after(bone, axis, 10.0)
    minus = tail_after(bone, axis, -10.0)
    return 1.0 if (plus - minus).dot(want) > 0.0 else -1.0


FORWARD = Vector((0.0, -1.0, 0.0))  # the head points down -Y
BACK = -FORWARD
LEFT = Vector((-1.0, 0.0, 0.0))  # the "_L" chains sit at -X
UP = Vector((0.0, 0.0, 1.0))

LEGS = {
    # name: (upper, middle, lower, is_front)
    "LF": ("Bone_L", "Bone_L.001", "Bone_L.002", True),
    "RF": ("Bone_R", "Bone_R.001", "Bone_R.002", True),
    "LH": ("Bone_L.003", "Bone_L.004", "Bone_L.005", False),
    "RH": ("Bone_R.003", "Bone_R.004", "Bone_R.005", False),
}
SWING_SIGN = {k: sign_toward(pb[v[0]], 0, FORWARD) for k, v in LEGS.items()}
FLEX_SIGN = {k: sign_toward(pb[v[2]], 0, BACK) for k, v in LEGS.items()}
MID_SIGN = {k: sign_toward(pb[v[1]], 0, FORWARD if v[3] else BACK) for k, v in LEGS.items()}
NECK_SIGN = sign_toward(pb["Bone.001"], 0, UP)
TAIL_SIDE_SIGN = sign_toward(pb["Bone.003"], 2, LEFT)
TAIL_LIFT_SIGN = sign_toward(pb["Bone.003"], 0, UP)
print("[prepare_horse] swing", SWING_SIGN, "flex", FLEX_SIGN, "mid", MID_SIGN)


def smooth_bump(x):
    """0 -> 1 -> 0 over x in [0,1]."""
    return math.sin(math.pi * min(max(x, 0.0), 1.0))


def leg_pose(phase, duty, swing_deg, flex_deg, mid_deg):
    """(upper swing, middle, lower flex) in degrees for a leg at `phase`.

    Stance (phase < duty) carries the hoof from front to back at constant
    speed, as a planted hoof must; swing returns it with the lower joint
    folded, which is what lifts the hoof clear of the ground.
    """
    if phase < duty:
        t = phase / duty
        upper = swing_deg * (1.0 - 2.0 * t)
        return upper, 0.0, 0.0
    t = (phase - duty) / (1.0 - duty)
    ease = 0.5 - 0.5 * math.cos(math.pi * t)
    upper = -swing_deg + 2.0 * swing_deg * ease
    bump = smooth_bump(t)
    return upper, mid_deg * bump, flex_deg * bump


def key_gait(action_name, seconds, duty, offsets, swing, flex, mid, nod, tail_side, tail_lift):
    action = bpy.data.actions.new(action_name)
    arm.animation_data_create()
    arm.animation_data.action = action
    frames = int(round(seconds * FPS))
    for f in range(0, frames + 1):
        cycle = f / frames
        for leg, (upper, middle, lower, front) in LEGS.items():
            phase = (cycle + offsets[leg]) % 1.0
            u, m, lw = leg_pose(phase, duty, swing[front], flex[front], mid[front])
            pb[upper].rotation_euler = (math.radians(u) * SWING_SIGN[leg], 0.0, 0.0)
            pb[middle].rotation_euler = (math.radians(m) * MID_SIGN[leg], 0.0, 0.0)
            pb[lower].rotation_euler = (math.radians(lw) * FLEX_SIGN[leg], 0.0, 0.0)
            for name in (upper, middle, lower):
                pb[name].keyframe_insert("rotation_euler", frame=f + 1)
        # The head nods twice per stride at a walk; the tail swings once.
        pb["Bone.001"].rotation_euler = (math.radians(nod * math.sin(4.0 * math.pi * cycle)) * NECK_SIGN, 0.0, 0.0)
        pb["Bone.001"].keyframe_insert("rotation_euler", frame=f + 1)
        side = math.radians(tail_side * math.sin(2.0 * math.pi * cycle))
        lift = math.radians(tail_lift)
        pb["Bone.003"].rotation_euler = (lift * TAIL_LIFT_SIGN, 0.0, side * TAIL_SIDE_SIGN)
        pb["Bone.004"].rotation_euler = (0.0, 0.0, 0.6 * side * TAIL_SIDE_SIGN)
        pb["Bone.003"].keyframe_insert("rotation_euler", frame=f + 1)
        pb["Bone.004"].keyframe_insert("rotation_euler", frame=f + 1)
    track = arm.animation_data.nla_tracks.new()
    track.name = action_name
    track.strips.new(action_name, 1, action)
    arm.animation_data.action = None
    for b in pb:
        b.rotation_euler = (0.0, 0.0, 0.0)
    return action


def key_idle(seconds):
    action = bpy.data.actions.new("Idle")
    arm.animation_data_create()
    arm.animation_data.action = action
    frames = int(round(seconds * FPS))
    for f in range(0, frames + 1):
        cycle = f / frames
        pb["Bone.001"].rotation_euler = (math.radians(3.0 * math.sin(2.0 * math.pi * cycle)) * NECK_SIGN, 0.0, 0.0)
        pb["Bone.002"].rotation_euler = (0.0, 0.0, math.radians(6.0 * math.sin(2.0 * math.pi * cycle + 1.0)))
        swish = math.radians(22.0 * math.sin(4.0 * math.pi * cycle) * smooth_bump(cycle))
        pb["Bone.003"].rotation_euler = (0.0, 0.0, swish * TAIL_SIDE_SIGN)
        pb["Bone.004"].rotation_euler = (0.0, 0.0, 0.8 * swish * TAIL_SIDE_SIGN)
        for name in ("Bone.001", "Bone.002", "Bone.003", "Bone.004"):
            pb[name].keyframe_insert("rotation_euler", frame=f + 1)
    track = arm.animation_data.nla_tracks.new()
    track.name = "Idle"
    track.strips.new("Idle", 1, action)
    arm.animation_data.action = None
    for b in pb:
        b.rotation_euler = (0.0, 0.0, 0.0)
    return action


FRONT, HIND = True, False
# Four-beat lateral walk: LH, LF, RH, RF a quarter-stride apart.
key_gait(
    "Walk",
    seconds=1.2,
    duty=0.62,
    offsets={"LH": 0.0, "LF": 0.75, "RH": 0.5, "RF": 0.25},
    swing={FRONT: 17.0, HIND: 15.0},
    flex={FRONT: 55.0, HIND: 40.0},
    mid={FRONT: 12.0, HIND: 18.0},
    nod=4.0,
    tail_side=7.0,
    tail_lift=4.0,
)
# Two-beat trot: diagonal pairs together, a shorter stance.
key_gait(
    "Trot",
    seconds=0.72,
    duty=0.45,
    offsets={"LH": 0.0, "RF": 0.0, "RH": 0.5, "LF": 0.5},
    swing={FRONT: 22.0, HIND: 19.0},
    flex={FRONT: 75.0, HIND: 55.0},
    mid={FRONT: 18.0, HIND: 24.0},
    nod=2.0,
    tail_side=4.0,
    tail_lift=12.0,
)
key_idle(4.0)

# -- previews -------------------------------------------------------------------
if PREVIEW_DIR:
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = 800
    scene.render.resolution_y = 520
    cam_data = bpy.data.cameras.new("PreviewCam")
    cam = bpy.data.objects.new("PreviewCam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.location = Vector((7.5, -0.2, 1.3))
    cam.rotation_euler = (Vector((0.0, -0.9, 1.0)) - cam.location).to_track_quat("-Z", "Y").to_euler()
    for clip in ("Walk", "Trot"):
        arm.animation_data.action = bpy.data.actions[clip]
        for track in arm.animation_data.nla_tracks:
            track.mute = True
        frames = int(bpy.data.actions[clip].frame_range[1])
        for i, f in enumerate((1, frames // 4, frames // 2, (3 * frames) // 4)):
            scene.frame_set(f)
            scene.render.filepath = os.path.join(PREVIEW_DIR, f"horse_{clip}_{i}.png")
            bpy.ops.render.render(write_still=True)
    arm.animation_data.action = None
    for track in arm.animation_data.nla_tracks:
        track.mute = False
    bpy.data.objects.remove(cam, do_unlink=True)

# -- 7. export ------------------------------------------------------------------
for obj in bpy.data.objects:
    obj.select_set(obj in (arm, body))
bpy.ops.export_scene.gltf(
    filepath=os.path.join(OUT_DIR, "Horse.gltf"),
    export_format="GLTF_SEPARATE",
    use_selection=True,
    export_yup=True,
    export_animations=True,
    export_animation_mode="NLA_TRACKS",
    export_skins=True,
    export_image_format="AUTO",
    export_apply=False,
)
print("[prepare_horse] wrote", os.path.join(OUT_DIR, "Horse.gltf"))
