# The reference path tracer owns its sampling model; the parity fixtures own the shared subset

Issue #869. The lightmap and probe bakes use the CPU reference path tracer as their kernel, so they
inherited its two fidelity limits: materials were factor-only (a photo-textured wall bounced its
base-colour *factor*) and the environment was a single uniform radiance (an exterior baked with no
sky contribution at all). #869 asked which of three things to do about that, and said the decision
had to be made before any code, because one of the options quietly changes what every existing
parity test means.

**The decision is the issue's option 2 — split the roles — stated as an invariant rather than as a
second scene type.**

---

## 1. The rule

> The reference scene's **description** may be as rich as the bake needs. Its **sampling model** is
> its own, defined here and pinned by tests that never involve the raster path. It is never
> inherited from, nor calibrated against, the raster's samplers.

Two corollaries, and they are what make the rule usable:

- **A parity fixture stays inside the subset both worlds express.** `DDGIReferenceParityTest`,
  `LightmapBakeParityTest` and `LightProbePathTracedBakeTest` build both worlds from one
  description that uses neither a texture nor a sky. They therefore pin exactly what they pinned
  before this ADR, unchanged, and they do so *by construction* — not by a promise to remember.
- **The richer description is opt-in at the call site, not a mode.** There is one `ReferenceScene`
  type. What separates "the parity population" from "the bake population" is whether the caller
  supplied maps and a sky, and the default is not to.

## 2. Why the tension in the issue dissolves

#869's worry: teaching the oracle to sample textures or a cubemap makes it share a failure mode
with the thing it validates, so every parity comparison gets weaker.

That worry is correct about *conventions* and wrong about *content*, and the two separate cleanly:

- **A surface's albedo image is content.** It is a property of the scene, not of a renderer. Both
  worlds reading one image is the same discipline `reference-path-tracer.md` §4 already demands
  ("build both worlds from ONE description"); a hand-mirrored albedo is exactly the fictitious
  divergence that section is about.
- **How that image is filtered is a convention**, and this is where the two must differ. The raster
  path samples through a mip chain with anisotropy and whatever wrap state the material set. The
  reference samples **level 0, bilinear, REPEAT, sRGB decoded before filtering** — and that is not a
  compromise, it is the *correct* choice for an integrator. A path tracer's hit points are already
  distributed over the surface, so level-0 sampling converges to the footprint-averaged albedo; a
  mip lookup would pre-average it a second time. **Mipping is the approximation here, and the
  reference declines it.** A filtering bug in the raster path therefore still shows up as
  raster-vs-reference disagreement, which is the property the oracle exists to have.

The environment separates even more sharply. The raster path's diffuse ambient is a *prefiltered
approximation* — a 32² irradiance cubemap, or an SH projection of it. The reference integrates the
raw sky by tracing. The two are not doing the same thing at all, so there is no shared failure mode
to acquire: the reference computes the quantity the prefilter is trying to approximate.

## 3. Sky had a second blocker, and it does not hold

#869: *"with no directional environment model there is no ground truth to validate an HDRI bake
against — you couldn't tell whether the result was right."*

True if the only candidate ground truth is the engine's own IBL. It is not. A directional
environment admits three independent checks, none of which involves the raster path:

1. **Reduction.** A cubemap of constant radiance must reproduce the uniform-environment white
   furnace. That is a strong regression pin: the entire existing furnace suite becomes a special
   case of the new code path.
2. **Quadrature.** Unoccluded irradiance `E(n) = ∫ L(ω) max(0, n·ω) dω` is computed by a dense
   deterministic quadrature over the same `Evaluate`, with no BVH, no sampling and no MIS. The
   traced estimate must land on it. The two share only the environment lookup, so this pins the
   *transport*.
3. **Face selection.** A cubemap with a different constant per face pins which face a direction
   reads, against the standard GL convention the skybox shader uses (`GetSkyboxSampleDirection` is
   the identity, so the engine samples with the raw world direction).

That is ground truth in the sense `reference-path-tracer.md` demands — an answer with a definition
independent of any renderer — so the sky half is not merely unverifiable-but-shipped.

