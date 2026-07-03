#!/usr/bin/env python3
# =============================================================================
# generate_perf_scenes.py
#
# Emits parameterized performance stress scenes (.olo YAML) into
# OloEditor/SandboxProject/Assets/Scenes/PerfStress/ (git-ignored output — only
# this generator is checked in; regenerate locally on demand).
#
# Each scene isolates ONE axis of frame cost that normally shows up in games:
#
#   physics_pile       N dynamic Rigidbody3D bodies falling into a walled arena
#                      (Jolt step + Jolt->ECS transform writeback)
#   physics_sleeping   N bodies resting on the floor -> asleep after settling
#                      (measures the sleeping-body writeback, Scene.cpp)
#   physics_stacks     towers of 10 boxes (contact/solver-heavy)
#   ecs_static         N transform-only entities (spatial-index rebuild floor,
#                      SceneHierarchyPanel scalability)
#   draws_unique       N small cubes, each a unique material (defeats batching ->
#                      CPU submission + Ref<T> churn)
#   draws_instanced    N instances in one InstancedMeshComponent (GPU
#                      instancing path; GPU-cull threshold >=1024)
#   lights_many        N point lights over a field of receivers (Forward+)
#   lights_shadowed    same, all lights cast shadows
#   anim_crowd         N skinned Fox characters, playing (per-bone string-map
#                      lookups + pose copies in AnimationSystem)
#   scripts_swarm      N entities with a trivial per-frame Lua script
#                      (interop crossing cost)
#   scripts_swarm_cs   C# variant (Mono interop crossing cost)
#   overdraw_sheets    N stacked full-screen alpha-blended quads (fill rate)
#   kitchen_sink       a combined "game level" with moderate counts of all
#
# Usage:
#   python OloEngine/tests/scripts/generate_perf_scenes.py --scene physics_pile --count 10000
#   python OloEngine/tests/scripts/generate_perf_scenes.py --scene all
#   python OloEngine/tests/scripts/generate_perf_scenes.py --list
#
# The YAML shape follows SceneSerializer.cpp's contract (validated by loading
# every generated scene through the editor): unique u64 entity ids, component
# maps exactly as the (de)serializer reads them. Per-entity YAML is kept
# minimal so a 50k-entity file stays manageable.
#
# A manifest (PerfStress/manifest.json) records every generated scene with the
# metadata the measurement driver (scripts/perf/run-perf-battery.ps1) consumes:
# file, entity count, whether the scene needs Play mode to exercise its axis,
# and generation stats (a 50k YAML load being slow is itself a finding).
# =============================================================================

import argparse
import json
import math
import pathlib
import random
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_OUT_DIR = REPO_ROOT / "OloEditor" / "SandboxProject" / "Assets" / "Scenes" / "PerfStress"

# Entity ids only need to be unique u64s within one scene; a fixed readable
# base keeps diffs stable across regenerations.
ID_BASE = 910_000_000_000_001

PALETTE = [
    (0.85, 0.30, 0.25), (0.25, 0.75, 0.35), (0.25, 0.40, 0.85), (0.90, 0.80, 0.25),
    (0.75, 0.30, 0.80), (0.30, 0.80, 0.80), (0.90, 0.55, 0.25), (0.60, 0.60, 0.60),
]


def f(x):
    """Compact float formatting: 4 significant decimals, no trailing zeros."""
    s = f"{x:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def vec(v):
    return "[" + ", ".join(f(c) for c in v) + "]"


def hue_color(t):
    """Cheap HSV(h,0.65,0.9) -> RGB for visually distinct materials."""
    h = (t % 1.0) * 6.0
    i = int(h) % 6
    frac = h - int(h)
    p, q, r = 0.315, 0.9 * (1 - 0.65 * frac), 0.9 * (1 - 0.65 * (1 - frac))
    return [(0.9, r, p), (q, 0.9, p), (p, 0.9, r), (p, q, 0.9), (r, p, 0.9), (0.9, p, q)][i]


