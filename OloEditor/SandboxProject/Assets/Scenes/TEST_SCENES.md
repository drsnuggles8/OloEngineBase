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

### [FSR2UpscaleTest.olo](FSR2UpscaleTest.olo)

**Purpose**: Verify FSR2 temporal upscaling (#684) — and, just as importantly, verify it the only way
that works. A temporal upscaler's failure modes (ghosting, disocclusion trails, an inverted
motion-vector sign, a jitter scaled against the wrong extent) all produce a **still frame that looks
correct**, so a screenshot is not evidence here. You have to move the camera.
**Contents**: SponzaDeferred's geometry and lighting — dense thin detail (drapes, railings, pillar
filigree) is what a reconstruction either recovers or smears — with a PostProcessSettings block set
to `Upscale: 1` (Quality, 0.667x render scale) and `UpscaleTechnique: 1` (Temporal / FSR2). FXAA is
off so nothing blurs the result being judged; `TAAEnabled` is left **true** on purpose, because FSR2
suppresses engine TAA while it runs and this scene should exercise that.
**Pass** — open with renderer set to **Deferred**, then **fly the camera along the nave and stop**:
- The frame is at least as sharp as native while the scene renders at 67% — thin geometry resolves,
  it does not shimmer or crawl.
- Nothing trails. Pillar edges, the drape fringes and the helmet silhouette leave no smear behind
  them during the move, and none remains after you stop.
- Stopping converges: within roughly half a second the image settles and stays settled.
- `OloEngine.log` says FSR2 is in use. If it instead reports falling back to the spatial upscaler,
  that is the honest answer for this build/backend (non-Windows, Vulkan, `OLO_WITH_FSR2=OFF`, or MSAA
  on) — not a scene bug.
- A/B: set `UpscaleTechnique: 0` for FSR1 at the same render scale, or `Upscale: 0` for native.
**Fail**: trails behind moving silhouettes (reprojection sign or history invalidation); the image is
uniformly soft and does NOT sharpen as you hold still (the jitter is not reconstructing — suspect the
render-vs-display extent); ringing on high-contrast edges (sharpening applied twice); the frame
darkens or brightens relative to native (exposure); a black frame (the upscaler produced no output
while the post chain still routed through it).

---

### [DecalModeMatrixTest.olo](DecalModeMatrixTest.olo)

