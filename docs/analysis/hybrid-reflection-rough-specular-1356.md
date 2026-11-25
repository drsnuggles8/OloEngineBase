# Rough-specular reflection trial (#1356)

Keep the textured mirror tier for production. The one-ray visible-normal estimator converged to the independent reference over repeated frames. The tested spatial and temporal reconstruction remains noisy at moderate roughness, gives almost the same error as zero fallback at high roughness, and suppresses valid signal after a lighting change.

## Estimator and independent reference

The trial samples a GGX visible half vector with the existing `sampleGGXVNDF` and one scrambled Sobol point per pixel and frame. It reflects about that half vector and accumulates zero below the horizon. The single-scatter factor `f × cos(theta) / pdf` simplifies to Schlick Fresnel times the height-correlated Smith `G2/G1`. The deliberate incorrect PDF omits `G1` and scales the sample by `1 / (1 + Lambda(V))`.

The independent CPU integration samples the faces of a constant-radiance box on a deterministic grid. For each receiver point, each visible area sample contributes with its projected solid angle, `cos(theta_emitter) × dA / distance²`, and the GGX BRDF times the receiver cosine. It uses double precision and no engine sampler or PDF. The GPU output is raw geometry-only linear HDR in `RTSpecular`. `RTColour` carries hit distance, throughput and gate confidence. A miss has no box contribution.

These results use a shiny metal receiver with RGB F0 `[0.90, 0.90, 0.92]`. Each row uses 48 actual GPU frames at one sample per pixel. Measurements use the 150 × 58 pixel mirror region starting at (75, 115). Refining the face grid from 16² to 24² changed mean luma by at most 0.000012. The range shows the six consecutive eight-frame means and describes seen variation under the same run.

| Roughness | CPU reference | Correct GPU mean | Eight-frame range | Wrong-PDF GPU mean |
| --- | ---: | ---: | --- | ---: |
| 0.20 | 0.216372 | 0.216855 | 0.215753–0.218206 | — |
| 0.45 | 0.089690 | 0.089841 | 0.087711–0.090657 | 0.087363 |
| 0.65 | 0.034569 | 0.034116 | 0.033320–0.035390 | 0.030745 |
| 0.80 | 0.015859 | 0.015249 | 0.014303–0.016073 | 0.012231 |

An independent synthetic CPU cross-check swept the same four roughnesses, view cosines `1.0, 0.55, 0.18, 0.08`, and dielectric and colored-metal F0. All eight valid means matched half-vector quadrature within four sample standard errors plus twice the grid-refinement difference and 0.0001. The wrong PDF exceeded that bound in all six oblique controls. Normal incidence leaves `G1 = 1`, so it cannot expose this particular mutation.

## Reconstruction and failure modes

The offline replay uses the moment update and 2% relative depth comparison from `SurfaceHistory.glsl`. It combines a 3×3 spatial kernel with an eight-sample temporal cap and limits history to the current 3×3 range. The reference and all reported errors below use linear luma. This replay uses the actual raw hardware ray captures. It leaves the production render graph and `TemporalHistoryRegistry` unchanged.

| Roughness | Raw 1 spp RMSE | Spatial and temporal RMSE | Mirror and probe RMSE | Mean of all 48 raw frames RMSE |
| --- | ---: | ---: | ---: | ---: |
| 0.20 | 0.44759 | 0.10412 | 0.25662 | 0.06005 |
| 0.45 | 0.38159 | 0.09019 | 0.11091 | 0.04811 |
| 0.65 | 0.23451 | 0.03379 | 0.03634 | 0.03036 |
| 0.80 | 0.15243 | 0.01611 | 0.01611 | 0.02052 |

- **Motion and off-screen source.** The emitter is above the direct view. In the rightward 0.9 m/s dolly, five sampled frames at roughness 0.65 gave reconstructed RMSE 0.03316–0.03382, against 0.03605–0.03627 from fallback and 0.21951–0.24585 from new samples. Hit distances ranged from 7.47 to 23.88 m. The replay used captured camera poses to reproject the floor and rejected 3,603 out-of-bounds or inconsistent history samples. A stricter 10% hit-distance test would reject 2.86% of otherwise eligible reprojections because the sampled ray changed, even on the same receiver.
- **Foreground disocclusion.** A nearer block crossed the projected floor under the same dolly. The roughness lane identified which primary pixels showed the floor. The replay found 990 newly revealed floor samples and used the current raw signal with zero retained history error at those pixels. Pixels without valid receiver geometry kept the lower tier.
- **Changed secondary lighting.** Replay joined the two 0.65 static runs at 24 frames, changing the box from 2.0 to 0.25 linear radiance without changing the primary receiver. Mean reference luma after the change was 0.004321, versus 0.004350 for the first raw frame, 0.001444 for the first reconstructed frame and 0.000548 for the fourth. The sparse newly dark samples over-clamped valid history. Across steady 0.65 frames, the clamp changed 13.83% of ROI pixels on average and the raw variance estimate averaged 0.05149.

## Composition and budget

