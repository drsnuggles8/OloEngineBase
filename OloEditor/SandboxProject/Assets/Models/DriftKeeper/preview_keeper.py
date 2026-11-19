"""Render fixed review views of the generated glTF in Blender 5.3 Workbench."""

import argparse
import json
from pathlib import Path
import sys
import bpy
from mathutils import Vector


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("models", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--study",
        action="store_true",
        help="Also render the clips, expressions, LOD masks and scanned reference",
    )
    parser.add_argument(
        "--anatomy", type=Path, help="Path to the optional rigged KeeperAnatomy.gltf proportion study"
    )
    parser.add_argument("--groom", action="store_true")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1 :])
    args.output.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)

    def load(name):
        bpy.ops.import_scene.gltf(filepath=str((args.models / name).resolve()), import_pack_images=True)
        return list(bpy.context.selected_objects)

    objs = load(args.anatomy.resolve() if args.anatomy else "Keeper.gltf")
    arm = next(o for o in objs if o.type == "ARMATURE")
    body = next(o for o in objs if o.type == "MESH" and o.data.shape_keys)
    for ob in objs:
        ob.animation_data_clear()
        if ob.type == "ARMATURE":
            ob.data.pose_position = "REST"
    foundation = (
        None if args.anatomy else next(ob for ob in load("KeeperHairFoundation.gltf") if ob.type == "MESH")
    )
    rig = json.loads((args.models / "Keeper.rig.json").read_text())
    attachments = [(foundation, "head")] if foundation else []
    for eye in rig["eyes"]:
        globe = next(o for o in load("KeeperEyeball.gltf") if o.type == "MESH")
        x, y, z = eye["centre"]
        globe.location = (x, -z, y)
        globe.scale = (rig["eyeRadius"],) * 3
        attachments.append((globe, eye["bone"]))
    scene = bpy.context.scene
    if args.groom and foundation:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from build_keeper_groom import grow

        for hair in grow(foundation):
            attachments.append((hair, "head"))
    bpy.ops.object.camera_add()
    camera = bpy.context.object
    scene.camera = camera
    camera.data.type = "ORTHO"
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 12
    scene.render.engine = "CYCLES" if args.groom else "BLENDER_WORKBENCH"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "MATERIAL"
    shading.show_shadows = True
    shading.show_cavity = True
    shading.cavity_type = "WORLD"
    # Workbench material display colors are separate from the glTF nodes.
    for mat in bpy.data.materials:
        if not mat.use_nodes:
            continue
        node = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if node:
            mat.diffuse_color = node.inputs["Base Color"].default_value
    world = bpy.data.worlds.new("Slate")
    world.color = (0.13, 0.14, 0.15)
    scene.world = world
    for loc, energy, size in [((-2, -3, 4), 370, 3), ((1, 2, 3), 220, 2)]:
        bpy.ops.object.light_add(type="AREA", location=loc)
        light = bpy.context.object
        light.data.energy = energy
        light.data.shape = "DISK"
        light.data.size = size
        light.rotation_euler = (Vector((0, 0, 1)) - light.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.resolution_x = 520
    scene.render.resolution_y = 680
    scene.render.resolution_percentage = 100

    def shot(name, delta, height, scale):
        target = Vector((0, 0, height))
        camera.location = target + Vector(delta)
        camera.rotation_euler = (-Vector(delta)).to_track_quat("-Z", "Y").to_euler()
        camera.data.ortho_scale = scale
        scene.render.filepath = str((args.output / (name + ".png")).resolve())
        bpy.ops.render.render(write_still=True)

    if args.anatomy:
        for name, delta in [
            ("front", (0, -1, 0.02)),
            ("quarter", (0.64, -1, 0.04)),
            ("profile", (1, -0.02, 0.02)),
            ("back", (0, 1, 0.04)),
        ]:
            shot(name, delta, 0.88, 1.94)
        if args.study:
            arm.data.pose_position = "POSE"
            anim = arm.animation_data_create()
            anim.action = bpy.data.actions["Wave"]
            anim.action_slot = anim.action.slots[0]
            scene.frame_set(37)
            bpy.context.view_layer.update()
            for ob, bone in attachments:
                posed = arm.pose.bones[bone]
                ob.matrix_world = posed.matrix @ posed.bone.matrix_local.inverted() @ ob.matrix_world
            shot("wave", (0.45, -2, 0.10), 0.90, 2.05)
        return
    for name, delta, height, scale in [
        ("body", (0.85, -2, 0.25), 0.90, 1.97),
        ("front", (0, -1, 0.018), 1.62, 0.34),
        ("quarter", (0.70, -1, 0.09), 1.62, 0.34),
        ("profile", (1, -0.012, 0.025), 1.62, 0.34),
        ("high", (0.52, -1, 0.35), 1.62, 0.34),
    ]:
        shot(name, delta, height, scale)
    if not args.study:
        return
    for shape in ("BlinkL", "JawOpen", "Smile", "OO"):
        key = body.data.shape_keys.key_blocks[shape]
        key.value = 1
        root_key = foundation.data.shape_keys.key_blocks.get(shape)
        if root_key:
            root_key.value = 1
        shot(shape, (0.18, -1, 0.045), 1.615, 0.29)
        key.value = 0
        if root_key:
            root_key.value = 0
    bpy.context.view_layer.update()
    rest = [(ob, bone, ob.matrix_world.copy()) for ob, bone in attachments]
    arm.data.pose_position = "POSE"
    anim = arm.animation_data_create()
    for clip, frame in [("Wave", 37), ("LanternRaise", 66)]:
        anim.action = bpy.data.actions[clip]
        anim.action_slot = anim.action.slots[0]
        scene.frame_set(frame)
        for ob, bone, matrix in rest:
            posed = arm.pose.bones[bone]
            ob.matrix_world = posed.matrix @ posed.bone.matrix_local.inverted() @ matrix
        shot(clip, (0.45, -2, 0.10), 1.02, 1.95)
    for ob, bone, matrix in rest:
        ob.matrix_world = matrix
    arm.data.pose_position = "REST"
    # An orthographic alpha mask at the same scale resolves silhouette changes
    # even when a distant character would cover only a few dozen pixels.
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.film_transparent = True
    for level, name in enumerate(("Keeper", "KeeperLOD1", "KeeperLOD2")):
        tier = (
            body
            if level == 0
            else next(o for o in load(name + ".gltf") if o.type == "MESH" and o.data.shape_keys)
        )
        for owner in bpy.context.scene.objects:
            if owner.type == "ARMATURE":
                owner.data.pose_position = "REST"
        shot("mask" + str(level), (0.43, -1, 0.08), 0.88, 1.95)
        tier.hide_render = True
    scene.render.film_transparent = False
    for ob in list(bpy.context.scene.objects):
        if ob.type not in ("CAMERA", "LIGHT"):
            ob.hide_render = True
    ref = Path(__file__).resolve().parents[4] / "assets/models/InfiniteScanHead/HeadRigged.gltf"
    load(ref)
    scene.render.engine = "CYCLES"
    shot("scan", (0.70, -1, 0.09), 0.295, 0.40)


if __name__ == "__main__":
    main()
