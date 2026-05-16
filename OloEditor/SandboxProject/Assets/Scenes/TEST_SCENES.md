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

## Suggested test order

1. **PBRReference** — lightest scene, validates basic PBR + IBL. Broken here → everything else broken.
2. **MaterialSpheres** — confirms metallic/roughness shader behaves across the parameter space.
3. **SponzaCSM** — main shadows + alpha-mask + IBL test on a large scene. Stress the camera.
4. **SponzaForwardPlus** — switch path, stress the light culling.
5. **SponzaDeferred** — switch path again, validate the G-Buffer matches.

---

## Notes

- Each scene saves its own post-process settings, so in-scene tuning persists across reloads.
- Scenes intentionally use Khronos sample assets (DamagedHelmet, SciFiHelmet, Suzanne, Sponza) so visual diffs against published reference renders are meaningful.
- HDR environment (Newport Loft) is a standard PBR validation HDRI.
