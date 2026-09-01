#!/usr/bin/env python3
# =============================================================================
# generate_material_lab_scene.py
#
# Emits the two Material-laboratory benchmark scenes (issue #974) into
# OloEditor/SandboxProject/Assets/Scenes/Benchmark/ as COMMITTED files:
#
#   MaterialLab.olo         neutral studio: a 10x10 roughness x metallic sphere
#                           sweep (ClosureV2), an 18% grey card, a strip of
#                           known linear colour patches, emissive references and
#                           normal-mapped reference spheres, under one white
#                           directional light + the Newport Loft HDRI.
#   MaterialLabFurnace.olo  the white-furnace variant: the same sweep grid with
#                           pure-white albedo inside a uniform white HDRI, no
#                           sun, no ground. An energy-conserving material must
#                           be indistinguishable from the background.
#
# Unlike generate_perf_scenes.py (git-ignored output), the output here is
# COMMITTED: the scenes are hand-designed reference content, the generator
# exists so the layout can be regenerated / re-parameterised deterministically.
# Byte-stable on purpose: no timestamps, no RNG, fixed entity-id namespace
# (974000001000000-974000001999999, per the #974 authoring contract), LF
# newlines. Re-running it must produce an identical file or the diff IS the
# change you made.
#
# Usage:
#   python OloEngine/tests/scripts/generate_material_lab_scene.py
#   python OloEngine/tests/scripts/generate_material_lab_scene.py --out-dir <dir>
#
# The YAML shape follows SceneSerializer.cpp's contract (each key verified
# against the deserializer or copied from a scene that demonstrably loads —
# MaterialSpheres.olo / DecalModeMatrixTest.olo / PBRClosureV2Test.olo).
# `Emissive`, `AlbedoMapPath`, `NormalMapPath` on MaterialComponent are the
# #974-added keys (this branch); scenes emitted by this script require them.
#
# The numerical-sanity test (MaterialLabSceneEvidenceTest.cpp) looks entities
# up BY TAG — the tag spellings below ("Gray18", "PatchRed", "Sweep_M1_R0",
# "Emissive_16x", ...) are load-bearing. Keep them in sync with the test.
# =============================================================================

import argparse
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_OUT_DIR = REPO_ROOT / "OloEditor" / "SandboxProject" / "Assets" / "Scenes" / "Benchmark"

# Entity-id namespace assigned to issue #974 scene content. Unique u64s within
# each scene; a fixed readable base keeps diffs stable across regenerations.
LAB_ID_BASE = 974_000_001_000_000
FURNACE_ID_BASE = 974_000_001_500_000

# 10x10 sweep: value = index / 9 so both axes reach the TRUE endpoints 0 and 1
# (metals only behave like metals at metallic = 1, and the furnace variant is
# only meaningful with the endpoints present). A strict "0.1 steps" ladder
# cannot cover 0..1 in 10 samples; index/9 is the same convention
# MaterialSpheres.olo uses for its 7-step metallic axis (0, 0.167, ..., 1).
GRID_N = 10
GRID_SPACING = 1.5
GRID_ORIGIN = -(GRID_N - 1) * GRID_SPACING / 2.0  # -6.75
SPHERE_Y = 0.5
SPHERE_SCALE = 0.55


def f(x):
    """Compact float formatting: 4 significant decimals, no trailing zeros."""
    s = f"{x:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def vec(v):
    return "[" + ", ".join(f(c) for c in v) + "]"


def sweep_value(index):
    return index / (GRID_N - 1)


def sweep_tag(metallic, roughness):
    return f"Sweep_M{f(metallic)}_R{f(roughness)}"


class SceneWriter:
    def __init__(self, scene_name, id_base):
        self.parts = [f"Scene: {scene_name}\n"]
        self.next_id = id_base
        self.count = 0

    def raw(self, text):
        self.parts.append(text)

    def entity(self, tag, translation=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1),
               components="", entity_id=None):
        eid = entity_id if entity_id is not None else self.next_id
        if entity_id is None:
            self.next_id += 1
        else:
            self.next_id = max(self.next_id, entity_id + 1)
        self.count += 1
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

def camera():
    # Key set mirrors SceneSerializer.cpp's CameraComponent reader, which reads
    # every one of these WITHOUT a default — all keys are required.
    return (
        "    CameraComponent:\n"
        "      Camera:\n"
        "        ProjectionType: 0\n"
        "        PerspectiveFOV: 0.785398185\n"
        "        PerspectiveNear: 0.05\n"
        "        PerspectiveFar: 300\n"
        "        OrthographicSize: 10\n"
        "        OrthographicNear: -1\n"
        "        OrthographicFar: 1\n"
        "      Primary: true\n"
        "      FixedAspectRatio: false\n"
    )


