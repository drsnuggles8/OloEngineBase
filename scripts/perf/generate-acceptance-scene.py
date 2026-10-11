"""Compose the #1259 integrated AAA acceptance scene and its capture manifests.

The scene joins committed content; it invents no substitute for a missing
representation (the composition and the gate were decided on #1259):

* the meadow-to-woodland flora traversal (`Scenes/FoliageMeadowToWoodland.olo`:
  terrain, six foliage layers, sky, sun), with scene wind switched on;
* the moving animal population (`Scenes/Benchmark/AnimalPopulation.olo`: hero,
  featured and herd foxes with their coats), laid along the traversal so the
  herd is seen near, mid and far through grass, shrubs and trees;
* the bound short- and long-coat horses and the rigged human head with its hair
  (`Scenes/GroomAnimals.olo`): the horses walk in the meadow, the head sits in a
  studio station beside the terrain for the digital-human close-up.

Every walking animal follows the ground (`AnimalPathComponent::FollowTerrain`),
every coat is parented to the body its binding targets
(docs/agent-rules/a-bound-coat-rides-its-body.md), and the sixteen subjects
nearest the cameras push the grass (`FoliageInteractionComponent`; the field has
sixteen slots, so more sources would only evict each other).

The manifests pin the decided gate: native 1920x1080, virtual shadow maps on, a
16.667 ms deadline. Run from the repository root:

    python scripts/perf/generate-acceptance-scene.py

and re-run it after changing any source scene. `--check` fails if the committed
output differs from what the sources generate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SCENES = ROOT / "OloEditor/SandboxProject/Assets/Scenes"
MANIFESTS = ROOT / "OloEditor/assets/benchmark/manifests"
OUTPUT = SCENES / "Benchmark/Acceptance.olo"
ENTITY_START = re.compile(r"(?m)^  - Entity: ")
TAG = re.compile(r"(?m)^      Tag: (.+)$")
TRANSLATION = re.compile(r"(?m)^(      Translation: )\[([^\]]+)\]$")
ROTATION = re.compile(r"(?m)^(      Rotation: )\[([^\]]+)\]$")

# ---------------------------------------------------------------------------
# Placement. The traversal runs from the meadow edge to the woodland along the
# nine poses FloraTraversalEvidenceTest / FoliageCostBaselineTest derive
# (Step0 eye at [252.4, 14.7, 237.3], Step8 at [163.8, 26.4, 73.8], heading
# 28.44 degrees; docs/analysis/foliage-cost-baseline-1391). The herd's local -Z
# (where AnimalPopulation spreads it, 8-78 m) is turned onto that heading, so
# the population stands near, mid and far along the walk.
# ---------------------------------------------------------------------------
TRAVERSAL_HEADING_DEG = 28.441923
HERD_ANCHOR = (230.0, 207.0)  # world XZ of the population's local origin, between Step1 and Step2
HORSE_STATION = (247.0, 229.0)  # beside Step0/Step1, in the open meadow
HORSE_SPACING = 5.0
STUDIO = (-34.0, 0.0, 196.0)  # off the terrain's [0, 256] footprint: a flat studio for the head
INTERACTION_SLOTS = 16
# The sun travels along Directional Light's [-0.4, -0.8, -0.3], so it stands
# at +X+Z. The head is turned to face away from it: the sun is then its
# backlight for the camera that looks into it, and the soft key lights the face.
SUN_FROM_XZ = (0.8, 0.6)
FACE_YAW = math.atan2(-SUN_FROM_XZ[0], -SUN_FROM_XZ[1])  # turns the head's +Z face onto -SUN_FROM_XZ
HEAD_CENTRE_ABOVE_STUDIO = 1.42
GROUND_GRID = ROOT / "scripts/perf/acceptance-ground.json"


def ground_at(x: float, z: float) -> float:
    """Terrain height at (x, z), bilinear over the committed 8 m grid.

    The grid is exported from the real height field by
    AcceptanceSceneTest.ExportsTheGroundHeightGrid (OLO_ACCEPTANCE_GROUND_EXPORT=1);
    AcceptanceSceneTest.EveryManifestCameraClearsTheTerrainAlongItsPath checks
    the cameras placed from it against the height field itself.
    """
    grid = json.loads(GROUND_GRID.read_text(encoding="utf-8"))
    step, heights = grid["stepMetres"], grid["heights"]
    fx = min(max(x / step, 0.0), grid["columns"] - 1.0)
    fz = min(max(z / step, 0.0), grid["rows"] - 1.0)
    x0, z0 = int(fx), int(fz)
    x1, z1 = min(x0 + 1, grid["columns"] - 1), min(z0 + 1, grid["rows"] - 1)
    tx, tz = fx - x0, fz - z0
    top = heights[z0][x0] * (1 - tx) + heights[z0][x1] * tx
    bottom = heights[z1][x0] * (1 - tx) + heights[z1][x1] * tx
    return top * (1 - tz) + bottom * tz


def rotate_y(local: list[float], yaw: float) -> list[float]:
    x, y, z = local
    return [x * math.cos(yaw) + z * math.sin(yaw), y, -x * math.sin(yaw) + z * math.cos(yaw)]


def look_at(eye: list[float], target: list[float]) -> tuple[float, float]:
    """Manifest yaw/pitch (degrees) for an EditorCamera at `eye` looking at `target`.

    EditorCamera's orientation is the quaternion of (-pitch, -yaw, 0), so yaw 0
    looks down -Z, yaw h looks along (sin h, -cos h), and a positive pitch looks
    down (EditorCamera::GetOrientation / GetForwardDirection).
    """
    dx, dy, dz = (t - e for t, e in zip(target, eye))
    horizontal = math.hypot(dx, dz)
    yaw = math.degrees(math.atan2(dx, -dz))
    pitch = math.degrees(math.atan2(-dy, horizontal))
    return yaw, pitch


def entities(text: str) -> list[str]:
    if "\nEntities:\n" not in text:
        raise ValueError("missing Entities block")
    body = text.split("\nEntities:\n", 1)[1]
    # Scene-level blocks may follow the entity list in some files.
    tail = re.search(r"(?m)^[A-Za-z]", body)
    if tail:
        body = body[: tail.start()]
    starts = [m.start() for m in ENTITY_START.finditer(body)]
    return [body[s:e].rstrip() for s, e in zip(starts, starts[1:] + [len(body)])]


def header_blocks(text: str) -> str:
    """Everything before `Entities:` minus the first two `Scene`/`Version` lines."""
    head = text.split("\nEntities:\n", 1)[0]
    lines = head.splitlines()
    return "\n".join(line for line in lines if not line.startswith(("Scene:", "Version:")))


def tag_of(block: str) -> str:
    match = TAG.search(block)
    return match.group(1).strip() if match else ""


def uuid_of(block: str) -> int:
    return int(block.split("\n", 1)[0].split(":", 1)[1])


def floats(text: str) -> list[float]:
    return [float(v) for v in text.split(",")]


def fmt(values: list[float]) -> str:
    return "[" + ", ".join(f"{v:.6g}" for v in values) + "]"


def set_translation(block: str, xyz: list[float]) -> str:
    out, n = TRANSLATION.subn(lambda m: m.group(1) + fmt(xyz), block, count=1)
    if n != 1:
        raise ValueError(f"no Translation in {tag_of(block)}")
    return out


def translation_of(block: str) -> list[float]:
    match = TRANSLATION.search(block)
    if not match:
        raise ValueError(f"no Translation in {tag_of(block)}")
    return floats(match.group(2))


def add_yaw(block: str, radians: float) -> str:
    def repl(m: re.Match) -> str:
        rx, ry, rz = floats(m.group(2))
        return m.group(1) + fmt([rx, ry + radians, rz])

    out, n = ROTATION.subn(repl, block, count=1)
    if n != 1:
        raise ValueError(f"no Rotation in {tag_of(block)}")
    return out


def append_component(block: str, component: str) -> str:
    return block.rstrip() + "\n" + component.rstrip()


def has_component(block: str, name: str) -> bool:
    return re.search(rf"(?m)^    {name}:", block) is not None


def set_scalar(block: str, key: str, value: str) -> str:
    out, n = re.subn(rf"(?m)^(\s+{key}: ).*$", lambda m: m.group(1) + value, block, count=1)
    if n != 1:
        raise ValueError(f"{key} not found in {tag_of(block)}")
    return out


def parent_of(block: str) -> int:
    match = re.search(r"(?m)^      ParentHandle: (\d+)$", block)
    return int(match.group(1)) if match else 0


def relationship(parent: int, children: list[int]) -> str:
    lines = ["    RelationshipComponent:", f"      ParentHandle: {parent}", "      Children:"]
    lines += [f"        - {c}" for c in children] if children else ["        []"]
    return "\n".join(lines)


def set_relationship(block: str, parent: int, children: list[int]) -> str:
    stripped = re.sub(r"(?m)^    RelationshipComponent:\n(?:      .*\n?)*", "", block + "\n").rstrip()
    return append_component(stripped, relationship(parent, children))


def animal_path(radius_x: float, radius_z: float, rate_x: float, rate_z: float, phase_x: float,
                phase_z: float) -> str:
    # Key spellings copied from Scenes/Benchmark/AnimalPopulation.olo.
    return "\n".join([
        "    AnimalPathComponent:",
        f"      RadiusX: {radius_x:.6g}",
        f"      RadiusZ: {radius_z:.6g}",
        f"      RateX: {rate_x:.6g}",
        f"      RateZ: {rate_z:.6g}",
        f"      PhaseX: {phase_x:.6g}",
        f"      PhaseZ: {phase_z:.6g}",
        "      OriginX: 0",
        "      OriginY: 0",
        "      OriginZ: 0",
        "      ElapsedSeconds: 0",
        "      HasOrigin: false",
        "      FaceAlongMotion: true",
        "      Enabled: true",
        "      FollowTerrain: true",
    ])


def foliage_interaction(radius: float, height: float) -> str:
    # Key spellings copied from Scenes/FoliageMeadowToWoodland.olo ("Walker").
    return "\n".join([
        "    FoliageInteractionComponent:",
        f"      Radius: {radius:.6g}",
        f"      Height: {height:.6g}",
        "      Strength: 1",
        "      Falloff: 2",
        "      RecoverySeconds: 0.8",
        "      TrailSpacing: 1.4",
        "      Enabled: true",
    ])


def follow_terrain(block: str) -> str:
    """Set FollowTerrain inside the AnimalPathComponent block itself.

    An unknown key under another component is silently ignored by the scene
    loader, so the key must land in this block and nowhere else.
    """
    match = re.search(r"(?m)^    AnimalPathComponent:\n((?:      .*\n?)*)", block + "\n")
    if not match:
        return block
    body = match.group(1)
    if re.search(r"(?m)^      FollowTerrain: ", body):
        body = re.sub(r"(?m)^(      FollowTerrain: ).*$", r"\1true", body)
    else:
        body = body.rstrip("\n") + "\n      FollowTerrain: true\n"
    text = block + "\n"
    return (text[: match.start(1)] + body + text[match.end(1):]).rstrip()


def herd_world(local: list[float]) -> list[float]:
    """Local AnimalPopulation XZ -> world XZ along the traversal heading (Y left to the terrain)."""
    theta = math.radians(TRAVERSAL_HEADING_DEG)
    x, y, z = local
    wx = HERD_ANCHOR[0] + x * math.cos(theta) + z * math.sin(theta)
    wz = HERD_ANCHOR[1] - x * math.sin(theta) + z * math.cos(theta)
    # Authored on the ground, so the scene is right in edit mode before any
    # path tick (FollowTerrain then keeps it there as the animal walks).
    return [wx, ground_at(wx, wz), wz]


def lantern_height() -> float:
    return ground_at(*HORSE_STATION) + 4.0


# The traversal scene is the flora source, authored for #1224's 256 m test
# walk: 30 m of relief that leaves most of the ground steep enough to be rock
# (which excludes every plant layer), snow on the tops, and 1.2-2.6 m grass that
# swallows a fox whole. The acceptance scene keeps its layers, meshes and
# densities and changes only what makes a readable meadow-to-forest: a gentler
# relief, no snow, meadow-height grass, and enough ground for the pines to make
# a wood. These are content choices for legibility; no layer is removed and no
# density is lowered.
TERRAIN_HEIGHT_SCALE = 16
LAYER_OVERRIDES = {
    "Meadow Grass": {"MinHeight": 0.55, "MaxHeight": 1.15},
    "Wildflowers": {"MinHeight": 0.45, "MaxHeight": 0.9},
    "Dune Grass": {"MinHeight": 0.5, "MaxHeight": 1.0},
    "Woodland Pines": {"Density": 0.07, "MaxMoisture": 0.62},
}


def patch_terrain(block: str) -> str:
    block = set_scalar(block, "HeightScale", str(TERRAIN_HEIGHT_SCALE))
    # Drop the snow rule (LayerIndex 3) from the auto-material.
    out, n = re.subn(r"(?m)^        - LayerIndex: 3\n(?:          .*\n)*", "", block + "\n", count=1)
    if n != 1:
        raise ValueError("terrain has no LayerIndex 3 (snow) rule to drop")
    block = out.rstrip()
    for name, fields in LAYER_OVERRIDES.items():
        start = block.find(f"        - Name: {name}\n")
        if start < 0:
            raise ValueError(f"foliage layer '{name}' not found")
        end = block.find("        - Name: ", start + 1)
        end = len(block) if end < 0 else end
        layer = block[start:end]
        for key, value in fields.items():
            layer, n = re.subn(rf"(?m)^(          {key}: ).*$", lambda m, v=value: m.group(1) + f"{v:g}", layer, count=1)
            if n != 1:
                raise ValueError(f"{name}.{key} not found")
        block = block[:start] + layer + block[end:]
    return block


def compose() -> str:
    flora_text = (SCENES / "FoliageMeadowToWoodland.olo").read_text(encoding="utf-8")
    meadow_text = (SCENES / "Benchmark/Meadow.olo").read_text(encoding="utf-8")
    animals_text = (SCENES / "Benchmark/AnimalPopulation.olo").read_text(encoding="utf-8")
    groom_text = (SCENES / "GroomAnimals.olo").read_text(encoding="utf-8")
    human_text = (SCENES / "DigitalHuman.olo").read_text(encoding="utf-8")
    courtyard_text = (SCENES / "Benchmark/Courtyard.olo").read_text(encoding="utf-8")

    # -- Scene-level blocks: the traversal's, with Meadow's wind (the traversal
    #    authors per-layer wind but leaves the scene wind off).
    header = header_blocks(flora_text)
    wind = re.search(r"(?m)^WindSettings:\n(?:  .*\n)+", meadow_text + "\n")
    if not wind:
        raise ValueError("Meadow.olo has no WindSettings block")
    header = re.sub(r"(?m)^WindSettings:\n(?:  .*\n)*", lambda _: wind.group(0), header + "\n").rstrip()

    out: list[str] = []
    uuids: set[int] = set()

    def keep(block: str) -> None:
        uid = uuid_of(block)
        if uid in uuids:
            raise ValueError(f"duplicate entity UUID {uid} ({tag_of(block)})")
        uuids.add(uid)
        out.append(block)

    # -- Flora traversal: terrain + foliage, sky, sun. The traversal's own
    #    "Walker" stand-in and its camera are left out: the animals push the
    #    grass here, and the manifests own the cameras.
    for block in entities(flora_text):
        if tag_of(block) == "Meadow To Woodland Terrain":
            keep(patch_terrain(block))
        elif tag_of(block) in {"Sky", "Directional Light"}:
            keep(block)

    # -- The animal population, turned onto the traversal heading.
    population = [b for b in entities(animals_text) if tag_of(b) not in {"Camera", "Sun", "Ground"}]
    theta = math.radians(TRAVERSAL_HEADING_DEG)
    interaction_left = INTERACTION_SLOTS - 2  # the two horses take two slots
    by_distance = []
    for block in population:
        if parent_of(block):
            continue  # a coat parented to its fox moves with it
        x, _, z = translation_of(block)
        by_distance.append((abs(z), tag_of(block)))
    pushers = {tag for _, tag in sorted(by_distance)[:interaction_left]}
    for block in population:
        # The animal bodies are the root, animated entities; their coats are
        # parented to them (and carry the AnimalBudgetComponent), so they follow.
        if not parent_of(block) and has_component(block, "AnimationStateComponent"):
            block = set_translation(block, herd_world(translation_of(block)))
            block = add_yaw(block, theta)
            if has_component(block, "AnimalPathComponent"):
                block = follow_terrain(block)
            else:
                # The pathless hero walks a small figure of its own, so the
                # moving character is a skinned, coated, ground-following one.
                block = append_component(block, animal_path(2.4, 1.6, 0.31, 0.47, 0.0, 0.0))
            if tag_of(block) in pushers:
                block = append_component(block, foliage_interaction(1.1, 1.0))
        keep(block)

    # -- Horses with their bound coats, walking in the meadow.
    groom_blocks = {tag_of(b): b for b in entities(groom_text)}
    for index, (horse_tag, coat_tag) in enumerate([("ShortCoatHorse", "ShortCoatHorseCoat"),
                                                   ("LongCoatHorse", "LongCoatHorseCoat")]):
        horse, coat = groom_blocks[horse_tag], groom_blocks[coat_tag]
        lateral = (index - 0.5) * HORSE_SPACING
        hx = HORSE_STATION[0] + lateral * math.cos(theta)
        hz = HORSE_STATION[1] - lateral * math.sin(theta)
        position = [hx, ground_at(hx, hz), hz]
        horse = set_translation(horse, position)
        horse = append_component(horse, animal_path(3.0 + index, 2.2 + index, 0.21, 0.29 + 0.04 * index,
                                                     1.7 * index, 0.9 * index))
        horse = append_component(horse, foliage_interaction(1.6, 1.8))
        horse = set_relationship(horse, 0, [uuid_of(coat)])
        coat = set_relationship(coat, uuid_of(horse), [])
        keep(horse)
        keep(coat)

    # -- Head studio: the rigged head and its hair on a plinth, a floor and a
    #    backdrop, lit by DigitalHuman's soft key and hard side. The scene sun is
    #    the head's backlight (Forward+ transmits skin light for the directional
    #    light only; docs/guides/skin-transmission.md).
    sx, sy, sz = STUDIO
    human, hair, plinth = groom_blocks["Human"], groom_blocks["HumanCoat"], groom_blocks["Plinth"]
    human = set_translation(human, [sx, sy + 1.24, sz])
    human = add_yaw(human, FACE_YAW)
    # The rigged scan's imported HeadSkin patched to the reference skin
    # (Kind 2 = Skin, ReferenceHead.oloskin, 20 mm thickness factor); the
    # measured thickness map rides in HeadRigged.gltf. Key spellings from
    # Dog.olo. No MaterialComponent: it would override every submesh and
    # switch the patch off.
    human = append_component(human, "\n".join([
        "    MaterialOverridesComponent:",
        "      Overrides:",
        "        - MaterialName: HeadSkin",
        "          Kind: 2",
        "          SkinProfile: 15952688685437936278",
        "          ThicknessFactor: 0.02",
        "          OverrideBaseColor: false",
        "          BaseColor: [1, 1, 1, 1]",
        "          OverrideRoughness: false",
        "          Roughness: 0.5",
        "          OverrideMetallic: false",
        "          Metallic: 0",
    ]))
    plinth = set_translation(plinth, [sx, sy + 0.62, sz])
    human = set_relationship(human, 0, [uuid_of(hair)])
    hair = set_relationship(hair, uuid_of(human), [])
    for block in (human, hair, plinth):
        keep(block)
    studio_floor = groom_blocks["Ground"]
    studio_floor = set_translation(studio_floor, [sx, sy - 0.02, sz])
    studio_floor = re.sub(r"(?m)^      Scale: \[[^\]]+\]$", "      Scale: [200, 0.04, 200]", studio_floor, count=1)
    studio_floor = re.sub(r"(?m)^      Tag: .+$", "      Tag: Studio Floor", studio_floor, count=1)
    keep(studio_floor)
    head_centre = [sx, sy + HEAD_CENTRE_ABOVE_STUDIO, sz]
    # DigitalHuman's key and side lights, in the head's frame. Its backdrop is
    # left out: on the sun's side of the turned head it would shadow the backlight.
    for block in entities(human_text):
        if tag_of(block) in {"Soft Key", "Hard Side"}:
            local = rotate_y(translation_of(block), FACE_YAW)
            block = set_translation(block, [head_centre[i] + local[i] for i in range(3)])
            keep(block)

    # -- The moving light: Courtyard's shadowed point light, carried over the
    #    horse station by the manifests' EntityMotion.
    for block in entities(courtyard_text):
        if tag_of(block) == "PLight Shadow 0":
            block = re.sub(r"(?m)^      Tag: .+$", "      Tag: Moving Lantern", block, count=1)
            block = set_translation(block, [HORSE_STATION[0], lantern_height(), HORSE_STATION[1]])
            block = set_scalar(block, "Range", "14")
            block = set_scalar(block, "Intensity", "12")
            keep(block)

    note = (
        "# GENERATED by scripts/perf/generate-acceptance-scene.py (#1259). Do not edit by hand:\n"
        "# change a source scene or the generator, then regenerate.\n"
        "# Meadow-to-woodland flora traversal, the moving fox population with bound coats along it,\n"
        "# the bound short- and long-coat horses walking in the meadow, and the rigged human head\n"
        "# with its hair in a studio beside the terrain.\n"
    )
    return "Scene: Acceptance\nVersion: 1\n" + note + header + "\nEntities:\n" + "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Manifests
# ---------------------------------------------------------------------------
GATE_MS = 16.667
DT = 1.0 / 60.0


def camera(cam_id: str, position: list[float], yaw: float, pitch: float, fov: float,
           motion: dict | None = None, warmup: int | None = None) -> dict:
    entry = {"Id": cam_id, "Position": position, "YawDegrees": yaw, "PitchDegrees": pitch,
             "FovDegrees": fov, "Near": 0.05, "Far": 600.0}
    if warmup is not None:
        entry["WarmupFrames"] = warmup
    if motion:
        entry["Motion"] = {"VelocityPerSecond": [0.0, 0.0, 0.0], "YawRateDegreesPerSecond": 0.0,
                           "PitchRateDegreesPerSecond": 0.0, **motion}
    return entry


def outdoor_camera(cam_id: str, eye_xz: tuple[float, float], eye_above: float,
                   target_xz: tuple[float, float], target_above: float, fov: float,
                   motion: dict | None = None) -> dict:
    eye = [eye_xz[0], ground_at(*eye_xz) + eye_above, eye_xz[1]]
    target = [target_xz[0], ground_at(*target_xz) + target_above, target_xz[1]]
    yaw, pitch = look_at(eye, target)
    return camera(cam_id, [round(v, 3) for v in eye], round(yaw, 3), round(pitch, 3), fov, motion)


def cameras() -> list[dict]:
    sx, sy, sz = STUDIO
    head = [sx, sy + HEAD_CENTRE_ABOVE_STUDIO, sz]

    def head_camera(cam_id: str, local_eye: list[float], fov: float) -> dict:
        offset = rotate_y(local_eye, FACE_YAW)
        eye = [head[i] + offset[i] for i in range(3)]
        yaw, pitch = look_at(eye, [head[0], head[1] + 0.02, head[2]])
        return camera(cam_id, [round(v, 3) for v in eye], round(yaw, 3), round(pitch, 3), fov)

    heading = math.radians(TRAVERSAL_HEADING_DEG)
    forward = (-math.sin(heading), -math.cos(heading))
    dolly_speed = 4.0
    dolly_start = (HERD_ANCHOR[0] + 6.0 * math.sin(heading), HERD_ANCHOR[1] + 6.0 * math.cos(heading))
    dolly_target = (dolly_start[0] + 40.0 * forward[0], dolly_start[1] + 40.0 * forward[1])
    # The dolly holds one height (its motion is linear), so the height comes
    # from the highest ground along the whole travel, not from the start.
    travel = dolly_speed * (96 + 300) * DT
    highest = max(ground_at(dolly_start[0] + t * forward[0], dolly_start[1] + t * forward[1])
                  for t in [travel * i / 16 for i in range(17)])
    eye = [dolly_start[0], highest + 2.4, dolly_start[1]]
    yaw, pitch = look_at(eye, [dolly_target[0], ground_at(*dolly_target) + 0.6, dolly_target[1]])
    dolly = camera("population-dolly", [round(v, 3) for v in eye], round(yaw, 3), round(pitch, 3), 55.0,
                   motion={"VelocityPerSecond": [round(forward[0] * dolly_speed, 4), 0.0,
                                                 round(forward[1] * dolly_speed, 4)]})

    # From the sun's side, turned 50 degrees off it: the bearings nearer the sun
    # line look through the willow shrubs that ring the station (checked live).
    bearing = math.radians(50.0)
    coat_dir = (SUN_FROM_XZ[0] * math.cos(bearing) - SUN_FROM_XZ[1] * math.sin(bearing),
                SUN_FROM_XZ[0] * math.sin(bearing) + SUN_FROM_XZ[1] * math.cos(bearing))
    coat_eye = (HORSE_STATION[0] + 8.0 * coat_dir[0], HORSE_STATION[1] + 8.0 * coat_dir[1])
    turn_eye = (HERD_ANCHOR[0] + 18.0 * forward[0], HERD_ANCHOR[1] + 18.0 * forward[1])
    return [
        head_camera("head-closeup", [0.34, 0.06, 0.62], 30.0),
        head_camera("head-backlit", [-0.08, 0.02, 0.78], 30.0),
        outdoor_camera("coats-closeup", coat_eye, 3.4, HORSE_STATION, 1.3, 40.0),
        dolly,
        outdoor_camera("rapid-turn", turn_eye, 3.0, (turn_eye[0] + 30.0 * forward[0], turn_eye[1] + 30.0 * forward[1]), 1.0,
                       60.0, motion={"YawRateDegreesPerSecond": 90.0}),
    ]


ATTACHMENTS_DEFERRED = [
    {"Name": "Beauty", "Source": "UIComposite", "Format": "png"},
    {"Name": "SceneColorHDR", "Source": "SceneColor", "Format": "hdr"},
    {"Name": "Albedo", "Source": "GBufferAlbedo", "Format": "png"},
    {"Name": "Normals", "Source": "GBufferNormal", "Format": "png", "Normalize": "none"},
    {"Name": "Velocity", "Source": "Velocity", "Format": "png", "Normalize": "on"},
    {"Name": "SceneDepth", "Source": "SceneDepth", "Format": "hdr"},
]
FORWARD_NAMES = {"Beauty", "SceneColorHDR", "Velocity", "SceneDepth"}
PATHS = [("forward", "Forward", "gl-forward-native", "vk-forward-native"),
         ("forward-plus", "ForwardPlus", "gl-forward-plus-native", "vk-forward-plus-native"),
         ("deferred", "Deferred", "gl-deferred-native", "vk-deferred-native")]


def base_manifest(scene_sha: str, assets: list[dict]) -> dict:
    return {
        "ManifestVersion": 2,
        "Id": "",
        "Product": "diagnostic",
        "Scene": "Scenes/Benchmark/Acceptance.olo",
        "Description": ("#1259 integrated AAA acceptance workload: animated digital-human close-up, "
                        "bound short- and long-coat horses, and a moving fox population with bound coats "
                        "through meadow grass, shrubs and woodland, with a moving shadowed light."),
        "Backends": {"Supported": ["opengl", "vulkan"], "Features": {"vulkan": {"UnsupportedAttachments": []}}},
        "Cameras": cameras(),
        "Output": {"Resolution": [1920, 1080], "RenderScale": 1.0},
        "RendererSettings": {},
        "Exposure": {"Mode": "Manual", "Exposure": 1.0},
        "Determinism": {"Seed": 1259, "StartTimeSeconds": 12.0, "FixedDtSeconds": DT},
        "Warmup": {"Frames": 96, "PerFeature": {"TAA": 8, "SSAO": 4, "Exposure": 30}},
        "Measurement": {"Frames": 300, "DeadlineMs": GATE_MS},
        "Attachments": ATTACHMENTS_DEFERRED,
        "Tolerance": {"RepeatRmse": 0.0},
        "Assets": assets,
        "EntityMotion": [
            {"Tag": "Moving Lantern", "Origin": [HORSE_STATION[0], round(lantern_height(), 3), HORSE_STATION[1]],
             "Amplitude": [6.0, 1.0, 4.0], "FrequencyHz": 0.25, "PhaseRadians": 0.0},
        ],
        "Presets": {},
    }


def manifests(scene_sha: str) -> dict[str, dict]:
    sources = [yaml.safe_load((MANIFESTS / name).read_text(encoding="utf-8"))
               for name in ("animal-population.diagnostic.yaml", "meadow.diagnostic.yaml")]
    assets = {a["Path"]: a for s in sources for a in s["Assets"]}
    scene_path = "SandboxProject/Assets/Scenes/Benchmark/Acceptance.olo"
    assets[scene_path] = {
        "Path": scene_path,
        "Origin": "generated in-repo from FoliageMeadowToWoodland, AnimalPopulation, GroomAnimals, "
                  "DigitalHuman, Meadow and Courtyard",
        "License": "repository license (no third-party rights)",
        "Redistribution": "committed",
        "LicenseVerified": "in-repo-file",
        "Version": "generated",
        "Sha256": scene_sha,
        "Units": "metres",
        "UpAxis": "+Y",
        "ColorSpace": "n/a",
        "Acquisition": "run python scripts/perf/generate-acceptance-scene.py",
    }
    asset_list = list(assets.values())
    result: dict[str, dict] = {}
    for slug, path, gl_preset, vk_preset in PATHS:
        m = base_manifest(scene_sha, asset_list)
        m["Id"] = f"acceptance-{slug}"
        m["Presets"] = {"opengl": gl_preset, "vulkan": vk_preset}
        m["RendererSettings"] = {"Path": path, "EnableDDGI": False, "TAAEnabled": True,
                                 "RayTracedShadowsEnabled": False, "VirtualShadowMaps": True,
                                 "MSAASampleCount": 1, "Upscale": "Off", "UpscaleTechnique": "Spatial"}
        if path != "Deferred":
            m["Attachments"] = [a for a in ATTACHMENTS_DEFERRED if a["Name"] in FORWARD_NAMES]
        result[m["Id"]] = m

        # Motion footage: the same cameras and scene motion, no timing, a
        # readback every second frame for two seconds per camera.
        motion = dict(m)
        motion["Id"] = f"acceptance-{slug}-motion"
        motion.pop("Measurement")
        names = [a["Name"] for a in m["Attachments"] if a["Name"] in {"Beauty", "Velocity", "Albedo", "Normals"}]
        motion["Cameras"] = [{**c, "Sequence": {"Every": 2, "Count": 60, "Attachments": names}} for c in cameras()]
        result[motion["Id"]] = motion

        # One internal scale with an upscaler actually consumed (#1480 readback).
        upscale = dict(m)
        upscale["Id"] = f"acceptance-{slug}-upscale-quality"
        upscale["RendererSettings"] = {**m["RendererSettings"], "Upscale": "Quality"}
        result[upscale["Id"]] = upscale

        # The CSM control, so the VSM gate's cost is attributable.
        csm = dict(m)
        csm["Id"] = f"acceptance-{slug}-csm"
        csm["RendererSettings"] = {**m["RendererSettings"], "VirtualShadowMaps": False}
        result[csm["Id"]] = csm

    hybrid = base_manifest(scene_sha, asset_list)
    hybrid["Id"] = "acceptance-hybrid"
    hybrid["Backends"] = {"Supported": ["vulkan"], "Features": {"vulkan": {"UnsupportedAttachments": []}}}
    hybrid["Presets"] = {"vulkan": "vk-hybrid-native"}
    hybrid["RendererSettings"] = {"Path": "Deferred", "EnableDDGI": False, "TAAEnabled": True,
                                  "RayTracedShadowsEnabled": True, "VirtualShadowMaps": True,
                                  "MSAASampleCount": 1, "Upscale": "Off", "UpscaleTechnique": "Spatial"}
    result[hybrid["Id"]] = hybrid

    # Component AOVs: one lighting tap per run on Deferred (#1526; the five
    # radiance taps sum to the lit colour), at the head and coat close-ups.
    for tap in ("DirectDiffuse", "DirectSpecular", "IndirectDiffuse", "IndirectSpecular", "Remainder"):
        t = base_manifest(scene_sha, asset_list)
        t["Id"] = "acceptance-deferred-tap-" + re.sub(r"(?<!^)(?=[A-Z])", "-", tap).lower()
        t["Presets"] = {"opengl": "gl-deferred-native", "vulkan": "vk-deferred-native"}
        t["RendererSettings"] = {"Path": "Deferred", "EnableDDGI": False, "TAAEnabled": True,
                                 "RayTracedShadowsEnabled": False, "VirtualShadowMaps": True,
                                 "MSAASampleCount": 1, "Upscale": "Off", "UpscaleTechnique": "Spatial",
                                 "LightingTap": tap}
        t["Cameras"] = [c for c in cameras() if c["Id"] in {"head-closeup", "head-backlit", "coats-closeup"}]
        t["Attachments"] = [a for a in ATTACHMENTS_DEFERRED if a["Name"] in {"SceneColorHDR", "Beauty"}]
        t.pop("Measurement")
        result[t["Id"]] = t
    return result


def render_manifest(manifest: dict) -> str:
    return ("# GENERATED by scripts/perf/generate-acceptance-scene.py (#1259). Do not edit by hand.\n"
            + yaml.safe_dump(manifest, sort_keys=False, allow_unicode=True))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--check", action="store_true", help="fail if committed output is stale")
    args = parser.parse_args()

    scene = compose()
    scene_sha = hashlib.sha256(scene.encode("utf-8")).hexdigest()
    outputs = {OUTPUT: scene}
    outputs.update({MANIFESTS / f"{mid}.yaml": render_manifest(m) for mid, m in manifests(scene_sha).items()})

    stale = []
    for path, text in outputs.items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(path.relative_to(ROOT))
    if stale:
        for path in stale:
            print(f"stale: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