**What the reference deliberately does not model** is a bright *sun* inside the environment image.
The environment is collected only when a ray escapes; it is never next-event-estimated. That is
fine for a sky, which is broad and smooth, and terrible for a small bright disc. It costs nothing
here because this engine authors the sun as a `DirectionalLightComponent` and the cubemap as the
ambient source — the same decomposition the raster path uses. A future environment containing a
concentrated emitter needs environment NEE and MIS before it can be trusted, and
`reference-path-tracer.md` §6 says so.

## 4. Why not the other two options

**Option 1 — grow the oracle by adopting the raster's sampling conventions.** This is the one that
would have weakened every parity suite, and nothing needs it. Adopting the mip chain buys a worse
integrator and a shared failure mode in exchange for nothing.

**Option 3 — leave both as-is and bound the gap.** Rejected because for albedo the gap is not a
small correction and this repo's own importer is what makes that concrete. `Model::ProcessMaterial`
initialises the base colour to **white** and only overwrites it if the source material carries an
`AI_MATKEY_COLOR_DIFFUSE` — and when a texture override is in play it forces white explicitly, with
the comment *"set base color to white so texture colors come through properly"*. A glTF material
whose colour lives in its base-colour texture, which is the ordinary case, therefore has a white
factor, and a factor-only bake bounces **white** off it regardless of what the surface looks like.
"Bake with an area-averaged albedo per material" is then not a correction to a small error; it is a
second, different approximation replacing a total one. `LightmapSkyAndTextureBakeTest` measures the
divergence directly on a fixture built around exactly that white-factor case.

For sky the gap is the entire ambient term of an exterior bake. Documenting a bound of "up to all of
it" is not a bound.

Option 3 also does not get cheaper by waiting. Half of option 2's material work had already landed
before this issue was picked up: `ReferenceMaterial` grew albedo/metallic-roughness/normal/emissive
maps and `ReferenceTexture` grew its pinned sampler for the GPU path tracer's device-parity test
(#1055, #1112). What was missing was not the model but the *population* — the ECS adapter never
filled those fields in, so the bake stayed factor-only while the machinery to do better sat
unused. Confirming that precedent and finishing it is cheaper than writing down a bound.

## 5. What this ADR obliges

- **`ReferenceScene`'s sampling conventions are pinned, and a change to one is a change to this
  ADR.** Textures: level 0, bilinear, REPEAT, sRGB decode before the filter
  (`GpuPathTracerContract.ReferenceTextureSamplesLikeTheMaterialSampler`). Environment: level 0,
  standard GL face selection, bilinear within a face, clamped at face edges — seam filtering is a
  sub-texel effect that cannot move a hemisphere integral (`ReferenceEnvironmentTest`).
- **A new consumer that builds a `ReferenceScene` for a comparison must say which population it is
  in.** If it compares against a raster path, it stays in the shared subset. If it is a bake, it may
  take the richer one. `ReferenceSceneBuildOptions` makes that an explicit argument rather than a
  default, so the choice appears at the call site.
- **A second tracer reading a `ReferenceScene` must not silently ignore the richer fields.** The GPU
  device-parity test asserts its fixture carries no environment cubemap rather than quietly tracing
  a different world — the failure mode a silent fallback would create is a parity test that passes
  while measuring two different scenes.

## 6. Consequences

- The bake's bounce colour now tracks a surface's albedo texture, and an exterior bake picks up sky
  bounce. Both were previously "less bounce than reality, never wrong bounce"; they are now neither.
- The reference gained a GPU dependency **at capture time only**. `ReferenceScene`,
  `ReferenceSceneBuilder` and the integrator stay GL-free: the builder takes an injectable texture
  provider and a pre-captured cubemap, so every headless test supplies synthetic images and the
  editor supplies a readback. A null provider reproduces the pre-#869 factor-only world exactly.
- The units ledger in `baked-lightmap-pipeline.md` §3 is unchanged. Sky enters the lightmap the same
  way an emissive surface does — through a path vertex, never through a delta light — so the atlas
  still stores indirect-only irradiance E by construction.
- **The ambient ladder's replace-don't-add rule matters more now.** A lightmapped exterior pixel
  whose bake already contains the sky bounce must not also take the realtime IBL rung;
  `baked-lightmap-pipeline.md` §3 already states this, and this ADR is what makes it load-bearing
  rather than theoretical.