def dir_light():
    # Neutral WHITE studio key light — the 18%-grey neutrality check in
    # MaterialLabSceneEvidenceTest.cpp depends on the sun carrying no tint.
    return (
        "    DirectionalLightComponent:\n"
        "      Direction: [-0.35, -1, -0.25]\n"
        "      Color: [1, 1, 1]\n"
        "      Intensity: 3\n"
        "      CastShadows: true\n"
        "      ShadowBias: 0.005\n"
        "      ShadowNormalBias: 0.1\n"
        "      MaxShadowDistance: 60\n"
        "      CascadeSplitLambda: 0.95\n"
    )


def environment_map(file_path):
    return (
        "    EnvironmentMapComponent:\n"
        f"      FilePath: {file_path}\n"
        "      IsCubemapFolder: false\n"
        "      EnableSkybox: true\n"
        "      Rotation: 0\n"
        "      Exposure: 1\n"
        "      BlurAmount: 0\n"
        "      EnableIBL: true\n"
        "      IBLIntensity: 1\n"
        "      Tint: [1, 1, 1]\n"
    )


def mesh(primitive):
    # MeshPrimitive: 1 = Cube, 2 = Sphere, 3 = Plane (AnimatedMeshComponents.h)
    return f"    MeshComponent:\n      Primitive: {primitive}\n"


def material(albedo, metallic, roughness, emissive=None, albedo_map=None,
             normal_map=None):
    # AlbedoColor is LINEAR: it feeds Material::SetBaseColorFactor directly
    # (no sRGB decode) — see SceneSerializer.cpp's MaterialComponent reader.
    # PBRModel: 1 = ClosureV2 on every material in the lab (issue #975).
    out = (
        "    MaterialComponent:\n"
        f"      AlbedoColor: {vec(albedo)}\n"
        f"      Metallic: {f(metallic)}\n"
        f"      Roughness: {f(roughness)}\n"
    )
    if emissive is not None:
        out += f"      Emissive: {vec(emissive)}\n"
    if albedo_map is not None:
        out += f"      AlbedoMapPath: {albedo_map}\n"
    if normal_map is not None:
        out += f"      NormalMapPath: {normal_map}\n"
    out += "      PBRModel: 1\n"
    return out


def post_process(ssao_enabled):
    # Key set copied verbatim from DecalModeMatrixTest.olo (demonstrably
    # loads). TonemapOperator 0 = None: linear -> exposure -> gamma, which is
    # what makes the numerical patch predictions tractable. Bloom stays OFF in
    # both scenes so emissive spheres cannot bleed into their neighbours'
    # sample windows; FXAA off keeps patch edges crisp for window sampling.
    return (
        "PostProcessSettings:\n"
        "  TonemapOperator: 0\n"
        "  Exposure: 1\n"
        "  Gamma: 2.2\n"
        "  BloomEnabled: false\n"
        "  BloomThreshold: 1\n"
        "  BloomIntensity: 0.5\n"
        "  BloomIterations: 4\n"
        "  VignetteEnabled: false\n"
        "  VignetteIntensity: 0.5\n"
        "  VignetteSmoothness: 0.2\n"
        "  ChromaticAberrationEnabled: false\n"
        "  ChromaticAberrationIntensity: 0.5\n"
        "  FXAAEnabled: false\n"
        "  DOFEnabled: false\n"
        "  DOFFocusDistance: 10\n"
        "  DOFFocusRange: 5\n"
        "  DOFBokehRadius: 3\n"
        "  MotionBlurEnabled: false\n"
        "  MotionBlurStrength: 0.5\n"
        "  MotionBlurSamples: 4\n"
        "  ColorGradingEnabled: false\n"
        f"  SSAOEnabled: {'true' if ssao_enabled else 'false'}\n"
        "  SSAORadius: 0.4\n"
        "  SSAOBias: 0.025\n"
        "  SSAOIntensity: 0.7\n"
        "  SSAOSamples: 32\n"
        "  SSAODebugView: false\n"
    )


# --- scene builders -----------------------------------------------------------

