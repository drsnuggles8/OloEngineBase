# Textured hybrid reflection hits (#1355)

The Vulkan Deferred mirror tier shades a committed hit with its canonical GPU Scene material. It keeps the mirror direction, roughness gate and render-graph ordering from [ADR 0020](../adr/0020-reflection-tier-selection-contract.md).

## Material and lighting policy

The TLAS custom index selects the instance. Its geometry and material slots must be active and match the instance's generations. The primitive index is local to its geometry range. The triangle fetch applies `FirstIndex` and `BaseVertex`, uses the ray query's transforms and interpolates UVs and normals with its barycentrics. Mirrored instances retain the winding sign for front-face and bias decisions. The analytic UV frame uses world-space edges and inverse-transpose vertex normals.

Albedo and emissive maps use the canonical sRGB texture views. Their linear samples multiply material factors, and instance tint also multiplies albedo. The blue and green channels of the linear metallic-roughness map multiply the corresponding factors. The linear normal map uses `NormalScale`. The red occlusion channel and `OcclusionStrength` attenuate diffuse environment lighting at the hit. The shared masked-candidate path samples albedo alpha at mip 0 and honors the material cutoff. Mip 0 also remains the CPU and GPU reference tracers' policy.

The local closure evaluates one directional light with the material's PBR model and at most one visibility ray. It uses a minimum hit roughness of 0.2 for that directional highlight. The existing high-mip environment sample supplies diffuse ambient light. Emissive radiance comes from the material factor and map. Local lights, additional bounces, skin and foliage transport, and a secondary environment specular term are outside this closure. Transmission and unsupported back-face hits defer to the lower tier. Degenerate geometry, nonfinite hit radiance and a stale record also defer. While the material descriptor table is incomplete, the pass stands down before dispatch.

The hit radiance replaces the same indirect-specular term it did in the factor-only tier. The pass still exports `S + c × (W × L - S)` to SSR and changes the color by the same delta. Misses preserve the upstream term. The textured path adds no ray, attachment, binding or history allocation.

## Ray footprint

Estimate the pixel's angular diameter from the inverse projection and the actual render extent. Multiply the larger axis by the primary travel plus the reflected ray distance. Project two orthogonal diameter vectors onto the hit plane along the ray and transform those vectors through the triangle's UV Jacobian. `textureGrad` uses the resulting gradients for each map and its mip chain. The projection floors the absolute ray-plane cosine at 1/8 to bound the stretch near grazing incidence. Degenerate or nonfinite gradients use the base mip. This model neglects the change of receiver normal across the pixel, which can underestimate blur on curved mirrors or at normal-map discontinuities.

## Native Vulkan capture

Baseline `4554d8c4ae7cb9841f42c756f26eb4f82336abde` used Release on the RTX 4090 at 960 × 540, native Deferred, with SSR enabled and TAA, DDGI and upscale disabled. The first textured run used the same executable and fresh shaders. We inspected the center and lateral poses. The table counts pixels above a 0.002 linear HDR channel delta in `RTSpecular.hdr`.

| Comparison | Center | Lateral |
| --- | ---: | ---: |
| Factor tier off versus on | 21,897 | 22,611 |
| Textured tier off versus on | 21,888 | 22,611 |
| Textured tier on versus repeated on | 0 | 0 |
| Textured tier, all maps exchanged at equal factors | 13,883 | 13,712 |
| Textured tier, albedo map exchanged | 7,765 | 7,668 |
| Textured tier, normal map exchanged | 13,867 | 13,701 |
| Textured tier, metallic-roughness map exchanged | 7,563 | 7,477 |
| Textured tier, masked card added in front of the textured hit | 6,812 | 6,742 |

The factor-only captures were byte-identical when the off-screen maps were exchanged. In the textured run, the reflected checker showed through the holes in the masked card. The factor baseline's roughness transition reduced the peak HDR delta from 0.969 to 0.411 in the center pose at roughness 0.175. At 0.31 the separated term matched the upstream term byte for byte. Color, G-buffer and tier-debug captures confirmed the same underlying scene and visible reflection coverage. The shadow option was enabled. The pass budgets at most one reflection ray plus one sun visibility ray per pixel, or 1,036,800 total at this extent. Actual dispatch count is lower because sky, rough surfaces, misses and unlit hits skip work.

