# Test Scenes

Reference scenes used for regression-testing the renderer. Each scene is a
narrow, repeatable check of one or two subsystems — open it, look for the
described visual outcome, file a bug if something doesn't match.

When adding a new test scene, append it here under the most relevant
category. Keep entries short: purpose, contents, what to look for, what
counts as failure.

---

## Rendering / PBR / Lighting

### [PBRReference.olo](PBRReference.olo)

**Purpose**: Validate the PBR pipeline against the Khronos reference models.
**Contents**: DamagedHelmet (left), SciFiHelmet (right), Suzanne (back), lit
by the Newport Loft HDR environment + a soft fill directional. Camera at
`[0, 1, 4]`.
**Pass**:
- DamagedHelmet matches the [Khronos reference](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet) — visor reflects environment, gold accents, scratched paint, exposed circuitry.
- SciFiHelmet shows clean white/grey panels with PBR sheen.
- Suzanne is smoothly lit gray-cream — *intentionally monochrome* per the source asset, this is a normals/tangents smoke test, not a color test.
- Newport Loft window/brick visible in the skybox.
**Fail**: any model rendering flat-black (IBL not propagating); wrong metalness on the helmet visor; UV-island seams.

### [SponzaCSM.olo](SponzaCSM.olo)

**Purpose**: Cascaded shadow maps + directional lighting on a large interior. The canonical CSM benchmark.
**Contents**: Sponza atrium + sun (intensity 14, CSM on, dir `[-0.55, -0.7, -0.45]`) + dim IBL (0.25) + DamagedHelmet at center. Camera at the entrance looking down the long axis.
**Pass**:
- Crisp shadows from columns, arches, banners, and the helmet — visible on the floor and on adjacent walls.
- Sun-facing side of each column is bright; opposite side is in shadow.
- **CSM stability**: right-click + drag to rotate the camera, then WASD around. A column's shadow on the floor must stay **locked to the world** — no drift on rotation.
- Lion fountain visible at the far end (was previously occluded by alpha-masked decorations).
**Fail**: shadows only from a few small objects (column self-shadowing missing); shadows that drift as you rotate the camera; banners/foliage rendered with stone textures.

### [SponzaForwardPlus.olo](SponzaForwardPlus.olo)

**Purpose**: Stress test for Forward+ tiled light culling.
**Contents**: Sponza + **120 colored point lights** in a 6×4×5 grid throughout the atrium (12-hue cycle, intensities 4-6, ranges 7-9). Dim ambient sun.
**Pass** — open with renderer set to **Forward+**:
- Overlapping colored pools of light blanket the atrium.
- ≥60 FPS on a modern desktop GPU at editor resolution.
- Walking around with WASD shows smooth lighting; no tile-boundary squares; no abrupt brightness jumps.
**Fail**: per-tile light count overflow (visible squares where lights "snap" on/off); missing pools (culling false-negative).

### [MaterialSpheres.olo](MaterialSpheres.olo)

**Purpose**: Visual reference for PBR metallic/roughness sweep. Sanity check after shader changes.
**Contents**: 7×7 grid of spheres — 7 columns each in a different albedo color (red, orange, yellow, green, teal, blue, magenta), each column sweeping metallic (Z axis) × roughness (X axis). Plus 4 named "ref" spheres at the back (Gold, Rusted Iron, Plastic, Wall). Strong sun + Newport Loft IBL.
**Pass**:
- Each column: front-row spheres show sharp specular highlights (low roughness); back-row are matte (high roughness).
- Bottom rows (metallic=1.0) clearly reflect the colored environment; top rows (metallic=0.0) show diffuse-lit colored albedo.
- Smooth gradient in both axes — no banding or sudden jumps between adjacent spheres.
- 4 named ref spheres look believably like their material.
**Fail**: spheres all uniform color (PBR shader broken); banding between adjacent metallic/roughness levels; reference spheres look wrong (e.g. "Gold" not gold).

### [SponzaDeferred.olo](SponzaDeferred.olo)

