#!/usr/bin/env python3
# =============================================================================
# generate_animal_population_scene.py
#
# Emits the MIXED-DISTANCE ANIMAL POPULATION scene for issue #1258:
#   OloEditor/SandboxProject/Assets/Scenes/Benchmark/AnimalPopulation.olo
#
# WHY THIS IS A NEW SCENE AND NOT AN EDIT TO #1239'S FIXTURES
# -----------------------------------------------------------
# AnimalShortCoat.olo and AnimalLongCoat.olo are #1239's LIMITATION BASELINES.
# Their own headers say so: the long coat is a solid offset shell and the short
# coat carries no groom at all, deliberately, because those captures are what
# #1232/#1241/#1246/#1251/#1252 are measured against. "Improving a fixture to
# make its capture look better destroys the baseline it exists to record."
#
# So this scene sits beside them rather than replacing them. It is the first
# scene in the repository that puts MANY simultaneously groomed, LOD-enabled,
# independently moving animals in one frame — which is the population #1258's
# criterion 1 asks for and which nothing existing provides.
#
# WHAT MAKES IT REPRODUCIBLE
# --------------------------
# Every count, placement, phase and path parameter below is derived from one
# integer seed through a fixed LCG defined in this file. Not Python's `random`:
# its stream is an implementation detail that has changed between versions, and
# a scene whose contents depend on the interpreter is not reproducible in the
# sense criterion 1 means.
#
# The MOTION is reproducible for a different reason, and it is the load-bearing
# one: every animal carries an AnimalPathComponent, whose position is a CLOSED
# FORM of elapsed time rather than an accumulation. Capture the scene at 60 Hz
# or at 144 Hz and the animals are in the same places at the same simulated
# time — so two captures compare the same population instead of two different
# ones. A path integrated frame by frame would drift and quietly invalidate
# every measurement taken from it.
#
# Output is byte-stable: fixed seed, deterministic float formatting, no
# timestamps. The scene is COMMITTED; this generator exists because a
# 40-animal population with per-animal phases is unmaintainable by hand.
#
#   python OloEngine/tests/scripts/generate_animal_population_scene.py
#
# Every YAML key is verified against SceneSerializer.cpp or copied from a
# committed scene that uses it (GroomStrandCoat.olo, AnimalShortCoat.olo, and
# for the bound coats Dog.olo and GroomAnimals.olo).
#
# THE COATS ARE BOUND (#1259). Each animal is two entities: the Fox body (its
# model, clip and path) and its coat, parented to it and bound to it, the way
# a-bound-coat-rides-its-body.md says a bound coat is authored. The coats are
# FoxShortCoat / FoxLongCoat, grown on the Fox's bind-pose surface and cooked
# by FoxCoatEvidenceTest.ExportsTheFoxCoats (OLO_FOX_COATS_EXPORT=1), which is
# also the evidence for how they look. Their handles are READ from
# AssetRegistry.oar by path, so a re-export that assigns new ones needs only a
# re-run of this script.
# =============================================================================

import argparse
import math
import pathlib
import struct
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SCENE_DIR = REPO_ROOT / "OloEditor" / "SandboxProject" / "Assets" / "Scenes" / "Benchmark"
SCENE_PATH = SCENE_DIR / "AnimalPopulation.olo"

SEED = 1258

# The cooked coats, by their path in OloEditor/SandboxProject/AssetRegistry.oar.
REGISTRY_PATH = REPO_ROOT / "OloEditor" / "SandboxProject" / "AssetRegistry.oar"
FOX_SHORT_COAT = "Assets/Grooms/Animals/FoxShortCoat.ologroom"
FOX_SHORT_BINDING = "Assets/Grooms/Animals/FoxShortCoat.ologroombinding"
FOX_LONG_COAT = "Assets/Grooms/Animals/FoxLongCoat.ologroom"
FOX_LONG_BINDING = "Assets/Grooms/Animals/FoxLongCoat.ologroombinding"
FOX_COAT_COLOR = "Assets/Grooms/Animals/FoxCoatColor.png"
EXPORT_COMMAND = ("OLO_FOX_COATS_EXPORT=1 OloEngine-Tests "
                  "--gtest_filter=FoxCoatEvidenceTest.ExportsTheFoxCoats (cwd OloEditor/)")

