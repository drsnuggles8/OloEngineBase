# Reference path tracing and raster fallback (#1346)

Keep the raster colour input available until the acceleration-structure build has published the TLAS for the frame. A pass can be eligible at graph compilation and still need the raster frame when it executes.

## What consumes the frame

The CPU commits GPU Scene instances, geometry, materials, lights, environment and the two path-tracer tables before compiling the frame graph. Skeletal deformation feeds the BLAS/TLAS build. The trace consumes the published TLAS, scene records, emissive and material texture tables, the environment prefilter when available, and four RGBA32F history planes. The shader derives its next sample index from the radiance plane's alpha. The CPU reference and level-zero texture sampling are the comparison standard.

The successful trace overwrites the upstream colour. The zero-address branch copies that colour and carries the four histories forward. Both branches use the same shader. At the furnace camera, bloom, tone mapping, FXAA and UI compositing consumed the traced colour. Tone mapping also declared scene depth for underwater fog. The VSM marking pass rooted depth independently. Editor picking reads the entity-ID attachment of SceneColor after rendering. Removing a single colour dependency therefore has different effects on the three raster paths.

## Experiment

The two workloads were the production renderer with the CPU reference's Cornell geometry and materials, and the 100-sphere material laboratory. The latter used the camera, seed, scene and four samples per frame in `material-lab-furnace.pathtracer.yaml`. All measurements used the same RTX 4090 and Release configuration. The prototype editor SHA-256 was `4984d8036f7e9a0fa50fbbad4fccd025d43d69ff5bf43953dace1bc5fb044fbe`. The scoped prototype omitted the upstream colour read in `GpuPathTracerPass::Setup` for an enabled tracer and bound the renderer's white texture to unused sampler slots when no input resolved. This made the consequence of losing fallback visible and kept sampler descriptors valid. All other graph roots and the transport shader stayed intact.

## Cornell cost and execution

Two pairs ran in full/cut, cut/full order in fresh processes at 320 × 240 and one sample per pixel per frame. Each cell averages the two run medians for each arm. CPU is the sum of the graph's per-pass CPU intervals per frame. GPU is the whole timestamp span of a unique, valid resolved frame. Only frame ages below the timer pool's four-slot ring were admitted. The costs are descriptive for this scene.

| Path | Executed passes, full → cut | CPU ms, full → cut | GPU ms, full → cut | Transient demand, full → cut |
|---|---:|---:|---:|---:|
| Forward | 14 → 14 | 0.225 → 0.227 | 0.531 → 0.487 | 10,444,800 → 10,444,800 B |
| Forward+ | 14 → 14 | 0.239 → 0.241 | 0.551 → 0.551 | 10,444,800 → 10,444,800 B |
| Deferred | 18 → 13 | 0.256 → 0.138 | 0.497 → 0.398 | 10,444,800 → 10,444,800 B |

The Deferred cut removed deferred lighting, the scene-velocity seed, forward overlay, particles and debug overlay. Scene geometry still executed. Both forward paths executed the same pass list in both arms, so their timing differences cannot be attributed to pass culling.

## Frame where availability changes

The production Cornell test makes a published TLAS unavailable at the end of `RayTracingScenePass`, using the same readiness setter as the vegetation producer. This injects the timing of a real producer verdict into a reproducible scene. The shader preserved the radiance and count. Before the accounting fix the CPU still claimed one sample and reported the tracer active. The corrected pass fetches the trace address once at execution and uses it for both the shader and the counters.

All six attachment readbacks at 15 samples per pixel were bitwise identical on Forward, Forward+ and Deferred in the successful trace. With the TLAS withheld, the prototype replaced the raster fallback with a flat image on all three paths. Each comparison differed at all 76,800 pixels (320 × 240), with a maximum channel difference of 173/255.
## Material laboratory cost and picking

At 1280 × 720 on Deferred, six pairs of fresh editors ran in alternating order. Each run contributed 240 unique GPU frames after warmup. All six exported attachments matched across all 12 runs at 976 samples per pixel. Each pair saved exactly 14,871,212 bytes of transient pool demand. The table shows the median of the six run medians for each arm. Paired savings are computed within each pair before taking the median. The range gives the smallest and largest paired saving.

| Measurement | Full | Cut | Median paired saving (observed range) |
|---|---:|---:|---:|
| CPU, ms | 1.168 | 0.980 | 0.194 (−0.054 to 0.227) |
| GPU, ms | 4.148 | 3.387 | 0.620 (−0.918 to 0.894) |
| Completed editor frame, ms | 4.149 | 3.446 | 0.570 (−0.921 to 0.851) |
| Transient demand, B | 153,723,052 | 138,851,840 | 14,871,212 |
| Executed / culled passes | 19 / 52 | 15 / 56 | 4 fewer executed |

