# Render-graph declaration configuration (#1333)

> Work in progress. This commit records the inventory the refactor is built from; the rule and
> the mechanism are written up once they exist.

## Inventory at `c5cf5c6a9`: what Setup reads versus what the key hashed

`BuildFrameGraph(key)` and `PopulateBlackboard` both skip their work when
`ComputeBlackboardFingerprint` matches the previous frame. Any input either of them branches on,
sizes a declaration from, or latches for `Execute()` must therefore be in the key. The table lists
every input that was not, found by reading all 60 `Setup(RGBuilder&, FrameBlackboard&)` bodies
and the whole of `PopulateBlackboard` against the hashed list.

### Under-invalidation: read, not hashed

| # | Input | Read by | What a warm cache does |
|---|---|---|---|
| U1 | Graph physical (display) size | `PopulateBlackboard`: every display-res post output, Bloom mips, water/fluid refraction, UIComposite, OIT, TAA/cloud/outline specs | Only the scene band was hashed. Under FSR1 a display resize that floors to the same band keeps every post target at the old size. |
| U2 | `Precipitation.Enabled && (ScreenStreaks \|\| LensImpacts)` | `PopulateBlackboard` (`PrecipitationColor`), `PrecipitationRenderPass::Setup` | On: the pass stays culled and draws nothing. Off: the cached rename survives while `Execute` returns early, so Fog/ChromAb/ColorGrading read an unwritten transient. |
| U3 | `GTAODenoiseEnabled && GTAODenoisePasses > 0` | `GTAORenderPass::Setup` (declares and latches the denoise pong) | Turning denoise on: `Execute` finds no pong and publishes "no occlusion" every frame. |
| U4 | `EASU` / `DepthVelocityUpscale` readiness | `PopulateBlackboard` (`EASUColor`, `UpscaledDepthVelocity`) | Both passes were missing from the hand-written `HashPassState` list. |
| U5 | TAA history import after a resize | `PopulateBlackboard` | The flag is hashed before `EnsureHistoryStorage` clears it; the next frame reproduces the cached key and TAA runs history-less. SSR had a forced invalidation for this; TAA did not. |
| U6 | Particle render callback present | `ParticleRenderPass::Setup`, OIT prepare/resolve `m_HasContributors` | A frame without a scene callback caches a culled particle node. |
| U7 | The two cache layers' keys | `Renderer3DFrameExecution.cpp` vs `PopulateBlackboard` | The build key was computed before the FSR1 scene-band resize, the populate key after it: two keys for one frame. |
| U8 | ReSTIR DI/GI history planes other than the reservoir sample | ReSTIR Setup latches all five | Only one plane's validity was hashed; correct today only because the planes invalidate together. |
| U9 | DDGI probe-data texture | `DDGIProbeUpdatePass::Setup` import descriptor | Hashed by raw GL name, so a recreate that reuses the name keeps the old descriptor. |
| U10 | ReSTIR PT `StandDown()` inside `Setup` | `ReSTIRPTPass::Setup` | `Active` flips after it was hashed. |

### Over-invalidation: hashed, not a declaration input

| # | Hashed input | Why it does not change a declaration |
|---|---|---|
| O1 | Environment-map identity | Populate imports irradiance/prefilter/BRDF only; the environment map is bound in `Execute`. |
| O2 | `RayTracedReflection.TierDebugView` | A UBO flag in `Execute`. |
| O3 | `SkinDiffusion.Quality` | The tap table is rebuilt in `Execute`; `Setup` reads only the enable. |
| O4 | Raw `SSREnabled` / `SSGIEnabled` / `ContactShadowEnabled` / `RayTracedReflection.Enabled` / `GpuPathTracer.Enabled` | Each is folded into a resolved per-pass verdict that was also hashed; the raw bit rebuilds the graph on paths where the verdict cannot change (forward, no ray tracing). |
| O5 | Requested `ActiveAOTechnique` | Populate reads the graph's technique; the requested one only reaches a log line. |
| O6 | `ReSTIRPT.SpatialReuse` | `ReSTIRPTPass::Setup` reads only `Active`. |

### Execution-only (never a declaration input)

Camera matrices, view position, jitter, `StochasticFrameIndex`, cloud frame index, time, object
transforms and every UBO value are pushed by the uncached `ConfigurePassesForFrame`. No `Setup` and
no populate branch reads them.

Ping-pong is not frame-parity dependent anywhere: the DDGI atlases, the froxel-fog volumes and the
ReSTIR PT pools import BOTH halves under stable names every build and choose the current half in
`Execute`; the JFA and GTAO denoise ping/pong alternate inside one `Execute`; the ReSTIR spatial
extraction source depends on `SpatialPasses`, not on the frame.
