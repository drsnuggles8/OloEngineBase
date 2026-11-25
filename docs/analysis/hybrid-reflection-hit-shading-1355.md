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