# The coats are grown in METRES on the Fox's space times this (FoxCoatEvidenceTest's
# kFoxCookScale), and each coat entity carries the inverse under its fox. Then
# inverse(coatWorld) * foxWorld is the binding's SurfaceToGroom at every fox
# scale, and a fox placed at 0.014 wears the coat 12% larger than one at 0.0125.
FOX_COOK_SCALE = 0.0125
COAT_LOCAL_SCALE = 1.0 / FOX_COOK_SCALE  # exactly 80

# The whole cooked coat, EXACTLY: the strand counts ExportsTheFoxCoats prints.
# The distance ladder and the population budget thin it; the authored budget
# does not (density is not cut to buy frames). Not a round "everything" number:
# the strand cache keys a stream by the raw budget, so 1000000 and its ladder
# halvings 500000 and 250000 all built the same full coat under three keys, and
# the herd filled the 1 GiB strand cache several times over.
FOX_SHORT_STRANDS = 265724
FOX_LONG_STRANDS = 232394

# The card tier's hand-over, in pixels across the coat: what the cards were
# cooked for (GroomCardSettings::SourcePixelSize in FoxCoatEvidenceTest).
CARD_PIXEL_SIZE = 220.0

# AnimalRole, as AnimalBudgetComponent stores it.
ROLE_HERO = 0
ROLE_FEATURED = 1
ROLE_BACKGROUND = 2

# How many of each. Chosen so the DEFAULT High-tier budget (6000 units) is
# genuinely over-subscribed by the herd while the hero and the featured animals
# would fit on their own — a population that fits exercises nothing, and one
# that cannot be served at any step exercises only the refusal path.
HERD_COUNT = 36
FEATURED_COUNT = 4

# The hero faces the camera three-quarter on: the close-up is where the coat is
# judged, and a fox seen from behind shows a tail and two haunches.
HERO_YAW = 0.6


def f(x):
    """Compact float formatting: 4 significant decimals, no trailing zeros."""
    s = f"{x:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def vec(v):
    return "[" + ", ".join(f(c) for c in v) + "]"


def registry_handles(path=REGISTRY_PATH):
    """{asset path: handle} from AssetRegistry.oar (AssetRegistry::Serialize, v2).

    u32 version, u32 count, then per asset: u64 handle, u32 type, u32 status,
    u32 path length, the path's bytes (project-relative, forward slashes).
    """
    data = path.read_bytes()
    version, count = struct.unpack_from("<II", data, 0)
    if version != 2:
        raise SystemExit(f"{path}: registry format v{version}, this reader knows v2")
    offset = 8
    handles = {}
    for _ in range(count):
        handle, _type, _status, length = struct.unpack_from("<QIII", data, offset)
        offset += 20
        handles[data[offset:offset + length].decode("utf-8")] = handle
        offset += length
    return handles


def handle_of(handles, asset_path):
    if asset_path not in handles:
        raise SystemExit(f"{asset_path} is not in {REGISTRY_PATH}; cook and register it with: {EXPORT_COMMAND}")
    return handles[asset_path]


class Lcg:
    """A fixed 32-bit LCG (Numerical Recipes constants).

    NOT Python's `random`. Its stream is an implementation detail that has
    changed between interpreter versions, so a scene generated through it is
    reproducible only for as long as nobody upgrades Python — which is not what
    "reproducible counts and trajectories" means.
    """

    def __init__(self, seed):
        self._state = seed & 0xFFFFFFFF

    def next_u32(self):
        self._state = (1664525 * self._state + 1013904223) & 0xFFFFFFFF
        return self._state

    def unit(self):
        """Uniform in [0, 1)."""
        return self.next_u32() / 4294967296.0

    def range(self, lo, hi):
        return lo + (hi - lo) * self.unit()