class SceneWriter:
    """Streams entity blocks; avoids holding a 50MB string tree in memory."""

    def __init__(self, scene_name, watch_tags=()):
        self.parts = [f"Scene: {scene_name}\nEntities:\n"]
        self.next_id = ID_BASE
        self.count = 0
        # Camera pose, recorded for the manifest: edit-mode scenes are viewed
        # through the EDITOR camera (the in-scene camera only applies in Play
        # mode), so the measurement driver re-poses it via olo_camera_set_pose.
        self.camera_pos = None
        self.camera_rot = None
        # First entity id per watched tag (the driver's workload probe).
        self.watch_tags = set(watch_tags)
        self.tag_ids = {}

    def entity(self, tag, translation=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), components=""):
        eid = self.next_id
        self.next_id += 1
        self.count += 1
        if tag in self.watch_tags and tag not in self.tag_ids:
            self.tag_ids[tag] = eid
        self.parts.append(
            f"  - Entity: {eid}\n"
            f"    TagComponent:\n"
            f"      Tag: {tag}\n"
            f"    TransformComponent:\n"
            f"      Translation: {vec(translation)}\n"
            f"      Rotation: {vec(rotation)}\n"
            f"      Scale: {vec(scale)}\n"
            + components
        )
        return eid

    def text(self):
        return "".join(self.parts)


# --- component snippets (indentation: 4 spaces = component key level) --------

def camera(fov=0.785398, far=2000.0):
    return (
        "    CameraComponent:\n"
        "      Camera:\n"
        "        ProjectionType: 0\n"
        f"        PerspectiveFOV: {f(fov)}\n"
        "        PerspectiveNear: 0.01\n"
        f"        PerspectiveFar: {f(far)}\n"
        "        OrthographicSize: 10\n"
        "        OrthographicNear: -1\n"
        "        OrthographicFar: 1\n"
        "      Primary: true\n"
        "      FixedAspectRatio: false\n"
    )


def dir_light(cast_shadows=False, intensity=2.0):
    # Shadows default OFF: PCSS dominates ScenePass (~93% in live A/B) and would
    # mask every axis these scenes try to isolate. kitchen_sink turns them on.
    return (
        "    DirectionalLightComponent:\n"
        "      Direction: [-0.3, -1, -0.2]\n"
        "      Color: [1, 1, 1]\n"
        f"      Intensity: {f(intensity)}\n"
        f"      CastShadows: {'true' if cast_shadows else 'false'}\n"
        "      ShadowBias: 0.005\n"
        "      ShadowNormalBias: 0.1\n"
        "      MaxShadowDistance: 300\n"
        "      CascadeSplitLambda: 0.95\n"
    )


def mesh(primitive):
    return f"    MeshComponent:\n      Primitive: {primitive}\n"


def material(albedo, metallic=0.1, roughness=0.6):
    return (
        "    MaterialComponent:\n"
        f"      AlbedoColor: {vec(albedo)}\n"
        f"      Metallic: {f(metallic)}\n"
        f"      Roughness: {f(roughness)}\n"
    )


def rigidbody(body_type, mass=1.0):
    return (
        "    Rigidbody3DComponent:\n"
        f"      BodyType: {body_type}\n"
        f"      Mass: {f(mass)}\n"
        "      LinearDrag: 0.05\n"
        "      AngularDrag: 0.05\n"
        "      DisableGravity: false\n"
        "      IsTrigger: false\n"
    )


def box_collider(half, restitution=0.2):
    return (
        "    BoxCollider3DComponent:\n"
        f"      HalfExtents: {vec(half)}\n"
        "      Offset: [0, 0, 0]\n"
        "      StaticFriction: 0.7\n"
        "      DynamicFriction: 0.7\n"
        f"      Restitution: {f(restitution)}\n"
    )