A separate arm restored the scene's sky, geometry and SSR. It normalized the sampled single-scatter weight by its directional lobe integral, then clamped the updated specular term before applying its actual delta to the color. This preserves the non-specular remainder even when a sampled black hit and a normalized weight above one request a subtraction larger than the original specular. At roughness 0.65, upstream scene color, probe specular and BRDF weight matched the mirror arm byte for byte. For 1,287 pixels whose specular changed by more than 0.02, RMS disagreement between the changes in color and specular was 0.00237 against RMS specular signal 0.77986. A black-emitter control with independent floor emission measured 0.00102 RMS disagreement over 1,203 changed pixels against 0.10412 RMS specular signal. The comparisons include Radiance HDR quantization. SSR consumed the resulting term. The normalization control covers the scalar roughness and Fresnel model of this receiver.

The trial caps reflection work at one ray per eligible pixel. With sun shadow tracing disabled, this is at most 57,600 queries per 320 × 180 frame. The mirror tier gates these higher-roughness receivers out. The experiment uses the same two GPU outputs and adds no GPU history allocation because reconstruction runs offline. A full RGB implementation using four RGBA16F history planes would cost 32 bytes per pixel per bank, or 3,686,400 bytes for two banks at 320 × 180 before extra surface identity or motion data. The shipping allocation remains zero.

Four AB/BA pairs in fresh Release Vulkan editors on the idle RTX 4090 measured the textured mirror and the historical trial composition shader at 320 × 180, roughness 0.65. Each run supplied 232–240 valid unique GPU frames after excluding eight duplicate records from one mirror run. The median `RayTracedReflectionPass` p50/p95 was 15.36/15.36 µs for the mirror and 20.48/22.02 µs for the trial. Median paired deltas were +5.12 µs at p50 and +6.66 µs at p95, with p95 deltas between +6.14 and +8.19 µs. These intervals cover the timed shaders before the subtraction clamp correction and exclude offline reconstruction. To reproduce that comparison, take both the shader and the overlay generator from `b45d11d0856013a1ed0f9d7cb974483b7429788c`.

## Reproduce the trial

Start from textured mirror commit `6e915395204417b2a34f4156f79dbbcd0cb8623c` and build Release. The measurement executable was the unchanged base Release editor, SHA-256 `C19E53BDAE8F79166EAA929FACF171A1FB07729BA3849B6811F8A0CD9EEFA7AC`, with the CAP-04 shader tree. The trial needs no C++ change. Use a separate shader tree and a fresh editor process for every variant. Generate the shader with the [trial overlay generator](../../scripts/perf/fixtures/hybrid-reflection-vndf-1356.py), passing the CAP-04 `RayTracedReflection.glsl` as input and the separate editor's copy as output. Use the default `raw` view, `--wrong-pdf` for the negative control and `--view composition` for the composition control. Captures use an RTX 4090 and Vulkan 1.4.357.

Copy `ReflectionHierarchyTest.olo`. For raw captures keep the camera, 40 × 40 floor at y=0 and `RedPillarBehind` cube, with the cube centered at `[-2.5, 9, -9]`, scaled `[2.5, 3, 2.5]` and emitting `[2, 2, 2]`. Clear the cube albedo. The control for changed light emits `[0.25, 0.25, 0.25]`. Disable SSR and sun visibility rays. Use native Deferred 320 × 180, 1× MSAA, no upscale, no DDGI and no TAA. Use a 45-degree FOV at `[1.1, 1.85, 8.6]`, yaw −4 degrees and pitch 15 degrees. Warm for 24 frames, then capture `RTReflectionSpecularTexture` and `RTReflectionColorTexture` at each of 48 frames with mock `dt = 1/60`. The mirror arm uses the original shader with those same inputs. Use renderer seed 1355 and overlay seed 1356.

For motion, add the 0.9 m/s rightward movement and −1 degree/s yaw. For disocclusion, keep the `Occluder` cube at `[1, 0.35, 4.25]`, scale `[0.9, 0.7, 1]` and roughness 1, and capture `GBufferNormal` on the same frames. For the composition control restore the remaining scene entities and enable SSR. For its black-emitter arm set the box emission to zero and the floor emission to `[0.25, 0.12, 0.17]`. Export `SceneColor`, the indirect specular term and weight, RT color and specular, and `SSRColorTexture`. Keep generated HDR and PNG output under `OloEditor/assets/tests/visual/`.

Run the [offline analysis](../../scripts/perf/fixtures/hybrid-reflection-analyze-1356.py) with NumPy and Pillow. Pass `--capture-root` with the parent of `rough-raw`, `rough-wrong-pdf`, `rough-mirror`, `rough-motion` and `rough-disocclusion`. Name each run folder with a prefix followed by `-r20`, `-r45`, `-r65`, `-r80` or `-r65-dim` as appropriate. Keep the benchmark's `result.json` and `sequence/` in every run folder. The analysis emits the reference and reconstructed PNGs, `analysis.json` and NumPy diagnostics containing per-pixel variance, history validity, clamp masks and hit distances. Raw signal and gate confidence remain in the two HDR sequences.