**Purpose**: Validate the deferred G-Buffer pipeline on the same Sponza geometry. Pair with SponzaCSM.olo, but expect intentional lighting differences (see below) — this is *not* a pixel-for-pixel A/B.
**Contents**: Same Sponza geometry as SponzaCSM.olo, with these deliberate scene-level differences:
- Sun direction `[-0.4, -0.85, -0.3]` (more overhead) vs SponzaCSM's `[-0.55, -0.7, -0.45]` (more side-cast).
- Sun intensity `6` vs SponzaCSM's `14` — overall scene is noticeably dimmer.
- Full-strength IBL (SponzaCSM dims IBL to keep the sun dominant).
- 4 warm/cool accent point lights at floor level (exercise tiled deferred light culling).
**Pass** — open with renderer set to **Deferred**:
- Sponza renders correctly through the deferred path (geometry, materials, normals, AO all visible).
- Shadow cascades work through deferred (column/arch shadows visible on floor and walls).
- Sun shadows fall at the steeper angle implied by the more-overhead direction — shorter horizontal shadows than in SponzaCSM.
- Overall scene is dimmer than SponzaCSM (intensity 6 vs 14) but mid-tones are more visible (IBL not dimmed).
- 4 accent point lights produce localized warm/cool color pools on the floor.
- DamagedHelmet emissive parts glow.
**Fail**: G-Buffer encoding artifacts (banded normals, blocky roughness); missing emissive on the helmet; shadows missing on deferred path while working on forward; lighting matches SponzaCSM exactly (means the scene file wasn't loaded with its own sun/IBL values); accent point lights missing (deferred light culling regression).

---

## Animation / Procedural

### [AnimationNoiseTest.olo](AnimationNoiseTest.olo)

**Purpose**: Validate the procedural **noise animator** (issue #107) — smooth
fractal-noise-driven *additive* offsets layered on top of keyframe animation
for organic idle motion (breathing, idle sway, gentle wind).
**Contents**: Two CesiumMan characters on a grey floor, both playing their
default clip:
- **Left — "Idle Breathing"**: subtle `NoiseAnimationComponent` on the head
  chain (`EndBoneIndex 4`, `ChainLength 3`), low frequency (0.5), small
  amplitude (`[0.05, 0.03, 0.08]` rad), `Seed 1`.
- **Right — "Wind Sway"**: stronger sway — same chain but `ChainLength 4`,
  frequency 1.2, amplitude `[0.16, 0.08, 0.2]` rad, `Seed 77`.

**Important**: the noise runs only in **Play mode** and only while the entity
is actively playing an animation clip (same gate as the spring-bone pass) —
press Play to see it.
**Pass**:
- In Play mode, both characters' upper body / head **sway smoothly and
  continuously** — no jitter, no per-frame popping, no runaway drift.
- The two characters move **independently** (different Seed) — they never
  wobble in lock-step.
- The left character's motion is clearly subtler than the right's.
- Toggling a character's `NoiseAnimationComponent` → **Enabled** off freezes
  its sway back to the plain clip; toggling **Weight** to 0 does the same.
- `OloEngine.log` shows no animation/skeleton errors.
**Fail**: jitter or discontinuous popping (noise not smooth); the head/body
drifting away and not returning (offset not bounded by amplitude); both
characters moving identically (seed de-correlation broken); the sway
persisting in Edit mode or with the component disabled.

---

## Physics / Cloth

### [ClothTest.olo](ClothTest.olo)

**Purpose**: Validate the base Jolt soft-body cloth pipeline (issue #460, first
slice) — gravity draping and soft-vs-rigid collision, no wind.
**Contents**: A ground plane and two cloths at `[±2.5, 5, 0]` — "Hanging Cloth"
(`Attachment: TopEdge`, pinned along its top row) and "Falling Cloth"
(`Attachment: None`, free-falls onto the ground).
**Important**: cloth only renders in **Play mode** (its deforming mesh is
built at runtime start) — press Play to see it.
**Pass**: the hanging cloth settles into a draped curtain, pinned edge held
near its spawn height; the falling cloth drops and rests flat on the ground
without tunnelling through it.
**Fail**: either cloth passes through the ground; the hanging cloth's pinned
edge drifts far from its spawn height; NaN/degenerate geometry.

### [ClothWindTest.olo](ClothWindTest.olo)

**Purpose**: Validate wind coupling on cloth soft bodies (issue #460,
wind-coupling slice) — `WindSystem`'s analytical wind field applied as a
force via `ClothWindSystem` / `JoltScene::ApplyClothWindForce`.
**Contents**: A ground plane, a strong steady `WindSettings` field
(`Direction [1, 0, 0.2]`, `Speed 25`, gust on), and two `TopEdge`-pinned
cloths side by side: orange **"Wind Cloth"** (`WindInfluence: 1`, full
response) and blue **"No-Wind Control Cloth"** (`WindInfluence: 0`, ignores
the field entirely) — the pairing makes the wind's effect legible by direct
A/B rather than trusting a single cloth's motion in isolation.
**Important**: cloth only renders in **Play mode** — press Play to see it.
Let it run a few seconds so both cloths finish their initial gravity-driven
fold from a flat spawn pose before judging the wind response.
**Pass**: the orange cloth visibly billows/twists sideways in the wind
direction; the blue control hangs as a flat, undisturbed rectangle from its
pinned edge, unaffected by the same wind field.
**Fail**: both cloths behave identically (wind not applied, or
`WindInfluence` not respected — check `Scene::Copy()` carried `WindSettings`
into the Play-mode scene copy); the orange cloth doesn't move at all; NaN/
degenerate geometry.

---

## Suggested test order

1. **PBRReference** — lightest scene, validates basic PBR + IBL. Broken here → everything else broken.
2. **MaterialSpheres** — confirms metallic/roughness shader behaves across the parameter space.
3. **SponzaCSM** — main shadows + alpha-mask + IBL test on a large scene. Stress the camera.
4. **SponzaForwardPlus** — switch path, stress the light culling.
5. **SponzaDeferred** — switch path again, validate the G-Buffer matches.
6. **AnimationNoiseTest** — procedural animation; press **Play** and confirm the two characters sway smoothly and independently.
7. **ClothTest** — soft-body cloth under gravity; press **Play** and confirm the hanging cloth drapes and the falling cloth lands on the ground.
8. **ClothWindTest** — cloth + wind; press **Play**, wait a few seconds, and confirm the wind cloth billows while the no-wind control stays flat.

---

## Notes

- Each scene saves its own post-process settings, so in-scene tuning persists across reloads.
- Scenes intentionally use Khronos sample assets (DamagedHelmet, SciFiHelmet, Suzanne, Sponza) so visual diffs against published reference renders are meaningful.
- HDR environment (Newport Loft) is a standard PBR validation HDRI.
