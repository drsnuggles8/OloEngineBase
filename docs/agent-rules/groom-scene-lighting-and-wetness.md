# A coat lit by its body's scene, and wet where the water reached it

Rules for the groom strand shader's scene inputs (#1618) and for wet coats (#1615). Each one is a
place where the coat and the body beside it disagreed, or would have, with every test green.

## 1. A coat takes the scene's lighting from the same sources as its body

- **Ambient through the ladder's rung, not the sky cube.** `oloGroomAmbientRadiance` asks
  `oloSelectAmbientDiffuseRung` (include/AmbientLadder.glsl) for the rung the body's ladder picks:
  probe volume (DDGI or baked SH) at the strand, then the irradiance cube, then the flat fill, with
  `DeferredLightingPass::AmbientLadderControls()` as the controls. Take the rung's normalised
  irradiance times its scale; never its Lambertian diffuse, which a fibre does not have. A coat that
  read only the cube stayed sky-lit in a closed red room while its body turned red.
- **Sample the probes through `include/GroomAmbient.glsl`, not through the ladder.** Calling
  `oloSelectAmbientDiffuseRung` twice from the strand shader doubled GroomPass (3.2 to 6.2 ms with
  one light, 15 ms with 256) with no probe volume bound: `sampleLightProbeGrid`'s two 9-entry
  coefficient arrays raised the register peak of a shader whose quadrature loops already hold most
  of them. SH evaluation is linear in the coefficients, so the groom accumulates each probe's
  irradiance directly and gets both directions in one pass (band 1 flips sign). Bisect a shader's
  cost by runtime variants against one binary: the base shader, then one feature reverted at a time.
- **Clustered lights on the paths whose clusters are live.** `fplus_Params.z` says so (Forward+,
  and Deferred, which always culls). Walk the multi-light buffer for its directional lights only,
  then the cluster list, as `PBR_MultiLight.glsl` does. Turn each cluster record into the
  `LightData` `GPUSceneLightAdapter::ToMultiLightData` would write for the same light, so one
  per-light function shades both walks.
- **Include `ForwardPlusCommon.glsl` after the shader's own `VirtualShadowSampling.glsl`.** The
  engine's include processor inlines each file once, textually, before the preprocessor runs.
  `ForwardPlusCommon.glsl` includes the VSM file inside `#ifdef FPLUS_ATLAS_SHADOWS`; included
  first without that define, it spends the file in a dead block, the shader's own include is then
  skipped, and the fragment stage fails to compile in the engine (`'vsmProjectIntoClip' : no
  matching overloaded function`). glslc runs a real preprocessor and passes the same file.
- **Re-publish the Forward+ snapshot in the groom pass.** ScenePass's `UnbindAfterShading` empties
  bindings 9-12 and 18 after an active frame, GPUScene owns 9 and 10 too, and every pass that
  rebinds the shared scene resources uploads `Enabled = 0` to 25. Do what `VolumetricFogPass`
  does: `CaptureShadingBindings()`, bind the five buffers, upload the parameters to a pass-owned
  UBO at 25, and `ConsumePublication(ForwardPlusLightClusters)` in Setup.
- **A pass that reads the clusters but is absent from the depth prepass turns off depth-aware
  culling.** The 2.5D cull keeps a light only for the depth cells opaque geometry occupies in its
  tile. A strand at the silhouette, in front of open sky, sits in no occupied cell, so it would
  lose every local light. `DepthAwareFrameInputs::HasGroomGeometry` joins blended and virtual
  geometry in forcing the conservative grid.
- **Fog needs nothing from the strand shader.** The fog pass samples `Scene.SceneDepth` after the
  groom pass, and the coat writes depth there on every path. `GroomSceneLightingEvidenceTest`
  checks that depth at every coat pixel before trusting a fog picture.

## 2. Measure a coat's height on the coat as it stands, in world metres

A coat's wet line is a height above its origin. Measure a point `p` of groom object space as
`dot(p, H)`, where `H` is the world-y **row** of the coat's transform (`GroomWetnessAxis`): world
y of `M * p` is `origin.y + dot(p, H)` for any rotation, scale or parent. Never use "local +Y
times its length": the dog's body is turned a quarter about Z, so its coat's local +Y is level,
and every coat parented to that body would have measured nothing.

## 3. Wetness that moves strands lives in the deformation buffer

The geometric response (lie down toward gravity, shorten, gather toward the clump) runs inside
`oloGroomDeformPoint`, from a per-frame wet record and a static per-strand clump record in the
same buffer the root transforms are in. Every consumer of a bound coat's geometry reads that
buffer: the strand draw, both shadow casters, the coat bake's drawn pose and the ray-traced proxy.
Lanes in one consumer's UBO would have left the others with a dry coat's shadow and proxy. The
CPU-deformed path applies the same record through `GroomDeformWetLocal`, and
`GroomGpuDeformation.AWetCoatReproducesTheCpuStreamExactly` holds the two bit for bit.

The optical response (absorption, gloss, sheen) is per vertex from UBO lanes, so an unbound coat
gets it too. An unbound coat has no root frame, so it has no geometric response.

## 4. A feature that is off costs nothing on the coat's per-point CPU paths

The coat self-shadow evaluates every posed point on the CPU every frame
(`EvaluateGroomDeformedPose`, about a million points on the long-coated horse), and so does the
CPU-deformed stream. Anything added per point there must be decided once per loop, not per point:
`DeformedPoint<Wet>` is a template, picked once, with the root span and the simulated flag hoisted
out. The first version put the wet hook inside `EvaluateGroomDeformedPoint`. That made the function
too large to inline and added a per-point test, and a dry horse's GroomPass doubled
(GL 5.2 → 11.0 ms).

**GroomPass's GPU time counts this CPU work.** The pass's timer brackets the whole pass, and the GPU
idles while the CPU builds the pose, so a CPU regression reads as GPU milliseconds. When the
sub-passes (`StrandPrepass`, `StrandShade` in `olo_perf_pass_timings`) add up to far less than the
pass, time the pass's sections on the CPU instead of bisecting the shader. To compare against
base, alternate a base editor and the branch editor on the same scene and camera.