def sphere_collider(radius, restitution=0.3):
    return (
        "    SphereCollider3DComponent:\n"
        f"      Radius: {f(radius)}\n"
        "      Offset: [0, 0, 0]\n"
        "      StaticFriction: 0.5\n"
        "      DynamicFriction: 0.5\n"
        f"      Restitution: {f(restitution)}\n"
    )


def point_light(color, intensity=3.0, range_=6.0, cast_shadows=False):
    return (
        "    PointLightComponent:\n"
        f"      Color: {vec(color)}\n"
        f"      Intensity: {f(intensity)}\n"
        f"      Range: {f(range_)}\n"
        "      Attenuation: 1\n"
        f"      CastShadows: {'true' if cast_shadows else 'false'}\n"
        "      ShadowBias: 0.005\n"
        "      ShadowNormalBias: 0.1\n"
    )


def sprite(rgba):
    return f"    SpriteRendererComponent:\n      Color: {vec(rgba)}\n"


def lua_script(path):
    return f"    LuaScriptComponent:\n      ScriptFile: {path}\n"


def csharp_script(class_name):
    return f"    ScriptComponent:\n      ClassName: {class_name}\n"


def anim_fox(clip_index, time_offset):
    # SourceFilePath is relative to the project asset directory
    # (SandboxProject/Assets), so ../../assets/... reaches the editor's own
    # asset tree — same path fox.olo uses.
    return (
        "    MeshComponent:\n      {}\n"
        + material([1, 1, 1], 0.0, 0.6)
        + "    AnimationStateComponent:\n"
        "      State: 0\n"
        f"      CurrentTime: {f(time_offset)}\n"
        "      BlendDuration: 0.3\n"
        f"      CurrentClipIndex: {clip_index}\n"
        "      IsPlaying: true\n"
        "      SourceFilePath: ../../assets/models/Fox/Fox.gltf\n"
        "    SkeletonComponent:\n      {}\n"
    )


def environment_map():
    return (
        "    EnvironmentMapComponent:\n"
        "      FilePath: assets/textures/Skybox\n"
        "      IsCubemapFolder: true\n"
        "      EnableSkybox: true\n"
        "      Rotation: 0\n"
        "      Exposure: 1\n"
        "      BlurAmount: 0\n"
        "      EnableIBL: true\n"
        "      IBLIntensity: 1\n"
        "      Tint: [1, 1, 1]\n"
    )


# --- shared scene furniture ---------------------------------------------------

def add_camera(w, pos, rot, far=2000.0):
    w.camera_pos, w.camera_rot = pos, rot
    w.entity("Camera", pos, rot, (1, 1, 1), camera(far=far))


def camera_target(pos, rot):
    """Point 10 units along the camera forward (-Z rotated by pitch/yaw)."""
    pitch, yaw = rot[0], rot[1]
    fwd = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), -math.cos(yaw) * math.cos(pitch))
    return [pos[0] + 10 * fwd[0], pos[1] + 10 * fwd[1], pos[2] + 10 * fwd[2]]


def add_dir_light(w, cast_shadows=False):
    w.entity("Sun", (0, 0, 0), (0, 0, 0), (1, 1, 1), dir_light(cast_shadows))


def add_arena(w, half=80.0, wall_h=20.0):
    """Static physics arena: floor (top at y=0) + 4 walls.

    BoxCollider3D HalfExtents are in MESH-LOCAL units — JoltShapes multiplies
    them by the transform scale (a unit cube visual needs 0.5, not scale/2;
    getting this wrong trips ValidateBoxDimensions and silently falls back to
    a default shape).
    """
    unit_half = (0.5, 0.5, 0.5)
    w.entity("Floor", (0, -0.5, 0), (0, 0, 0), (2 * half, 1, 2 * half),
             mesh(1) + material([0.45, 0.45, 0.48], 0.2, 0.8)
             + rigidbody(0, 0) + box_collider(unit_half))
    walls = [
        ((half, wall_h / 2 - 0.5, 0), (1, wall_h, 2 * half)),
        ((-half, wall_h / 2 - 0.5, 0), (1, wall_h, 2 * half)),
        ((0, wall_h / 2 - 0.5, half), (2 * half, wall_h, 1)),
        ((0, wall_h / 2 - 0.5, -half), (2 * half, wall_h, 1)),
    ]
    for idx, (pos, scale) in enumerate(walls):
        w.entity(f"Wall{idx}", pos, (0, 0, 0), scale,
                 mesh(1) + material([0.35, 0.35, 0.4], 0.2, 0.9)
                 + rigidbody(0, 0) + box_collider(unit_half))