The four removed passes were deferred lighting, GTAO, particles and the scene-velocity seed. Five pairs were faster and one was slower. With the reference still visibly showing the same spheres, full mode returned entity IDs 81 and 80 at (1035, 405) and (947, 409). Clicking those pixels selected the corresponding material spheres. In the cut both IDs were −1. Two more sphere clicks also lost their selection. A sky pixel remained empty in both arms. Capture confirmed zero Vulkan validation messages and zero editor errors.

The production regression snapshots the selected raster input and tracer output immediately after the same draw. Lavapipe's filtered read changed 733 of 76,800 RGB pixels on each path, with a maximum float difference of 0.001953125. An exact texel fetch for matching extents preserved every RGB texel on all three paths. Different extents keep scaled sampling. The four accumulation planes are checked byte for byte on the withheld frame.

## MSAA and upscale

At 1280 × 720 and 4× MSAA the Deferred G-buffer and resolve pass remained live in both arms. The observed resolve intervals were 0.082 and 0.081 ms (one sample each). With spatial Quality upscale, the traced attachments were 640 × 360 and the display was 960 × 540. EASU read `PathTracerColorTexture@GpuPathTracerPass`. Bloom read `EASUColorTexture@EASUPass`. The observed EASU intervals were 0.015 and 0.016 ms. Both cells matched all six exported attachments at 16 samples per pixel, with zero validation messages. Resource sizes and consumers were checked in the live graph.

On OpenGL with FSR2 Quality, the raster renderer produced an 853 × 480 input and a 1280 × 720 display. Requesting the tracer left its shader unavailable and its graph target absent. Four fresh off/on/on/off captures at 64 mocked ticks matched every display pixel (maximum delta and RMSE 0).

## Reproducing the comparison

* The source baseline was `2df5d5f751f28ba511a67dc54b2fad879584ce2a`, with the probe and the late-TLAS accounting fix described above. The full/cut arms used the same executable, driver 621.14 and scene inputs.
* Run `ReferencePaths/GpuPathTracerFrameGraphTest.*` in the Release test binary with `--olo-rhi=vulkan --olo-require-vulkan --gtest_output=xml:cornell.xml`. The production test records pass timings, GPU frame identity, transient demand and persistent pass-target bytes under `frameCosts`. It writes front and raking composites to `OloEditor/assets/tests/visual/`.
* For the experimental arm, omit the upstream candidate read in the enabled tracer's `Setup` and use `Renderer3D::GetWhiteTexture()->GetRHIHandle()` where that input is missing in `Execute`. Keep the real TLAS, prefilter and history reads and bindings. This temporary diagnostic is deliberately absent from the production API. Compare the six decoded float attachments at the same seed and actual sample count. Check the composite again on the injected late-withheld frame.
* For the furnace, run `olo_benchmark_capture` on `material-lab-furnace.pathtracer.yaml` in a Release Vulkan editor, with the manifest camera held stationary. The timing variant used 120 camera steps per run and retained each completed frame. Export graph topology, transient demand, pass timings and all six attachments. Check `olo_pathtracer_stats` for active tracing and nonzero samples. Inspect the entity-ID attachment and select visible spheres in the viewport. Use the float readbacks for the Cornell comparisons and the exported attachment bytes for the furnace.

## Decision

Keep the full comparison graph as the production path. The measured cut pays back on the Deferred furnace, but it saves no passes on either forward Cornell path and removes current-frame raster fallback and editor picking. Restoring the full graph one frame later would expose exactly the bad frame reproduced above. Keying the cut on the tracer's precompile availability would make the same mistake whenever the build withdraws the address.

A secondary view rendered after the world build can use a published TLAS, but restricting the cut to such views saves nothing on either measured single-view workload. A static-scene specialization would need the producer to guarantee the address through execution. Absence of scene edits alone does not express vegetation readiness or pending acceleration-structure work. It would also need every raster consumer, including hover and diagnostic reads, to declare demand before the graph compiles. A split before and after publishing the TLAS could support a same-frame choice. That changes scheduling, history ownership across the split and resource declaration for all three raster paths. For the measured workloads we retain the existing topology and the verified sample-accounting fix. Revisit a restricted graph if a caller can supply both a frame-stable trace result and its complete output demand.