The first GPU path-tracer comparison used 548 samples per pixel in each arm. The exchanged-map reference delta had RMS 0.000141 in the mirror region and estimated RMS uncertainty 0.000126. This fixture established that the raster and ray paths saw the same map changes but gave weak precision for quantifying agreement with reference transport. The per-map HDR comparisons above are detection controls.

## Analytic emissive reference

The final Release editor swapped a flat emitter map from sRGB bytes `[16, 64, 192]` to `[192, 64, 16]`, holding geometry, other maps and the linear 0.32 material factor fixed. For each of 2,204 same-UV samples in the reflected tile, multiply the decoded linear map difference by 0.32 and the measured receiver BRDF weight. The expected mean RGB delta was `[0.148145, 0, -0.151406]`. The captured delta was `[0.148177, -0.000048, -0.152173]`. RGB RMS error was 0.001202 and absolute p95 error was 0.001960, including the texture and Radiance HDR quantization. The isolated emissive check changed 13,867 and 13,687 HDR pixels in the two poses. All four captures exercised the Vulkan ray tier and reported zero shader errors or VUIDs.

## Vulkan settings and fallback

Each settings row used a fresh editor. The table counts the same separated HDR delta as the native captures. Resolved normals were available with 4× Deferred MSAA. The temporal request fell back to spatial upscale on Vulkan with `backendNotOpenGL`.

| Deferred settings | Render extent | Display extent | Center | Lateral |
| --- | --- | --- | ---: | ---: |
| 4× MSAA | 960 × 540 | 960 × 540 | 21,850 | 22,594 |
| Spatial Quality | 640 × 360 | 960 × 540 | 9,752 | 10,055 |
| Spatial Performance | 480 × 270 | 960 × 540 | 5,469 | 5,656 |
| Temporal Quality requested | 640 × 360 | 960 × 540 | 9,752 | 10,055 |
| Odd native extent | 959 × 539 | 959 × 539 | 21,892 | 22,543 |

The upstream indirect specular term and weight were byte-identical across toggles in each row. Vulkan Forward and Forward+ lacked the RT pass and targets, with byte-identical scene HDR and beauty on both poses. This held with hardware ray queries available and with ray queries disabled at device startup. Deferred with ray queries disabled also lacked the pass and matched the upstream scene HDR, indirect specular and weight exactly. Its downstream SSR variation was within the scale measured on repeated captures of each arm.

## Motion and mip control

Two fresh editor sessions captured the same 24-step 960 × 540 trajectory. The control uses the textured shader with only the solid-hit lookup changed from `textureGrad` to explicit mip 0. The tile uses high-frequency albedo with uniform roughness and metallic factors to control for moving glints. The projected tile traveled 7.216 pixels. A reflected-plane projection samples the same 2,268 UVs on every frame. The analysis divides the separated term by its weight before measuring temporal variation.

| Hit sampling | Temporal RMS | RMS relative to signal | Adjacent-step p95 |
| --- | ---: | ---: | ---: |
| Forced mip 0 | 0.022913 | 36.06% | 0.038194 |
| Projected ray footprint | 0.002176 | 3.70% | 0.003322 |

The projected footprint reduces temporal RMS by 10.53× in this fixture. The result covers the measured transformed tile and trajectory. Sharper close-up detail and curved receivers have different footprints.

## Material readiness and lifetime

The Vulkan linear metallic-roughness reload used the same registered handle and changed the image extent from 128 × 128 to 96 × 64. The three reflected HDR probes moved by 0.34241–0.34851 and the no-hit control stayed exact. When decoding a malformed edit failed, the live renderer retained the last usable image and all three HDR probes stayed exact. The registry exposed its failed placeholder. A subsequent valid file restored the registry bytes, while the already-bound raster and reflection materials kept the previous image until scene reopen. The shared recovery case is tracked in #1789. Reopening restored the initial reflected HDR exactly. The Vulkan heap tests also exercised unresolved descriptors, invalidated slots and stable handle identity.

## GPU cost

Four AB/BA pairs ran in fresh Release Vulkan editors on the RTX 4090, with the build lock held exclusively and competing apps stopped. Both arms used the same final executable, 960 × 540 native Deferred, the same geometry and texture data, and the same cameras. The factor-only arm restored the pre-change reflection shader. The metric is the resolved `RayTracedReflectionPass` GPU interval. Each camera contributed 239–240 unique valid GPU frames per run. One duplicate was excluded. The values below are medians of the four run percentiles in each arm.