def add_ground_visual(w, half=150.0):
    w.entity("Ground", (0, -0.5, 0), (0, 0, 0), (2 * half, 1, 2 * half),
             mesh(1) + material([0.4, 0.42, 0.45], 0.1, 0.85))


# --- scene builders -----------------------------------------------------------

def build_physics_pile(w, n, rng):
    add_camera(w, (0, 70, 150), (-0.45, 0, 0), far=1000)
    add_dir_light(w)
    add_arena(w)
    side = math.ceil(n ** (1.0 / 3.0))
    spacing = 1.4
    origin = -(side - 1) * spacing / 2
    made = 0
    for layer in range(side):
        y = 2.0 + layer * spacing
        for ix in range(side):
            for iz in range(side):
                if made >= n:
                    return
                x = origin + ix * spacing + rng.uniform(-0.1, 0.1)
                z = origin + iz * spacing + rng.uniform(-0.1, 0.1)
                col = PALETTE[made % len(PALETTE)]
                if made % 2 == 0:
                    comps = (mesh(1) + material(col) + rigidbody(1)
                             + box_collider((0.5, 0.5, 0.5)))
                else:
                    comps = (mesh(2) + material(col, 0.1, 0.4) + rigidbody(1, 0.8)
                             + sphere_collider(0.5))
                w.entity(f"b{made}", (x, y, z), (0, 0, 0), (1, 1, 1), comps)
                made += 1


def build_physics_sleeping(w, n, rng):
    add_camera(w, (0, 60, 140), (-0.4, 0, 0), far=1000)
    add_dir_light(w)
    add_arena(w)
    per_layer = min(n, 100 * 100)
    side = math.ceil(math.sqrt(per_layer))
    spacing = 1.3
    origin = -(side - 1) * spacing / 2
    made = 0
    layer = 0
    while made < n:
        y = 0.5 + layer * 1.05
        for ix in range(side):
            for iz in range(side):
                if made >= n or ix * side + iz >= per_layer:
                    break
                x = origin + ix * spacing
                z = origin + iz * spacing
                w.entity(f"b{made}", (x, y, z), (0, 0, 0), (1, 1, 1),
                         mesh(1) + material(PALETTE[made % len(PALETTE)])
                         + rigidbody(1) + box_collider((0.5, 0.5, 0.5), restitution=0.0))
                made += 1
            if made >= n:
                break
        layer += 1