class SceneWriter:
    def __init__(self, name, handle_base, note):
        self._lines = []
        self._handle = handle_base
        self._lines.append(f"Scene: {name}")
        self._lines.append("Version: 1")
        for line in note:
            self._lines.append(f"# {line}" if line else "#")
        self._lines.append("Entities:")

    def next_handle(self):
        self._handle += 1
        return self._handle

    def entity(self, tag, translation=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1),
               components="", comment=None, handle=None):
        if comment:
            self._lines.append(f"  # {comment}")
        self._lines.append(f"  - Entity: {handle if handle is not None else self.next_handle()}")
        self._lines.append("    TagComponent:")
        self._lines.append(f"      Tag: {tag}")
        self._lines.append("    TransformComponent:")
        self._lines.append(f"      Translation: {vec(translation)}")
        self._lines.append(f"      Rotation: {vec(rotation)}")
        self._lines.append(f"      Scale: {vec(scale)}")
        if components:
            self._lines.append(components.rstrip("\n"))

    def text(self):
        return "\n".join(self._lines) + "\n"


# =============================================================================
# Component emitters
# =============================================================================

def camera():
    return (
        "    CameraComponent:\n"
        "      Camera:\n"
        "        ProjectionType: 0\n"
        "        PerspectiveFOV: 0.7854\n"
        "        PerspectiveNear: 0.05\n"
        "        PerspectiveFar: 400\n"
        "        OrthographicSize: 10\n"
        "        OrthographicNear: -1\n"
        "        OrthographicFar: 1\n"
        "      Primary: true\n"
        "      FixedAspectRatio: false\n"
    )


def dir_light(direction, color, intensity):
    # Key spellings copied from generate_reference_fixture_scenes.py rather than
    # guessed. An unknown key here does NOT fail the load — it is silently
    # ignored — so a misspelling is a value that quietly never applies.
    # `CascadeSplitLambda` was spelled `CascadeLambda` in this generator's first
    # draft and did nothing at all.
    return (
        "    DirectionalLightComponent:\n"
        f"      Direction: {vec(direction)}\n"
        f"      Color: {vec(color)}\n"
        f"      Intensity: {f(intensity)}\n"
        "      CastShadows: true\n"
        "      ShadowDepthBiasTexels: 2\n"
        # The engine default (0.01), deliberately NOT the 0.1 the sample scenes
        # author: a 10x normal bias detaches contact shadows from their caster
        # (#1119), and what this scene exists to show is the coats.
        "      ShadowNormalBias: 0.01\n"
        f"      MaxShadowDistance: 220\n"
        "      CascadeSplitLambda: 0.9\n"
        "      CascadeDebugVisualization: false\n"
    )


def mesh(primitive):
    # `Primitive`, NOT `PrimitiveType`. SceneSerializer reads the former; the
    # latter deserialises as a bad conversion and takes the WHOLE SCENE down,
    # because one failed entity aborts the load. The first run of this
    # generator did exactly that, and the only symptom was
    # "SceneSerializer: 1 entities failed to deserialize - aborting" in
    # OloEngine.log next to an empty viewport.
    return f"    MeshComponent:\n      Primitive: {primitive}\n"


def material(albedo, metallic, roughness):
    return (
        "    MaterialComponent:\n"
        f"      AlbedoColor: {vec(albedo)}\n"
        f"      Metallic: {f(metallic)}\n"
        f"      Roughness: {f(roughness)}\n"
    )


def animated_model(source_path, clip_index=1, blend=0.3):
    """Skinned model + skeleton + playback state, AnimalShortCoat.olo's shape.

    SourceFilePath is a content path (#1539): `assets/...` names the editor's
    engine-content models, resolved by ResolveContentPath. SceneSerializer's
    animated branch calls ModelImporter::PopulateAnimatedEntity, so the empty
    maps below are placeholders the loader overwrites.
    """
    return (
        "    MeshComponent:\n"
        "      {}\n"
        "    AnimationStateComponent:\n"
        "      State: 0\n"
        "      CurrentTime: 0\n"
        f"      BlendDuration: {f(blend)}\n"
        f"      CurrentClipIndex: {clip_index}\n"
        "      IsPlaying: true\n"
        f"      SourceFilePath: {source_path}\n"
        "    SkeletonComponent:\n"
        "      {}\n"
    )


