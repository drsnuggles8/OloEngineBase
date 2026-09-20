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
# committed scene that uses it (GroomStrandCoat.olo, AnimalShortCoat.olo).
# =============================================================================

import argparse
import math
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SCENE_DIR = REPO_ROOT / "OloEditor" / "SandboxProject" / "Assets" / "Scenes" / "Benchmark"
SCENE_PATH = SCENE_DIR / "AnimalPopulation.olo"

SEED = 1258

# Cooked groom handles, read out of OloEditor/SandboxProject/AssetRegistry.oar.
# The registry is a BINARY file (grepping it for a handle returns nothing), so
# these are recorded here rather than looked up — and a wrong one fails loudly:
# AssetManager logs the miss and the entity renders with no coat, which is
# visible in the first capture rather than subtle.
GROOM_LONGCOAT = 15952688685437936287
GROOM_SHORTCOAT = 15952688685437936285

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


def f(x):
    """Compact float formatting: 4 significant decimals, no trailing zeros."""
    s = f"{x:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def vec(v):
    return "[" + ", ".join(f(c) for c in v) + "]"


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
        for line in note:
            self._lines.append(f"# {line}" if line else "#")
        self._lines.append("Entities:")

    def next_handle(self):
        self._handle += 1
        return self._handle

    def entity(self, tag, translation=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1),
               components="", comment=None):
        if comment:
            self._lines.append(f"  # {comment}")
        self._lines.append(f"  - Entity: {self.next_handle()}")
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

    SourceFilePath is relative to Project::GetAssetDirectory(), which is why
    the editor-tree models reach out through `../../assets/`. SceneSerializer's
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


def groom(handle, max_strands, width_scale, color):
    return (
        "    GroomComponent:\n"
        f"      Groom: {handle}\n"
        "      RenderStrands: true\n"
        f"      MaxRenderStrands: {max_strands}\n"
        f"      WidthScale: {f(width_scale)}\n"
        f"      StrandColor: {vec(color)}\n"
        "      ShowPreview: false\n"
    )


def groom_lod(card_px=220.0, mesh_px=8.0):
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
        f"      OrientToPath: {'true' if orient else 'false'}\n"
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
    "MIXED-DISTANCE ANIMAL POPULATION (issue #1258).",
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
    "KNOWN LIMITATION, STATED RATHER THAN HIDDEN. The grooms here are drawn at",
    "each animal's own transform, NOT bound to the fox body: there is no cooked",
    "GroomBindingComponent binding for the Fox rig, and building one is #1249's",
    "surface rather than #1258's. The Fox is authored in centimetres and placed",
    "at a scale of ~0.012, so each coat renders as a small blob at the animal's",
    "origin instead of sitting on its back.",
    "",
    "That does not affect what this scene is FOR. The budget is measured on the",
    "work each animal submits — strand counts, guide counts, bone counts — and",
    "the editor log shows the scheduler assigning different strides (1, 2, 3, 4)",
    "across this population, which is the whole point. But this is a SCHEDULING",
    "fixture, not a finished groomed herd, and it should not be cited as one.",
    "The budget's visual evidence lives in",
    "OloEngine/tests/Rendering/PropertyTests/AnimalBudgetVisualEvidenceTest.cpp,",
    "whose grooms are correctly scaled.",
]


def build():
    rng = Lcg(SEED)
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
    w.entity(
        "Hero",
        (0.0, 0.0, 5.6), (0, math.pi, 0), (0.014, 0.014, 0.014),
        animated_model("../../assets/models/Fox/Fox.gltf", clip_index=1)
        + groom(GROOM_LONGCOAT, 60000, 1.0, (0.58, 0.44, 0.31))
        + groom_lod(card_px=220.0)
        + animal_budget(ROLE_HERO, motion_metres=0.06, deform=3, sim=4, vis=4, shadow=3),
        comment="── THE HERO: close-up, full coat, protected by the budget ──",
    )

    # ── Featured animals ────────────────────────────────────────────────
    for i in range(FEATURED_COUNT):
        angle = (i / FEATURED_COUNT) * math.tau
        x = 6.0 * math.cos(angle)
        z = -2.0 + 4.0 * math.sin(angle)
        w.entity(
            f"Featured {i}",
            (x, 0.0, z), (0, rng.range(0.0, math.tau), 0), (0.012, 0.012, 0.012),
            animated_model("../../assets/models/Fox/Fox.gltf", clip_index=1)
            + groom(GROOM_LONGCOAT, 24000, 1.0, (0.54, 0.41, 0.29))
            + groom_lod()
            + animal_budget(ROLE_FEATURED, motion_metres=rng.range(0.04, 0.09))
            + animal_path(
                radius_x=rng.range(3.0, 6.0), radius_z=rng.range(2.0, 4.5),
                rate_x=rng.range(0.18, 0.34), rate_z=rng.range(0.25, 0.47),
                phase_x=rng.range(0.0, math.tau), phase_z=rng.range(0.0, math.tau),
            ),
            comment=("── Featured animals: mid distance, long coats ──" if i == 0 else None),
        )

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
        w.entity(
            f"Herd {i}",
            (lateral, 0.0, depth), (0, rng.range(0.0, math.tau), 0), (0.011, 0.011, 0.011),
            animated_model("../../assets/models/Fox/Fox.gltf", clip_index=1)
            + groom(
                GROOM_LONGCOAT if long_coat else GROOM_SHORTCOAT,
                24000 if long_coat else 4000,
                1.0,
                (0.52, 0.40, 0.28) if long_coat else (0.62, 0.47, 0.33),
            )
            + groom_lod()
            + animal_budget(ROLE_BACKGROUND, motion_metres=rng.range(0.03, 0.10))
            + animal_path(
                radius_x=rng.range(4.0, 14.0), radius_z=rng.range(3.0, 10.0),
                rate_x=rng.range(0.14, 0.40), rate_z=rng.range(0.21, 0.58),
                phase_x=rng.range(0.0, math.tau), phase_z=rng.range(0.0, math.tau),
            ),
            comment=("── The herd: alternating short and long coats, receding ──" if i == 0 else None),
        )

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