**Purpose**: The decal **mode matrix** acceptance scene (issue #853) — one decal per `DecalMode` on a single uniform floor, so each mode's G-Buffer *channel* routing is observable against a known neighbour. Before this scene existed, no sandbox scene put a decal on the deferred path at all, which is a large part of why #853 stayed invisible.
**Contents**: One flat grey plane (albedo 0.55, metallic 0, roughness 0.85), a single directional light, no skybox/IBL (an environment reflection would mask a roughness/metallic leak), and four opaque decals in a row: Albedo at x=−6, Normal at x=−2, RMA at x=+2, Emissive at x=+6.
**Pass** — open with renderer set to **Deferred** (`olo_renderer_settings_set`, or the renderer panel; on Forward/Forward+ every mode collapses to the transparent albedo overlay and nothing here is exercised):
- **Albedo** patch: a red/white checker tints the floor; the patch stays **matte**, exactly as rough and as non-metallic as the floor around it. RT0.a (metallic) is the mode's masked-out channel.
- **Normal** patch: surface relief appears (the lighting response gains bumps); the patch's **gloss is unchanged** from the surrounding floor. RT1.zw (roughness, AO) are masked out.
- **RMA** patch: the floor's **albedo is unchanged** — same grey, same checker-free surface — while its roughness/metallic response changes. RT0.rgb is masked out.
- **Emissive** patch: a cyan checker glows additively over the lit floor and the surface stays lit (not flagged unlit). RT2.a is masked out.
**Fail** — each of these is the corresponding channel mask being lost before the draw (#853's exact signature; probe the G-Buffer with `olo_render_capture_target` / `olo_render_probe_pixel` rather than trusting the composite):
- RMA patch turns **black** — `Decal_GBuffer_RMA.glsl` writes `gAlbedo = vec4(0, 0, 0, metallic)`, so an unmasked RT0.rgb paints the floor black. This is the loudest arm; read it first.
- Albedo patch turns **mirror-metallic** on a matte floor — the decal's alpha reached RT0.a.
- Normal patch turns **glossy** — the Normal shader's `vec4(oct, 0.0, 1.0)` reached RT1.zw (roughness 0, AO 1).
- (The Emissive arm's mask leak is *not* observable here and is not evidence: RT2 blends One/One, so `dst.a + 0` leaves the unlit flag alone either way.)

---


### [LightmapTest.olo](LightmapTest.olo)

**Purpose**: The baked-GI acceptance scene (issue #439) — static geometry lit by a baked lightmap, a dynamic object lit by baked probes, and the staleness gate demonstrable in one room.
**Contents**: A matte corridor (grey floor/ceiling/back wall, RED wall at −X, GREEN wall at +X, an orange pillar occluder), all marked **Lightmap Static**; a dynamic sphere (not static) covered by a Baked `LightProbeVolume`; two white point lights.
**Pass** — on the **Forward** path:
- Before baking: the room shows direct lighting only (dark ceiling, black shadowed pillar side, no colour bleed).
- **Build > Bake Lightmaps**, wait for the log's "Lightmap bake complete", then: the floor beside the red wall picks up a visible red indirect tint, beside the green wall a green one; the ceiling brightens; the pillar's shadowed side is no longer black.
- **Bake Light Probes (Path Traced)** on the Probe Volume entity: the dynamic sphere picks up ambient consistent with its surroundings.
- Move any static entity → the log warns "bake key mismatch … baked GI disabled" and the room falls back to direct-only rendering (no stale lightmap is ever sampled). Re-bake restores it.
- Save the scene after baking so `LightmapSettings.LightmapAsset` persists.
**Fail**: no visual change after a bake (check the log for unwrap/bake errors); colour bleed on the WRONG sides (a UV/region mapping regression); the lightmap still rendering after moving a static entity (the staleness gate is broken — the worst failure this feature can have).

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

### [VehiclesTest.olo](VehiclesTest.olo)

**Purpose**: Validate all three vehicle families from issue #438 in one shot —
the FWD/RWD/AWD differential modes on the wheeled `VehicleComponent`,
`BoatComponent` propulsion + rudder on top of buoyancy, and the
`AircraftComponent` flight model.
**Contents**: A 600 m sea (Gerstner, no FFT) with a seafloor, an 800 m concrete
quay whose deck sits 3 m above the water, three otherwise-identical cars
differing only in `DriveMode` (red = rear, blue = front, green = all), two
floating boats (white runs straight, orange holds right rudder), and one
aircraft that **takes off from the quay on its landing gear**. Every driver
input is **authored in the scene**, so the whole thing animates on
Play/Simulate with no scripting.
**Important**: press **Play** or **Simulate** — nothing moves in edit mode.
**Camera**: the scene camera has `RuntimeControl` on, so in **Play** you can fly
around with **WASD** (Q/E for down/up) while holding the **right mouse button**
to look. `FlySpeed` is 45 m/s — the quay alone is 800 m long.
**Pass**:
- All three cars pull away down the quay and stay on their wheels (chassis
  around y = 4.1, i.e. held above the 3 m deck by the suspension). They separate
  over distance — different driven axles, different traction — but none of them
  may sit still; that means a differential wired to the wrong axle. They
  eventually run off the far end into the sea, which is fine.
- Both boats float with the hull **half-immersed** — deck clear of the water,
  origin at the waterline — and drive forward. The orange one carves a steady
  circle; it must **turn**, not slide broadside (broadside = the hull's lateral
  drag isn't biting).
- The aircraft rides on its gear with the **belly clear of the deck**, rolls,
  rotates under back-pressure, unsticks around 33 m/s and climbs away over the
  sea — wings level throughout (nothing applies a roll input, so any roll is a
  bug).
**Fail**: a car that never moves; a boat that pivots on the spot while
stationary (rudder authority not speed-scaled), planes into the sky, or is
driven under; an aircraft that never rotates (it will run off the end of the
quay), leaves the ground without rolling forward, porpoises, or rolls/yaws;
any NaN pose. Check `OloEngine.log` for physics warnings.

**On the lighting.** The scene uses the same cubemap `EnvironmentMapComponent` as
`WaterTest.olo` rather than a procedural sky. That is not decoration: water is
largely a mirror, so with only a flat procedural gradient to reflect it falls back
to its dark base colour and reads as a huge shadow over the whole scene. If the sea
ever looks like a black void again, check that the environment map is present and
loading before suspecting the shadow cascades — disabling `CastShadows` entirely
changes nothing here, which is the quickest way to rule shadows out.

*Known, pre-existing:* the sea still renders dark when viewed steeply from above,
and `PlanarReflectionRenderPass` emits a per-frame `[trace]` GLStateGuard message
about depth state escaping the pass. Both reproduce in `WaterTest.olo` on an
unmodified tree, so they are water/renderer issues rather than anything this scene
introduced.

**On the aircraft's landing gear.** `m_HasLandingGear` puts the aircraft on
three sprung ray-cast legs instead of its fuselage collider. That is what makes
the takeoff rotation possible at all: a box resting on the ground pivots about
its **rear edge**, where the weight moment (≈ 9810 N × 3 m ≈ 29 kN·m) is an
order of magnitude past what the elevator (≈ 3.5 kN·m) can beat, whereas the
main gear sits just aft of the centre of mass and cuts that arm to centimetres.
Turn `HasLandingGear` off in the inspector and you can watch the aircraft
accelerate the length of the quay without ever lifting its nose — the direct
before/after, and what `AircraftGearTest` pins as a pair.

---

## Terrain / Water

### [Drift.olo](Drift.olo)

**Purpose**: The *Drift* game scene, and — since issue #880 — the only scene in
the project that puts a procedural terrain tile in an ocean. It is also the only
gameplay scene that turns the **GPU LOD quadtree** on (`TessellationEnabled`) and
the only scene of any kind that turns **foliage impostor cards** on
(`UseImpostor`); before #880 both flags appeared solely in the tests written with
them, which is test coverage and not product coverage.
**Contents**: six procedurally generated islands scattered across a 1.6 km sea,
each with its own seed / shaping / height scale / material palette / vegetation,
plus the sailing boat, the weather director and the time-of-day clock. Press
**Play** and sail (W/S throttle, A/D rudder; the wind does the work). The scene's
own header comment carries the island table and the coordinates.
**Pass**:
- **The shoreline is a beach, not a cut.** Sail up to any island and look at where
  the land meets the water from three heights: from above, from deck level, and
  with the camera below the surface. The ground must shelve away under the water
  — there must be no vertical face of terrain standing in the sea, and none at the
  square edge of a tile in particular. That edge is where the defect #880 fixed
  used to live (measured: 64.9% of the original island's tile border stood above
  sea level).
- **No LOD pop or crack on the approach.** Sail at an island from ~600 m in. The
  relief resolves progressively; no triangle-sized seam ever opens between two
  patches, and no visible step where a level changes.
- **The six read as six.** From the start line the islands at different bearings
  must be tellable apart by silhouette alone: Stacks is tall/sheer/bare, Dunes is
  smooth and rounded, Mesa is a stack of flat pale ledges, Atoll is low and wide.
- Tree lines stay legible at range (the impostor cards), and do not pop as a block
  at the 30 m cross-fade distance.
**Fail**: a vertical wall of ground at the waterline, or a hard horizontal line
where the seafloor plane cuts an island's underwater flank; cracks between terrain
patches while approaching; islands that read as one heightfield at different
positions; trees that vanish or flatten into cards facing the wrong way.

**Related**: `DriftIslandFieldEvidenceTest` captures the same poses headlessly
(`assets/tests/visual/DriftIsland_*.png`) and asserts the tile-border contract on
a rendered frame. The CPU side is in `TerrainGeneratorTest`. Background:
[terrain-tile-meets-ocean.md](../../../../docs/agent-rules/terrain-tile-meets-ocean.md).

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