def groom(handle, strands):
    # Every key as Dog.olo spells it (SceneSerializer's own output).
    return (
        "    GroomComponent:\n"
        f"      Groom: {handle}\n"
        "      RootMarkerSize: 0.01\n"
        "      MaxPreviewStrands: 2000\n"
        f"      MaxRenderStrands: {strands}\n"
        "      WidthScale: 1\n"
        "      StrandColor: [1, 1, 1]\n"
        "      ShowPreview: false\n"
        "      ShowStrands: true\n"
        "      ShowRoots: true\n"
        "      ShowDirection: true\n"
        "      ColorByGroup: true\n"
        "      GuidesOnly: false\n"
        "      RenderStrands: true\n"
        "      CompositionMode: 1\n"
    )


# THE COAT'S LOOK, as FoxCoatEvidenceTest::MakeCoat authors it. Keep the two in
# step, or the evidence stops being evidence for this scene.
def groom_fibre():
    # Pale fibres; FoxCoatColor.png carries the fox's colours.
    return (
        "    GroomFibreComponent:\n"
        "      BaseColor: [1, 0.94, 0.86]\n"
        "      Absorption: [0.84, 1.39, 2.74]\n"
        "      Eumelanin: 1.3\n"
        "      Pheomelanin: 0\n"
        "      LongitudinalRoughness: 0.4\n"
        "      AzimuthalRoughness: 0.3\n"
        "      TiltDegrees: 2\n"
        "      IndexOfRefraction: 1.55\n"
        "      Intensity: 1\n"
        "      HSamples: 4\n"
        "      PigmentMode: 1\n"
        "      DebugMode: 0\n"
        "      Enabled: true\n"
    )


def groom_coat(color_map):
    # VariationSeed 0 on EVERY fox, deliberately. The seed is folded into the
    # coat's build digest (GroomRenderPass::CacheKey), so a fox with its own seed
    # gets its own ~350 MiB rest stream: with a seed per animal the 41 foxes asked
    # for ~15 GiB of strand geometry against the 1 GiB cache and the editor
    # stalled rebuilding them. One seed lets every fox share the two cooked
    # coats' streams.
    return (
        "    GroomCoatComponent:\n"
        "      RegionMap: 0\n"
        f"      ColorMap: {color_map}\n"
        "      UndercoatDensity: 1\n"
        "      UndercoatLength: 1\n"
        "      UndercoatWidth: 1\n"
        "      UndercoatClump: 1\n"
        "      GuardDensity: 1\n"
        "      GuardLength: 1\n"
        "      GuardWidth: 1\n"
        "      GuardClump: 1\n"
        "      LengthJitter: 0.2\n"
        "      WidthJitter: 0\n"
        "      ShadeJitter: 0.06\n"
        "      ClumpCellSize: 0.01\n"
        "      VariationSeed: 0\n"
        "      RoleVisibilityMask: 31\n"
        "      Enabled: true\n"
    )


def groom_coat_shadow():
    return (
        "    GroomCoatShadowComponent:\n"
        "      Kappa: 0.9\n"
        "      Resolution: 128\n"
        "      StepVoxels: 1.5\n"
        "      MaxLodSteps: 3\n"
        "      PixelSizeForLod0: 512\n"
        "      MinResolution: 8\n"
        "      Mode: 2\n"
        "      Enabled: true\n"
        "      MultipleScattering: true\n"
        "      BakeAtRest: true\n"
    )


def groom_scene_shadow():
    return (
        "    GroomSceneShadowComponent:\n"
        "      ShadowWidthTexels: 1\n"
        "      CastShadows: true\n"
        "      ReceiveShadows: true\n"
    )


def groom_simulation():
    # The engine defaults, as the evidence runs them.
    return (
        "    GroomSimulationComponent:\n"
        "      Gravity: [0, -9.81, 0]\n"
        "      Stiffness: 90\n"
        "      Damping: 6\n"
        "      VelocityCorrection: 0.85\n"
        "      FixedHz: 60\n"
        "      StretchTolerance: 0.01\n"
        "      TeleportDistance: 1\n"
        "      ColliderRadiusScale: 1\n"
        "      ColliderPadding: 0\n"
        "      ColliderFriction: 0.35\n"
        "      MaxSubsteps: 4\n"
        "      Iterations: 4\n"
        "      MaxGuidesUnassigned: 256\n"
        "      MaxGuidesUndercoat: 128\n"
        "      MaxGuidesGuardHair: 512\n"
        "      MaxGuidesWhisker: 64\n"
        "      MaxGuidesLongHair: 512\n"
        "      ResetKey: 0\n"
        "      Model: 1\n"
        "      DebugView: 0\n"
        "      Enabled: true\n"
        "      Collide: true\n"
    )