def build_physics_stacks(w, n, rng):
    add_camera(w, (0, 30, 110), (-0.25, 0, 0), far=1000)
    add_dir_light(w)
    add_arena(w)
    height = 10
    towers = math.ceil(n / height)
    side = math.ceil(math.sqrt(towers))
    spacing = 3.0
    origin = -(side - 1) * spacing / 2
    made = 0
    for t in range(towers):
        tx = origin + (t % side) * spacing
        tz = origin + (t // side) * spacing
        for level in range(height):
            if made >= n:
                return
            w.entity(f"b{made}", (tx, 0.5 + level * 1.0005, tz), (0, 0, 0), (1, 1, 1),
                     mesh(1) + material(PALETTE[level % len(PALETTE)])
                     + rigidbody(1) + box_collider((0.5, 0.5, 0.5), restitution=0.0))
            made += 1


def build_ecs_static(w, n, rng):
    add_camera(w, (0, 40, 120), (-0.3, 0, 0))
    add_dir_light(w)
    for i in range(n):
        w.entity(f"e{i}",
                 (rng.uniform(-250, 250), rng.uniform(0, 100), rng.uniform(-250, 250)),
                 (0, rng.uniform(0, 6.28), 0), (1, 1, 1), "")


def build_draws_unique(w, n, rng):
    add_camera(w, (0, 55, 110), (-0.5, 0, 0), far=1000)
    add_dir_light(w)
    add_ground_visual(w, 100)
    side = math.ceil(math.sqrt(n))
    spacing = 1.6
    origin = -(side - 1) * spacing / 2
    for i in range(n):
        x = origin + (i % side) * spacing
        z = origin + (i // side) * spacing
        # Unique material per entity: defeats batching, stresses per-draw CPU.
        albedo = hue_color(i / max(1, n))
        metallic = (i % 17) / 16.0
        roughness = 0.05 + 0.9 * (i % 23) / 22.0
        w.entity(f"d{i}", (x, 0.3, z), (0, 0, 0), (0.6, 0.6, 0.6),
                 mesh(1) + material(albedo, metallic, roughness))


def build_draws_instanced(w, n, rng):
    add_camera(w, (0, 120, 320), (-0.35, 0, 0), far=3000)
    add_dir_light(w)
    side = math.ceil(math.sqrt(n))
    spacing = 1.2
    origin = -(side - 1) * spacing / 2
    header = (
        "    InstancedMeshComponent:\n"
        "      Primitive: 1\n"
        "      FrustumCullPerInstance: true\n"
        "      CastShadows: false\n"
        "      CullDistance: 0\n"
        "      Instances:\n"
    )
    rows = []
    for i in range(n):
        x = origin + (i % side) * spacing
        z = origin + (i // side) * spacing
        c = hue_color(i / max(1, n))
        rows.append(
            f"        - Transform: [0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, {f(x)}, 0.25, {f(z)}, 1]\n"
            f"          Color: [{f(c[0])}, {f(c[1])}, {f(c[2])}, 1]\n"
        )
    w.entity("InstancedField", (0, 0, 0), (0, 0, 0), (1, 1, 1), header + "".join(rows))


def build_lights(w, n, rng, shadowed):
    add_camera(w, (0, 60, 120), (-0.5, 0, 0), far=1000)
    add_dir_light(w)
    # Keep the sun dim so the point lights dominate the shading cost.
    add_ground_visual(w, 75)
    rside = 30
    rspacing = 4.5
    rorigin = -(rside - 1) * rspacing / 2
    for i in range(rside * rside):
        x = rorigin + (i % rside) * rspacing
        z = rorigin + (i // rside) * rspacing
        w.entity(f"r{i}", (x, 0.5, z), (0, 0, 0), (1, 1, 1),
                 mesh(1) + material([0.8, 0.8, 0.8], 0.2, 0.3))
    lside = math.ceil(math.sqrt(n))
    lspacing = (rside * rspacing) / lside
    lorigin = -(lside - 1) * lspacing / 2
    for i in range(n):
        x = lorigin + (i % lside) * lspacing
        z = lorigin + (i // lside) * lspacing
        w.entity(f"L{i}", (x, 2.5, z), (0, 0, 0), (1, 1, 1),
                 point_light(hue_color(i / max(1, n)), cast_shadows=shadowed))


def build_anim_crowd(w, n, rng):
    side = math.ceil(math.sqrt(n))
    spacing = 10.0
    extent = side * spacing
    add_camera(w, (0, extent * 0.35 + 15, extent * 0.75 + 25), (-0.4, 0, 0), far=3000)
    add_dir_light(w)
    add_ground_visual(w, extent / 2 + 20)
    origin = -(side - 1) * spacing / 2
    for i in range(n):
        x = origin + (i % side) * spacing
        z = origin + (i // side) * spacing
        w.entity(f"fox{i}", (x, 0, z), (0, rng.uniform(0, 6.28), 0), (0.05, 0.05, 0.05),
                 anim_fox(clip_index=i % 3, time_offset=(i % 100) / 33.0))


def build_scripts_swarm(w, n, rng, lang):
    add_camera(w, (0, 40, 100), (-0.35, 0, 0))
    add_dir_light(w)
    # Visual anchor only (1 draw): swarm entities are meshless on purpose so
    # the scene isolates interop cost, but an all-grey frame is unverifiable.
    add_ground_visual(w, 80)
    side = math.ceil(math.sqrt(n))
    spacing = 2.0
    origin = -(side - 1) * spacing / 2
    if lang == "lua":
        script = lua_script("Scripts/LuaScripts/PerfBob.lua")
    else:
        script = csharp_script("Sandbox.PerfBob")
    for i in range(n):
        x = origin + (i % side) * spacing
        z = origin + (i // side) * spacing
        w.entity(f"s{i}", (x, 1, z), (0, 0, 0), (1, 1, 1), script)


def build_overdraw_sheets(w, n, rng):
    # Camera at z=50 looking down -Z; sheets span z in [30-…], each scaled to
    # overfill the frustum -> every sheet shades every pixel (alpha blended).
    add_camera(w, (0, 0, 50), (0, 0, 0), far=200)
    add_dir_light(w)
    for i in range(n):
        c = hue_color(i / max(1, n))
        w.entity(f"sheet{i}", (0, 0, 30 - i * 0.12), (0, 0, 0), (46, 26, 1),
                 sprite([c[0], c[1], c[2], 0.35]))


def build_kitchen_sink(w, n, rng):
    # n scales the physics pile; the other axes stay at fixed moderate counts.
    add_camera(w, (0, 130, 380), (-0.35, 0, 0), far=4000)
    w.entity("Sky", (0, 0, 0), (0, 0, 0), (1, 1, 1), environment_map())
    add_dir_light(w, cast_shadows=True)
    add_arena(w, half=60)

    # physics pile (n, default 1000)
    side = math.ceil(n ** (1.0 / 3.0))
    spacing = 1.4
    origin = -(side - 1) * spacing / 2
    for i in range(n):
        layer, rem = divmod(i, side * side)
        x = origin + (rem % side) * spacing + rng.uniform(-0.1, 0.1)
        z = origin + (rem // side) * spacing + rng.uniform(-0.1, 0.1)
        w.entity(f"b{i}", (x, 2 + layer * spacing, z), (0, 0, 0), (1, 1, 1),
                 mesh(1) + material(PALETTE[i % len(PALETTE)])
                 + rigidbody(1) + box_collider((0.5, 0.5, 0.5)))

    # 5000 transform-only entities
    for i in range(5000):
        w.entity(f"e{i}", (rng.uniform(-400, 400), rng.uniform(0, 80), rng.uniform(-400, 400)),
                 (0, 0, 0), (1, 1, 1), "")

    # 1500 unique-material cubes at x=+250
    uside = math.ceil(math.sqrt(1500))
    for i in range(1500):
        x = 250 + (i % uside - uside / 2) * 1.6
        z = (i // uside - uside / 2) * 1.6
        w.entity(f"d{i}", (x, 0.3, z), (0, 0, 0), (0.6, 0.6, 0.6),
                 mesh(1) + material(hue_color(i / 1500), (i % 17) / 16.0, 0.05 + 0.9 * (i % 23) / 22.0))

    # 20000 instances at x=-250
    iside = math.ceil(math.sqrt(20000))
    header = (
        "    InstancedMeshComponent:\n"
        "      Primitive: 1\n"
        "      FrustumCullPerInstance: true\n"
        "      CastShadows: false\n"
        "      CullDistance: 0\n"
        "      Instances:\n"
    )
    rows = []
    for i in range(20000):
        x = -250 + (i % iside - iside / 2) * 1.2
        z = (i // iside - iside / 2) * 1.2
        c = hue_color(i / 20000)
        rows.append(
            f"        - Transform: [0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, {f(x)}, 0.25, {f(z)}, 1]\n"
            f"          Color: [{f(c[0])}, {f(c[1])}, {f(c[2])}, 1]\n"
        )
    w.entity("InstancedField", (0, 0, 0), (0, 0, 0), (1, 1, 1), header + "".join(rows))

    # 128 point lights at z=+250
    for i in range(128):
        x = (i % 12 - 6) * 10
        z = 250 + (i // 12 - 5) * 10
        w.entity(f"L{i}", (x, 2.5, z), (0, 0, 0), (1, 1, 1),
                 point_light(hue_color(i / 128.0)))

    # 20 foxes at z=-250
    for i in range(20):
        w.entity(f"fox{i}", ((i % 5 - 2) * 10, 0, -250 + (i // 5 - 2) * 10),
                 (0, rng.uniform(0, 6.28), 0), (0.05, 0.05, 0.05),
                 anim_fox(i % 3, (i % 20) / 6.0))

    # 200 Lua scripts
    for i in range(200):
        w.entity(f"s{i}", ((i % 20 - 10) * 2, 30, (i // 20 - 5) * 2), (0, 0, 0), (1, 1, 1),
                 lua_script("Scripts/LuaScripts/PerfBob.lua"))

    # 6 translucent sheets low over the arena (fog-plane stand-ins). Kept out
    # of the camera's sightline — a full-screen sheet right at the lens blinds
    # the whole level view in screenshots.
    for i in range(6):
        c = hue_color(i / 6.0)
        w.entity(f"sheet{i}", (0, 12 + i * 2, -20 - i * 6), (-1.5708, 0, 0), (100, 100, 1),
                 sprite([c[0], c[1], c[2], 0.15]))


# scene id -> (builder, default_count, needs_play_mode, probe_tag, description)
# probe_tag: an entity the measurement driver samples twice to verify the
# workload actually runs (physics falls, scripts bob, animation plays) —
# a silent non-running workload otherwise reads as a great fps number.
SCENES = {
    "physics_pile":     (build_physics_pile,     10000, True, "b0",
                         "N dynamic bodies (boxes+spheres) falling into a walled arena"),
    "physics_sleeping": (build_physics_sleeping, 10000, True, None,
                         "N bodies resting on the floor; settle & sleep (sleeping-body writeback)"),
    "physics_stacks":   (build_physics_stacks,   5000,  True, None,
                         "Towers of 10 boxes; contact/solver heavy"),
    "ecs_static":       (build_ecs_static,       100000, True, None,
                         "N transform-only entities (spatial index rebuild, hierarchy panel floor)"),
    "draws_unique":     (build_draws_unique,     10000, False, None,
                         "N cubes with unique materials; defeats batching (CPU submission)"),
    "draws_instanced":  (build_draws_instanced,  200000, False, None,
                         "N instances of one cube in one InstancedMeshComponent (GPU instancing)"),
    "lights_many":      (build_lights,           512,   False, None,
                         "N unshadowed point lights over a receiver field (Forward+)"),
    "lights_shadowed":  (build_lights,           64,    False, None,
                         "N shadow-casting point lights over a receiver field"),
    "anim_crowd":       (build_anim_crowd,       200,   True, None,
                         "N skinned Fox characters, all playing (AnimationSystem)"),
    "scripts_swarm":    (build_scripts_swarm,    5000,  True, "s0",
                         "N entities each with a trivial per-frame Lua script"),
    "scripts_swarm_cs": (build_scripts_swarm,    2000,  True, "s0",
                         "N entities each with a trivial per-frame C# script (needs Sandbox-Scripting.dll)"),
    # Play mode required: world-space sprites only render on the runtime path
    # (Scene::OnUpdateRuntime's UIComposite callback) — in Edit mode the 3D
    # viewport draws no SpriteRendererComponents at all.
    "overdraw_sheets":  (build_overdraw_sheets,  80,    True, None,
                         "N stacked full-screen alpha-blended quads (fill rate / overdraw)"),
    "kitchen_sink":     (build_kitchen_sink,     1000,  True, "b0",
                         "Combined game level: physics+statics+draws+instances+lights+anim+scripts+overdraw"),
}


def build_scene(scene, count, seed):
    rng = random.Random(seed)
    name = f"{scene}_{count}"
    probe = SCENES[scene][3]
    w = SceneWriter(name + ".olo", watch_tags=(probe,) if probe else ())
    builder = SCENES[scene][0]
    if scene in ("lights_many", "lights_shadowed"):
        builder(w, count, rng, shadowed=(scene == "lights_shadowed"))
    elif scene in ("scripts_swarm", "scripts_swarm_cs"):
        builder(w, count, rng, lang=("lua" if scene == "scripts_swarm" else "cs"))
    else:
        builder(w, count, rng)
    return name, w


def main():
    ap = argparse.ArgumentParser(description="Generate OloEditor performance stress scenes (.olo)")
    ap.add_argument("--scene", required=False, default="all",
                    help="scene id or 'all' (see --list)")
    ap.add_argument("--count", type=int, default=0,
                    help="override the scene's default primary count (single-scene mode only)")
    ap.add_argument("--counts", type=str, default="",
                    help="comma-separated list of counts (single-scene mode; e.g. 1000,10000,50000)")
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--out-dir", type=pathlib.Path, default=DEFAULT_OUT_DIR)
    ap.add_argument("--list", action="store_true", help="list scene ids and defaults")
    args = ap.parse_args()

    if args.list:
        for k, (_, dflt, play, _probe, desc) in SCENES.items():
            print(f"{k:18s} default={dflt:<7d} play_mode={'yes' if play else 'no ':3s} {desc}")
        return 0

    if args.scene != "all" and args.scene not in SCENES:
        print(f"Unknown scene '{args.scene}'. Use --list.", file=sys.stderr)
        return 1
    if args.scene == "all" and (args.count or args.counts):
        print("--count/--counts only apply to a single --scene.", file=sys.stderr)
        return 1

    jobs = []
    if args.scene == "all":
        jobs = [(k, SCENES[k][1]) for k in SCENES]
    elif args.counts:
        jobs = [(args.scene, int(c)) for c in args.counts.split(",")]
    else:
        jobs = [(args.scene, args.count or SCENES[args.scene][1])]

    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out_dir / "manifest.json"
    manifest = {}
    if manifest_path.exists():
        try:
            manifest = json.loads(manifest_path.read_text())
        except json.JSONDecodeError:
            manifest = {}

    for scene, count in jobs:
        t0 = time.perf_counter()
        name, w = build_scene(scene, count, args.seed)
        text = w.text()
        out = args.out_dir / f"{name}.olo"
        out.write_text(text, encoding="utf-8", newline="\n")
        dt = time.perf_counter() - t0
        size_mb = out.stat().st_size / (1024 * 1024)
        manifest[name] = {
            "file": out.name,
            "scene": scene,
            "count": count,
            "entities": w.count,
            "sizeMB": round(size_mb, 2),
            "genSeconds": round(dt, 2),
            "needsPlayMode": SCENES[scene][2],
            "probeTag": SCENES[scene][3],
            "probeEntityId": str(w.tag_ids[SCENES[scene][3]]) if SCENES[scene][3] in w.tag_ids else None,
            "camera": {
                "position": list(w.camera_pos),
                "target": camera_target(w.camera_pos, w.camera_rot),
            },
            "description": SCENES[scene][4],
            "seed": args.seed,
        }
        print(f"{name:28s} entities={w.count:<7d} {size_mb:7.2f} MB  gen {dt:5.2f}s -> {out}")

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"manifest -> {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