LAB_HEADER = """\
# Material laboratory (issue #974) — GENERATED by
# OloEngine/tests/scripts/generate_material_lab_scene.py. Edit the generator,
# not this file; re-running it is byte-stable.
#
# A neutral studio for reading material response numerically:
#
#   * 10x10 sphere sweep centred on the origin (ClosureV2, PBRModel: 1).
#     Roughness runs along +X (columns, 0 at x=-6.75 .. 1 at x=+6.75),
#     metallic along +Z (rows, 0 at z=-6.75 far .. 1 at z=+6.75 near the
#     camera). Values are index/9 so both axes REACH 0 and 1; tags are
#     Sweep_M<m>_R<r> with the exact serialized value in the tag.
#   * Reference patch strip at z = 8.25 (thin cubes, tops at y = 0.08),
#     x -6.75 .. +6.75: PatchBlack [0.02], Gray18 [0.18] (the 18% grey card,
#     roughness 1, metallic 0), PatchGray50 [0.5], PatchWhite [1], PatchRed,
#     PatchGreen, PatchBlue. All values LINEAR — AlbedoColor feeds
#     SetBaseColorFactor directly, no sRGB decode.
#   * Emissive row at z = -9: Emissive_Ref (control, no Emissive key),
#     Emissive_1x [1,1,1], Emissive_Colored [4,2,0.5], Emissive_Green8
#     [0,8,0], Emissive_16x [16,16,16] — all on a dark dielectric base so
#     the emission term dominates the reading.
#   * Normal-map row at z = -11.25: rusted_iron / wall / gold albedo+normal
#     map pairs (editor-tree assets/textures/pbr/...).
#
# Lighting is deliberately NEUTRAL: one white sun (intensity 3, shadows on)
# + Newport Loft HDRI IBL. Tonemap None, bloom/FXAA off (numerical windows),
# SSAO on (the golden manifest captures the AOBuffer AOV).
#
# Numerical contracts: MaterialLabSceneEvidenceTest.cpp (looks entities up by
# the tags above). Capture manifests: OloEditor/assets/benchmark/manifests/
# material-lab.{golden,hero,exposure}.yaml.
"""

FURNACE_HEADER = """\
# Material laboratory — WHITE FURNACE variant (issue #974) — GENERATED by
# OloEngine/tests/scripts/generate_material_lab_scene.py. Edit the generator,
# not this file.
#
# The classic energy-conservation probe: the same 10x10 roughness x metallic
# sweep as MaterialLab.olo, but every sphere has pure-white albedo [1,1,1]
# and the ONLY light is a uniform white environment
# (assets/textures/hdr/furnace_white_1x1.hdr, committed on this branch).
# No sun, no ground plane, skybox visible, IBL on, tonemap None, all
# post effects off.
#
# In a white furnace an energy-conserving material is INDISTINGUISHABLE from
# the background at every roughness and metallic value: any sphere that reads
# darker than the sky is losing energy (single-scattering GGX at high
# roughness), any brighter is gaining it. The sweep makes the failure's
# roughness/metallic dependence directly visible.
#
# Capture manifest: OloEditor/assets/benchmark/manifests/
# material-lab-furnace.golden.yaml.
"""


def add_sweep_grid(w, id_base, albedo):
    for row in range(GRID_N):          # metallic, along +Z
        for col in range(GRID_N):      # roughness, along +X
            metallic = sweep_value(row)
            roughness = sweep_value(col)
            x = GRID_ORIGIN + col * GRID_SPACING
            z = GRID_ORIGIN + row * GRID_SPACING
            w.entity(sweep_tag(metallic, roughness),
                     (x, SPHERE_Y, z), (0, 0, 0),
                     (SPHERE_SCALE, SPHERE_SCALE, SPHERE_SCALE),
                     mesh(2) + material(albedo, metallic, roughness),
                     entity_id=id_base + 100 + row * GRID_N + col)