def groom_binding(binding, target):
    return (
        "    GroomBindingComponent:\n"
        f"      Binding: {binding}\n"
        f"      TargetEntity: {target}\n"
        "      TeleportDistance: 5\n"
        "      Enabled: true\n"
        "      ShowBindingPreview: false\n"
    )


def relationship(parent, children):
    # Both sides of the link, as Dog.olo writes them.
    lines = ["    RelationshipComponent:\n", f"      ParentHandle: {parent}\n", "      Children:\n"]
    if children:
        lines += [f"        - {child}\n" for child in children]
    else:
        lines.append("        []\n")
    return "".join(lines)


def groom_lod(card_px=CARD_PIXEL_SIZE, mesh_px=8.0):
    """The per-entity distance ladder from #1252.

    PRESENT ON EVERY ANIMAL, and that matters for what this scene measures:
    the population budget takes each animal's ladder answer as its DESIRED step
    and only ever coarsens from there. Without a ladder every animal would ask
    for full rate at every distance, and the budget would be measured against a
    workload no shipping scene would ever submit.
    """
    return (
        "    GroomLodComponent:\n"
        f"      CardPixelSize: {f(card_px)}\n"
        f"      MeshPixelSize: {f(mesh_px)}\n"
        "      Hysteresis: 0.15\n"
        "      MaxWidthCompensation: 8\n"
        "      VisibilityFullPixelSize: 512\n"
        "      SimulationFullPixelSize: 384\n"
        "      ShadowFullPixelSize: 512\n"
        "      HoldFrames: 4\n"
        "      VisibilitySteps: 4\n"
        "      SimulationSteps: 4\n"
        "      ShadowSteps: 3\n"
        "      Enabled: true\n"
    )


def animal_budget(role, motion_metres, deform=3, sim=4, vis=4, shadow=3):
    return (
        "    AnimalBudgetComponent:\n"
        f"      FullRateMotionMetres: {f(motion_metres)}\n"
        f"      MaxDeformationSteps: {deform}\n"
        f"      MaxSimulationSteps: {sim}\n"
        f"      MaxVisibilitySteps: {vis}\n"
        f"      MaxShadowSteps: {shadow}\n"
        f"      Role: {role}\n"
        "      Enabled: true\n"
    )


def animal_path(radius_x, radius_z, rate_x, rate_z, phase_x, phase_z, orient=True):
    return (
        "    AnimalPathComponent:\n"
        f"      RadiusX: {f(radius_x)}\n"
        f"      RadiusZ: {f(radius_z)}\n"
        f"      RateX: {f(rate_x)}\n"
        f"      RateZ: {f(rate_z)}\n"
        f"      PhaseX: {f(phase_x)}\n"
        f"      PhaseZ: {f(phase_z)}\n"
        "      OriginX: 0\n"
        "      OriginY: 0\n"
        "      OriginZ: 0\n"
        "      ElapsedSeconds: 0\n"
        "      HasOrigin: false\n"
        f"      FaceAlongMotion: {'true' if orient else 'false'}\n"
        "      Enabled: true\n"
    )


def settings():
    return (
        "PostProcessSettings:\n"
        "  Tonemapping: 2\n"
        "  Exposure: 1\n"
        "  BloomEnabled: true\n"
        "  BloomIntensity: 0.25\n"
        "  SSAOEnabled: true\n"
        "  FXAAEnabled: false\n"
    )