| Pose | Factor-only p50 / p95 | Textured p50 / p95 | Median paired p50 / p95 delta |
| --- | ---: | ---: | ---: |
| Center | 29.70 / 30.72 µs | 32.77 / 34.82 µs | +3.07 / +4.10 µs |
| Lateral | 29.70 / 30.72 µs | 32.77 / 34.82 µs | +3.07 / +4.10 µs |
| Direct control | 23.55 / 25.60 µs | 23.55 / 25.60 µs | 0 / 0 µs |

Paired p50 deltas for the two reflected poses ranged from +3.07 to +4.10 µs. The ray budget and history allocation are the same in both arms.

## Sky-only control

The global environment A/B used the same imported tile with the sun removed. The first textured shader incorrectly required a per-material IBL flag. Its 2,268 interior samples read zero in both poses. The corrected bound-environment gate produced positive linear HDR in all 2,268 samples per pose, with center mean RGB `[0.08153, 0.02488, 0.03017]` and luma p5–p95 `0.03548–0.03923`. The checker is visible in `sky-before-after-centre.png`. The upstream specular and weight were byte-identical. The SDK loader confirmed the Khronos validation layer in the final native Deferred off/on/emissive capture, with zero reported validation diagnostics.

## Reproduce the comparisons

Use the [live benchmark capture entry point](../guides/renderer-benchmarks.md#the-capture-entry-point) in a fresh Release Vulkan editor. Use `ReflectionHierarchyTest.olo` as the base scene. Make two scratch copies that differ only in `RTReflectionEnabled`. Keep SSR enabled and set FXAA, DDGI, TAA and primary ray-traced shadows off. Leave the reflection's own sun visibility ray enabled. Use seed 1355, fixed `dt = 1/60`, manual exposure 0.7 and 24 warmup frames. Capture `IndirectSpecularTexture`, its weight, `RTReflectionSpecularTexture` when present, and `UIComposite`.

| Pose | Position | Yaw | Downward pitch |
| --- | --- | ---: | ---: |
| Center | `[0, 1.6, 9]` | 0° | 14° |
| Lateral | `[1.1, 1.85, 8.6]` | −4° | 15° |
| Direct tile control | `[0, 1.6, 9]` | −6° | −19° |

Use 45° FOV and clip distances 0.01 and 1000. Move the lateral camera along +X at 0.9 m/s while yaw decreases by 1°/s. Name the motion manifest `textured-motion-960x540-motion.yaml` and point its `Scene` to the generated `textured-motion.olo`. Set the lateral camera's `Sequence` to `{Every: 1, Count: 24, Attachments: [RTSpecular, Weight]}`. Declare both attachments as `hdr`, with sources `RTReflectionSpecularTexture` and `IndirectSpecularWeightTexture`. Divide separated specular by its weight before comparing the same surface points. The mip control replaces the solid-hit `textureGrad` lookup with `oloHeapSampleLod` at mip 0 in an isolated shader copy.

Generate the exact scene, geometry, UVs, factors and embedded PNGs with the [fixture recipe](../../scripts/perf/fixtures/hybrid-reflection-fixtures-1355.py), which uses NumPy, Pillow and PyYAML. The default destination is `OloEditor/assets/tests/visual/HybridReflectionFixture1355/`.

| Arms | Generated inputs |
| --- | --- |
| RT off/on | `off.olo` and `on.olo`, with the original pillar |
| Full and isolated map swaps | `textured-a.olo` versus `textured-b.olo`, `textured-albedo.olo`, `textured-normal.olo`, `textured-mr.olo` and `textured-emissive.olo`, each referencing its namesake GLB |
| Alpha masked | `masked.olo` with `masked.glb` and the foreground `alpha-card.glb` |
| Analytic emitter | `emitter-a.olo` and `emitter-b.olo`, each referencing its namesake flat-emission GLB |
| Motion and timing | `textured-motion.olo` with the albedo-only `textured-motion.glb` |
| Sky only | `--sky-only --arms textured-a` creates `textured-a-sky-only.olo` with the sun removed |

The tile materials `TextureTile0` and `TextureTile1` occupy the two halves of a quad. Each half has unit UVs and its own albedo and emission texels. `MaskedStripe` uses one full quad and a 0.5 cutoff. The fixture recipe holds transforms and factors while changing the map bytes.

For timing use 120 measurement steps per camera in fresh AB/BA runs and the valid unique pass rows in the benchmark trace. Use the shader from `4554d8c4ae7cb9841f42c756f26eb4f82336abde` for the factor baseline and the current shader for the textured arm, both on the same executable.