def build_material_lab():
    w = SceneWriter("MaterialLab.olo", LAB_ID_BASE)
    w.raw(LAB_HEADER)
    w.raw(post_process(ssao_enabled=True))
    w.raw("Entities:\n")

    w.raw("  # -- Camera: elevated at +Z, looking down over the whole lab --\n")
    w.entity("Camera", (0, 9, 16), (-0.55, 0, 0), (1, 1, 1), camera(),
             entity_id=LAB_ID_BASE + 1)
    w.raw("  # -- Skybox + IBL (Newport Loft equirect HDR, ~neutral studio) --\n")
    w.entity("Skybox", (0, 0, 0), (0, 0, 0), (1, 1, 1),
             environment_map("assets/textures/hdr/newport_loft.hdr"),
             entity_id=LAB_ID_BASE + 2)
    w.raw("  # -- White studio key light (NO tint: Gray18 neutrality depends on it) --\n")
    w.entity("Sun", (0, 0, 0), (0, 0, 0), (1, 1, 1), dir_light(),
             entity_id=LAB_ID_BASE + 3)
    w.raw("  # -- Ground plane (neutral mid-dark; see single-mesh-visual-test-lighting.md) --\n")
    w.entity("Ground", (0, -0.5, 0), (0, 0, 0), (40, 1, 40),
             mesh(3) + material([0.3, 0.3, 0.32], 0.0, 0.9),
             entity_id=LAB_ID_BASE + 4)

    w.raw("  # -- 10x10 sweep: roughness along +X, metallic along +Z (ClosureV2) --\n")
    add_sweep_grid(w, LAB_ID_BASE, albedo=[0.85, 0.85, 0.85])

    w.raw("  # -- Reference patch strip (thin cubes, tops at y = 0.08) --\n")
    patches = [
        ("PatchBlack", [0.02, 0.02, 0.02], 0.95),
        ("Gray18", [0.18, 0.18, 0.18], 1.0),
        ("PatchGray50", [0.5, 0.5, 0.5], 0.95),
        ("PatchWhite", [1, 1, 1], 0.95),
        ("PatchRed", [1, 0, 0], 0.95),
        ("PatchGreen", [0, 1, 0], 0.95),
        ("PatchBlue", [0, 0, 1], 0.95),
    ]
    for i, (tag, albedo, roughness) in enumerate(patches):
        x = -6.75 + i * 2.25
        w.entity(tag, (x, 0.04, 8.25), (0, 0, 0), (1.8, 0.08, 1.8),
                 mesh(1) + material(albedo, 0.0, roughness),
                 entity_id=LAB_ID_BASE + 200 + i)

    w.raw("  # -- Emissive row (dark dielectric base; Emissive_Ref = control) --\n")
    emissives = [
        ("Emissive_Ref", None),
        ("Emissive_1x", [1, 1, 1]),
        ("Emissive_Colored", [4, 2, 0.5]),
        ("Emissive_Green8", [0, 8, 0]),
        ("Emissive_16x", [16, 16, 16]),
    ]
    for i, (tag, emissive) in enumerate(emissives):
        x = -6 + i * 3
        w.entity(tag, (x, 0.5, -9), (0, 0, 0), (0.7, 0.7, 0.7),
                 mesh(2) + material([0.1, 0.1, 0.1], 0.0, 0.6, emissive=emissive),
                 entity_id=LAB_ID_BASE + 300 + i)

    w.raw("  # -- Normal-mapped reference spheres (editor-tree pbr texture sets) --\n")
    texture_sets = [
        ("NormalMap_RustedIron", "rusted_iron", 0.9, 0.4),
        ("NormalMap_Wall", "wall", 0.0, 0.9),
        ("NormalMap_Gold", "gold", 1.0, 0.3),
    ]
    for i, (tag, folder, metallic, roughness) in enumerate(texture_sets):
        x = -3 + i * 3
        w.entity(tag, (x, 0.5, -11.25), (0, 0, 0), (0.8, 0.8, 0.8),
                 mesh(2) + material([1, 1, 1], metallic, roughness,
                                    albedo_map=f"assets/textures/pbr/{folder}/albedo.png",
                                    normal_map=f"assets/textures/pbr/{folder}/normal.png"),
                 entity_id=LAB_ID_BASE + 400 + i)

    return w


def build_furnace():
    w = SceneWriter("MaterialLabFurnace.olo", FURNACE_ID_BASE)
    w.raw(FURNACE_HEADER)
    w.raw(post_process(ssao_enabled=False))
    w.raw("Entities:\n")

    w.raw("  # -- Camera: same pose as MaterialLab.olo --\n")
    w.entity("Camera", (0, 9, 16), (-0.55, 0, 0), (1, 1, 1), camera(),
             entity_id=FURNACE_ID_BASE + 1)
    w.raw("  # -- Uniform white furnace environment (skybox visible + IBL) --\n")
    w.entity("Furnace", (0, 0, 0), (0, 0, 0), (1, 1, 1),
             environment_map("assets/textures/hdr/furnace_white_1x1.hdr"),
             entity_id=FURNACE_ID_BASE + 2)

    w.raw("  # -- 10x10 sweep, pure white albedo: read energy loss/gain per cell --\n")
    add_sweep_grid(w, FURNACE_ID_BASE, albedo=[1, 1, 1])

    return w


def main():
    ap = argparse.ArgumentParser(
        description="Generate the Material-laboratory benchmark scenes (.olo, issue #974)")
    ap.add_argument("--out-dir", type=pathlib.Path, default=DEFAULT_OUT_DIR)
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for builder in (build_material_lab, build_furnace):
        w = builder()
        name = w.parts[0].split()[1].strip()  # "MaterialLab.olo" from "Scene: ..."
        out = args.out_dir / name
        out.write_text(w.text(), encoding="utf-8", newline="\n")
        print(f"{name:24s} entities={w.count:<4d} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