NOTE = [
    "MIXED-DISTANCE ANIMAL POPULATION (issue #1258), with bound fox coats (#1259).",
    "",
    "GENERATED — edit OloEngine/tests/scripts/generate_animal_population_scene.py,",
    "never this file.",
    "",
    "What this scene is for: it is the first scene in the repository that puts",
    "many simultaneously groomed, LOD-enabled, independently moving animals in",
    "one frame. #1239's AnimalShortCoat / AnimalLongCoat fixtures are single",
    "subjects and are deliberately frozen as limitation baselines, so neither",
    "can exercise a population budget.",
    "",
    "The population, per criterion 1:",
    f"  1 close-up HERO        long coat, full strand budget, hand-posed (no path)",
    f"  {FEATURED_COUNT} FEATURED animals    long coat, mid distance",
    f"  {HERD_COUNT} BACKGROUND animals  alternating short and long coats, receding",
    "",
    "Every animal carries an AnimalPathComponent whose position is a CLOSED",
    "FORM of elapsed time, so the population is in the same place at the same",
    "simulated time whatever the frame rate. The paths are LISSAJOUS FIGURES",
    "and not circles, deliberately: a herd on circles holds every animal at a",
    "constant distance from the camera, so the distance ladders never move and",
    "the budget is never exercised — the scene would look busy and measure",
    "nothing.",
    "",
    "The HERO has no path. A hero is hand-placed by definition; giving it one",
    "would make its apparent size a function of the clock and the hero-quality",
    "assertions unrepeatable.",
    "",
    f"The herd is sized ({HERD_COUNT} background animals) so the default High-tier",
    "budget is genuinely over-subscribed. A population that fits exercises",
    "nothing, and one that cannot be served at any step exercises only the",
    "refusal path.",
    "",
    f"Seed: {SEED}. Every placement, phase and rate below comes from it through",
    "the fixed LCG in the generator — not Python's `random`, whose stream is an",
    "interpreter implementation detail.",
    "",
    "THE COATS ARE BOUND. Each animal is a Fox body (model, clip, path) and a",
    "coat entity parented to it: FoxShortCoat or FoxLongCoat, grown on the Fox's",
    "own bind-pose surface, with a cooked card level, and bound to the body",
    "through GroomBindingComponent (a-bound-coat-rides-its-body.md). The coat is",
    f"authored in metres; its local scale {f(COAT_LOCAL_SCALE)} undoes the Fox's",
    "centimetres, so a fox placed at 0.011-0.014 wears it in proportion.",
    "The AnimalBudgetComponent sits on the coat, which holds the groom, and the",
    "scheduler poses the coat's body by it (multi-animal-scheduling-budgets.md",
    "rule 21). Evidence for how the coats look and that they stay on the walking",
    "fox: OloEngine/tests/Rendering/PropertyTests/FoxCoatEvidenceTest.cpp.",
]


def fox(w, handles, tag, translation, rotation, scale, long_coat, budget, path="", comment=None, casts=True):
    """One animal: the Fox body, and its coat parented to it and bound to it.

    `casts`: whether the coat casts into the scene's shadow maps. The hero and the
    featured foxes do; the herd does not. Each casting coat holds a caster entry
    per shadow step in the strand cache, and forty of them took the cache to
    217 entries and ~6 GiB; the herd's bodies still cast, and a background
    coat's own shadow is a few pixels.
    """
    body = w.next_handle()
    coat = w.next_handle()
    w.entity(
        tag, translation, rotation, scale,
        animated_model("assets/models/Fox/Fox.gltf", clip_index=1) + path + relationship(0, [coat]),
        comment=comment, handle=body,
    )
    groom_path, binding_path = (FOX_LONG_COAT, FOX_LONG_BINDING) if long_coat else (FOX_SHORT_COAT, FOX_SHORT_BINDING)
    w.entity(
        f"{tag} Coat", (0, 0, 0), (0, 0, 0), (COAT_LOCAL_SCALE,) * 3,
        groom_binding(handle_of(handles, binding_path), body)
        + groom_coat(handle_of(handles, FOX_COAT_COLOR))
        + groom_coat_shadow()
        + groom(handle_of(handles, groom_path), FOX_LONG_STRANDS if long_coat else FOX_SHORT_STRANDS)
        + groom_fibre()
        + groom_lod()
        + (groom_scene_shadow() if casts else "")
        + groom_simulation()
        + budget
        + relationship(body, []),
        handle=coat,
    )


def build():
    rng = Lcg(SEED)
    handles = registry_handles()
    w = SceneWriter("AnimalPopulation", 1_258_000_000_000_000, NOTE)

    w.entity("Camera", (0, 1.6, 9.0), (-0.06, 0, 0), (1, 1, 1), camera(),
             comment="── Camera (scene camera; capture manifests pose the EDITOR camera) ──")
    w.entity("Sun", components=dir_light((-0.5, -0.55, -0.67), (1.0, 0.95, 0.86), 5.0),
             comment="── Key light: raking enough to read a coat silhouette at distance ──")

    # ── The hero ────────────────────────────────────────────────────────
    #
    # Close to camera, full strand budget, NO path and NO deformation
    # halvings: the hero is the thing the budget exists to protect, and an
    # assertion that it stayed at full rate is only meaningful if its caps
    # would have allowed it to move.
    fox(w, handles, "Hero", (0.0, 0.0, 5.6), (0, HERO_YAW, 0), (0.014, 0.014, 0.014), True,
        animal_budget(ROLE_HERO, motion_metres=0.06, deform=3, sim=4, vis=4, shadow=3),
        comment="── THE HERO: close-up, full coat, protected by the budget ──")

    # ── Featured animals ────────────────────────────────────────────────
    for i in range(FEATURED_COUNT):
        angle = (i / FEATURED_COUNT) * math.tau
        x = 6.0 * math.cos(angle)
        z = -2.0 + 4.0 * math.sin(angle)
        rotation = (0, rng.range(0.0, math.tau), 0)
        budget = animal_budget(ROLE_FEATURED, motion_metres=rng.range(0.04, 0.09))
        path = animal_path(
            radius_x=rng.range(3.0, 6.0), radius_z=rng.range(2.0, 4.5),
            rate_x=rng.range(0.18, 0.34), rate_z=rng.range(0.25, 0.47),
            phase_x=rng.range(0.0, math.tau), phase_z=rng.range(0.0, math.tau),
        )
        fox(w, handles, f"Featured {i}", (x, 0.0, z), rotation, (0.012, 0.012, 0.012), True, budget, path,
            comment=("── Featured animals: mid distance, long coats ──" if i == 0 else None))

    # ── The herd ────────────────────────────────────────────────────────
    #
    # Alternating short and long coats, spread in depth so the population
    # covers the whole ladder at once: the near end is on strands at full
    # rate, the far end is on cards several halvings down, and the budget's
    # job is everything in between.
    for i in range(HERD_COUNT):
        long_coat = (i % 2) == 1
        depth = -8.0 - (i / HERD_COUNT) * 70.0
        lateral = rng.range(-26.0, 26.0)
        rotation = (0, rng.range(0.0, math.tau), 0)
        budget = animal_budget(ROLE_BACKGROUND, motion_metres=rng.range(0.03, 0.10))
        path = animal_path(
            radius_x=rng.range(4.0, 14.0), radius_z=rng.range(3.0, 10.0),
            rate_x=rng.range(0.14, 0.40), rate_z=rng.range(0.21, 0.58),
            phase_x=rng.range(0.0, math.tau), phase_z=rng.range(0.0, math.tau),
        )
        fox(w, handles, f"Herd {i}", (lateral, 0.0, depth), rotation, (0.011, 0.011, 0.011), long_coat, budget, path,
            comment=("── The herd: alternating short and long coats, receding ──" if i == 0 else None), casts=False)

    w.entity("Ground", (0, -0.02, 0), (0, 0, 0), (240, 0.04, 240),
             mesh(1) + material((0.24, 0.26, 0.18), 0.0, 0.92),
             comment="── Ground: matte earth ──")

    return w.text() + settings()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the committed scene differs from what this would emit")
    args = parser.parse_args()

    text = build()
    if args.check:
        if not SCENE_PATH.exists():
            print(f"{SCENE_PATH} does not exist", file=sys.stderr)
            return 1
        if SCENE_PATH.read_text(encoding="utf-8") != text:
            print(f"{SCENE_PATH} is stale — re-run this generator", file=sys.stderr)
            return 1
        print(f"{SCENE_PATH} is up to date")
        return 0

    SCENE_DIR.mkdir(parents=True, exist_ok=True)
    SCENE_PATH.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {SCENE_PATH} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
