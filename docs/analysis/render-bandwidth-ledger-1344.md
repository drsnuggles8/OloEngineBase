# Render bandwidth ledger (#1344)

## Source baseline

The baseline revision is `8d2adf7d178febe8e8fc8973a8326f28fb5f636c`. Deferred geometry writes six
colour attachments and D24S8 depth-stencil. The velocity attachment gained coverage and material
profile in #1256. The multisample path uses the graph-scheduled resolve from #1605. [G-buffer
attachment table](../../OloEngine/src/OloEngine/Renderer/GBuffer.h) · [Framebuffer construction and
resolve](../../OloEngine/src/OloEngine/Renderer/GBuffer.cpp)

| Target | Format | Bytes per pixel per sample | Semantics |
| --- | --- | ---: | --- |
| RT0 | RGBA8 | 4 | Base colour and metallic |
| RT1 | RGBA16F | 8 | Octahedral world normal, roughness and ambient occlusion |
| RT2 | RGBA16F | 8 | HDR emissive and material flags |
| RT3 | RGBA16F | 8 | Velocity, coverage and material profile |
| RT4 | R32I | 4 | Signed picking ID, with −1 for background |
| RT5 | RGBA16F | 8 | Baked irradiance and lightmap coverage |
| Depth-stencil | D24S8 | 4 | Depth and stencil |
| **Total** | | **44** | |

The storage estimates below use native scene extents and decimal bytes. At 4× the G-buffer has
four draw samples and a separate one-sample resolve target. Each ReSTIR history total covers the
five persistent RGBA32F planes of one effect. Stage scratch has separate lifetimes and backing.
The scene diffusion hand-off is attachment 4 of SceneColor across the raster paths.
[Graph allocation descriptors](../../OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp)

| Storage estimate | Bytes per pixel | 1920 × 1080, B | 3840 × 2160, B |
| --- | ---: | ---: | ---: |
| Deferred G-buffer, 1× | 44 | 91,238,400 | 364,953,600 |
| Deferred G-buffer, 4× including resolve target | 220 | 456,192,000 | 1,824,768,000 |
| Scene diffusion hand-off | 8 | 16,588,800 | 66,355,200 |
| ReSTIR DI persistent histories, when engaged | 80 | 165,888,000 | 663,552,000 |
| ReSTIR GI persistent histories, when engaged | 80 | 165,888,000 | 663,552,000 |

The scheduled multisample resolve blits five colour planes, or 36 bytes of payload per output
pixel (74,649,600 B at 1080p and 298,598,400 B at 4K). Those figures leave shader fetches and writes
out of the ledger. The flags shader repairs the RT2 alpha bitfield, and the depth/entity shader
takes both values from sample zero. Depending on the per-sample path and late writers, separate
depth-only resolves can also appear. [Resolve scheduling and coverage](../../OloEngine/tests/Rendering/PropertyTests/RenderTargetExportEvidenceTest.cpp)

The runtime ledger uses the copy ledger's known payload bytes per completed graph execution
and keeps multisample source counts separate. GPU timestamps measure elapsed work. Vulkan's
memory report attributes VMA allocation sizes, including alignment, and reports retirement
separately. OpenGL backing is a format estimate and driver residency is unknown.
[Copy ledger contract](../../OloEngine/src/OloEngine/Renderer/Debug/RenderTargetCopyLedger.h) ·
[Memory reporting contract](../agent-rules/renderer-memory-accounting.md)

The viewport scale in `RenderGraph::SetRenderScale` adjusts the area drawn into existing storage.
An upscale preset resizes scene-band attachments and reuses the full display extent for the
downstream chain. At Performance scale, a 3840 × 2160 display gives a 1920 × 1080 scene band. The
G-buffer estimates then follow the 1080p column, while display-sized histories retain their display
extent. [Viewport scale](../../OloEngine/src/OloEngine/Renderer/RenderGraph.cpp) ·
[Scene-band resize](../../OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp)

## Native baseline captures

The Release editor used clang-cl 23.1.0 and an RTX 4090 with driver 621.14. OpenUSD was disabled
for the local build. Each run launched a fresh editor, selected the backend explicitly and used
the face-fill camera from the matching `skin-dominant-*.yaml` manifest. The skin-off manifest
uses the same head geometry with the skin material disabled. Captures used 96 warmup steps and
144 measured steps, with frame throttles and autosave disabled. GPU medians use unique resolved
frame IDs. Source assets, manifests and the benchmark capture tool are shared with the existing
skin workload. [Benchmark replay](../guides/renderer-benchmarks.md)

| Backend | Path | Display | MSAA | Skin | Copies/frame | Payload, B/frame | GPU median, ms | Tracked live GPU, B (allocations) |
| --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: |
| GL | Forward | 1080p | 1× | Off | 4 | 43,545,600 | 2.07 | 881,881,916 (357) |
| GL | Forward | 1080p | 1× | On | 6 | 68,428,800 | 2.40 | 910,959,419 (360) |
| GL | Forward+ | 1080p | 1× | Off | 4 | 43,545,600 | 2.14 | 881,882,108 (358) |
| GL | Forward+ | 1080p | 1× | On | 6 | 68,428,800 | 2.42 | 910,959,611 (361) |
| GL | Deferred | 1080p | 1× | Off | 6 | 68,428,800 | 2.34 | 964,826,108 (358) |
| GL | Deferred | 1080p | 1× | On | 8 | 93,312,000 | 2.62 | 993,903,611 (361) |
| GL | Deferred | 1080p | 4× | Off | 11 | 143,078,400 | 4.50 | 1,329,779,708 (359) |
| GL | Deferred | 1080p | 4× | On | 13 | 167,961,600 | 4.86 | 1,358,857,211 (362) |
| GL | Deferred | 4K | 1× | Off | 6 | 273,715,200 | 6.35 | 2,165,709,260 (358) |
| GL | Deferred | 4K | 1× | On | 8 | 373,248,000 | 7.49 | 2,269,436,363 (361) |
| GL | Deferred | 4K | 4× | Off | 11 | 572,313,600 | 14.97 | 3,625,523,660 (359) |
| GL | Deferred | 4K | 4× | On | 13 | 671,846,400 | 17.07 | 3,729,250,763 (362) |
| Vulkan | Forward | 1080p | 1× | Off | 4 | 43,545,600 | 1.38 | 1,112,524,000 (344) |
| Vulkan | Forward | 1080p | 1× | On | 6 | 68,428,800 | 2.04 | 1,153,507,552 (347) |
| Vulkan | Forward+ | 1080p | 1× | Off | 4 | 43,545,600 | 1.42 | 1,112,524,000 (344) |
| Vulkan | Forward+ | 1080p | 1× | On | 6 | 68,428,800 | 1.72 | 1,153,507,552 (347) |
| Vulkan | Deferred | 1080p | 1× | Off | 6 | 68,428,800 | 1.88 | 1,200,998,496 (351) |
| Vulkan | Deferred | 1080p | 1× | On | 8 | 93,312,000 | 2.18 | 1,241,982,048 (354) |
| Vulkan | Deferred | 1080p | 4× | Off | 11 | 143,078,400 | 2.94 | 1,602,078,816 (358) |
| Vulkan | Deferred | 1080p | 4× | On | 13 | 167,961,600 | 3.30 | 1,643,062,368 (361) |
| Vulkan | Deferred | 4K | 1× | Off | 6 | 273,715,200 | 4.43 | 2,612,545,632 (351) |
| Vulkan | Deferred | 4K | 1× | On | 8 | 373,248,000 | 5.46 | 2,751,833,184 (354) |
| Vulkan | Deferred | 4K | 4× | Off | 11 | 572,313,600 | 11.06 | 4,216,866,912 (358) |
| Vulkan | Deferred | 4K | 4× | On | 13 | 671,846,400 | 12.17 | 4,356,154,464 (361) |

The OpenGL skin-on Forward+ run recorded 287 distinct valid GPU samples. Vulkan Forward+
recorded 284 without skin and 286 with skin. Vulkan Deferred 4K 4× without skin recorded 276.
The other rows each recorded 288. The independent repeat of the OpenGL 1080p 4× skin-on case
recorded the same 13 copies and 167,961,600 B payload, with a 4.95 ms GPU median. All native
capture logs reported zero errors and Vulkan validation IDs.

The OpenGL G-buffer owner's backing estimate matched the source table. Vulkan allocated
106,168,320 B at 1080p 1×, 507,248,640 B at 1080p 4×, 401,080,320 B at 4K 1× and
2,005,401,600 B at 4K 4×. The copy ledger recorded the same payload on both backends.

## Stochastic features and scaling

The two ReSTIR feature baselines use the stationary camera in `integrated-deferred.yaml` at
native resolution. The enabled scene runs DI with two initial candidates and an engagement
margin of zero, and runs GI. Four eligible emitters made DI engage. The separate fallback
control used four candidates. DI reported clustered fallback and still held 165,888,000 B
of history at 1080p. GI remained engaged in that control.

| Vulkan Deferred | Copies/frame | Known payload, B/frame | GPU median, ms | VMA live GPU, B (allocations) |
| --- | ---: | ---: | ---: | ---: |
| 1080p, DI and GI off | 27 | up to 163,535,476 | 24.66 | 2,331,912,512 (640) |
| 1080p, DI and GI on | 37 | 491,762,712–495,004,364 | 29.03 | 3,605,932,352 (677) |
| 4K, DI and GI off | 27 | 489,712,012–492,973,380 | 32.88 | 4,733,797,872 (689) |
| 4K, DI and GI on | 37 | 1,816,806,592–1,820,101,996 | 49.29 | 9,546,761,712 (726) |

Each row has 288 distinct valid GPU samples. The ten extra copies are five history planes
per tier. Their combined payload is 331,776,000 B at 1080p and 1,327,104,000 B at 4K. Shadow
atlas updates explain the changing total payload. The table excludes startup graph summaries
from the copy column.

The 1080p skin-on TAA-off case logged six copies and 60,134,400 B of copy payload on both
backends. With TAA on, it had eight copies and 93,312,000 B. The two persistent history planes
account for the 33,177,600 B difference in copy payload. The TAA-off GPU medians were 2.50 ms
on OpenGL and 1.69 ms on Vulkan, with 288 samples per run. The live memory reports held
944,137,211 B across 359 tracked OpenGL allocations and 1,188,897,888 B across 351 Vulkan
allocations with TAA off.

At 4K output, the Deferred skin workload with spatial Quality upscale realized a 2561 × 1440
scene band. Both backends logged eight copies and 239,657,760 B of payload. The G-buffer owner
held 162,264,960 B of OpenGL estimated backing and 189,431,808 B of Vulkan VMA backing. The
same scene with native 4K output held 364,953,600 B and 401,080,320 B respectively. The Forward
integrated workload with viewport-only scale used a 2561 × 1440 draw region. Its full-size
depth snapshot still copied 33,177,600 B in each sampled frame, corresponding to 3840 × 2160
depth backing.

| Scale case | GPU median, ms | Valid GPU samples | Tracked live GPU, B (allocations) |
| --- | ---: | ---: | ---: |
| OpenGL Deferred spatial Quality | 4.56 | 288 | 1,933,047,023 (364) |
| Vulkan Deferred spatial Quality | 3.84 | 289 | 2,296,959,072 (358) |
| OpenGL Forward viewport-only | 45.59 | 288 | 3,637,797,707 (625) |
| Vulkan Forward viewport-only | 28.69 | 288 | 4,173,004,512 (651) |

The reviewed OpenGL binary also completed the 1080p FSR2 Quality control on Forward,
Forward+ and Deferred. Each case resolved the temporal upscaler from 1280 × 720 to
1920 × 1080 and recorded `FSR2Pass` with 288 valid GPU samples and zero errors.

## Conditional spatial scratch

DI and GI now allocate spatial reservoir framebuffers according to the active draw schedule.
Zero spatial passes extract from the temporal target. One pass allocates spatial target 0 and
extracts from it. Two or more passes ping-pong between both spatial targets. The declaration
key records the scratch count and extraction source. One and three passes share the extraction
source and require different allocations. Two and four passes share both inputs and can reuse
the declaration.

Each spatial framebuffer holds three RGBA32F images. The formats, attachment order and history
layout versions are the same across the before and after runs. Allocated targets keep their
existing render-target signatures. The execution-time fallback still publishes empty reservoirs
when a tier stands down. A transient TLAS or engagement verdict can change within a frame, so
its pass retains the resources needed to publish that fallback. The allocation decision here
depends on configured spatial reuse.

The matched native 1080p runs used both active tiers with one spatial pass each. Three paired
captures saved three live Vulkan allocations and 106,168,032–106,168,608 B of live backing.
Format-estimated transient pool demand fell by 99,532,800 B in each pair. The median GPU frame
delta was −0.032 ms, with individual deltas from −0.053 to +0.247 ms. The saving is in backing.

At native 4K, three completed pairs saved 401,080,320 B of live Vulkan backing and three
allocations per pair. Format-estimated demand fell by 398,131,200 B. Each capture supplied
288 unique valid GPU samples. The median GPU frame delta was +0.124 ms, with individual
deltas from −0.046 to +0.450 ms. Arm order alternated and one 4K pair was replaced after
the editor was minimized during a capture. The completed native pairs each logged 37 copies
and kept all ten ReSTIR history extracts.

The single matched 1080p check with spatial reuse off saved six live Vulkan allocations and
212,336,640 B. The paired two-pass check retained the same backing, allocation count and
scratch demand. Both were compared at equal quality settings. The changes in required scratch
are shared across DI and GI through graph aliasing, so summing their logical target sizes would
overstate the physical saving.

The Release executables used for the native paired captures had these SHA-256 digests.

| Arm | SHA-256 |
| --- | --- |
| Before | `ed8909727733f660d80bc39d707b191a3e18edc0d891fa2bf14f40f92438f5cd` |
| After | `427c4b749d63424933cd1a6575bf155e0aa331ccf197ee316a2d7c4509b83eb0` |

### Motion and history

The 1080p dolly at 4× MSAA and the rapid turn at 4K output with spatial Quality upscale each
completed 144 motion steps with both ReSTIR temporal, spatial and resolve stages running.
The former exercised `GBufferResolvePass` and the latter `EASUPass`. The GPU trace contained
288 valid samples in each arm. The 4× cell recorded 42 copies per frame in both arms. In the
upscaled case, the number varied with the scene's other passes. The ten ReSTIR history copies
continued at scene extent in both arms.

The table compares 8-bit RGB RMSE for a same-build repeat and the allocation change at the
same point in each camera trajectory. The beauty, albedo, normal and velocity PNGs showed
differences of the same scale as the live repeat control. Both camera poses were inspected.

| Motion case | Beauty repeat / changed | Albedo repeat / changed | Normal repeat / changed | Velocity repeat / changed |
| --- | ---: | ---: | ---: | ---: |
| 1080p 4× dolly | 4.215 / 4.207 | 3.945 / 3.785 | 13.893 / 13.285 | 5.327 / 5.122 |
| 4K output, 2561 × 1440 scene, rapid turn | 6.910 / 6.913 | 7.902 / 7.928 | 26.188 / 26.282 | 16.060 / 16.082 |

The dolly saved 106,168,320 B of live backing. Spatial Quality saved 189,038,592 B. Both
cells shed three Vulkan allocations and logged zero errors and validation IDs.

### Live toggles and resize

The Vulkan editor drove both tiers through `1 → 3 → 1 → 0 → 2 → 4` spatial passes, then
selected one pass and resized from 1080p to 4K and back. Each stage had stable transient
pool capacity and compile count between successive live reads. DI and GI remained active
and reported temporal reuse with all ten typed history planes valid. The 1080p histories
kept generation 1 through the toggles, with age 226 after the first one-pass stage and
1265 after four passes. Resize replaced both histories with the current extent, and they
accumulated again through generations 3 and 5. No stale declaration-cache hits or Vulkan
validation IDs were reported.

The set of physical colour-attachment identities for spatial scratch matched between the
first and last read within each stage. The renderer reused the same backing even when logical
target names exchanged identities. The CPU convergence oracles independently checked DI
against analytic point-light sums and dense area-light quadrature, and GI against dense
hemisphere quadrature. Both temporal M-cap oracles passed without drift.

| Stage | Logical spatial targets in use | Verified cache hits | Compiles | Pool demand, B | Pool capacity, B |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1080p, one | 2 | 219 | 0 | 1,006,632,180 | 1,006,632,180 |
| 1080p, three | 4 | 191 | 2 | 1,106,164,980 | 1,106,164,980 |
| 1080p, one again | 2 | 215 | 2 | 1,006,632,180 | 1,006,632,180 |
| 1080p, zero | 0 | 220 | 2 | 940,276,980 | 1,139,342,580 |
| 1080p, two | 4 | 199 | 4 | 1,106,164,980 | 1,238,875,380 |
| 1080p, four | 4 | 184 | 0 | 1,106,164,980 | 1,238,875,380 |
| 4K, one | 2 | 144 | 4 | 4,026,531,604 | 4,026,531,604 |
| 1080p after resize, one | 2 | 184 | 2 | 1,006,632,180 | 1,006,632,180 |

The warm pool can retain inactive capacity according to its per-descriptor limits. The
fresh-process savings above quantify the backing required by the new graph shape. After
returning to 1080p the pool returned to its first one-pass demand and capacity.

### Replay

Use `olo_benchmark_capture` in the Release editor with `OLO_RG_COPY_LEDGER=1` and an idle
host. For the native timed ledger, set the selected manifest's `Output.Resolution` to
1920 × 1080 or 3840 × 2160 and set `Measurement.Frames` to 144 with the authored warmup and
camera unchanged. Retain `result.json`, `measurement.csv`, `measurement-passes.jsonl`,
the attachment images and `OloEngine.log` per run. Use the live memory report's VMA
allocations for Vulkan backing. Count the known bytes for the workload's graph executions.

For DI/GI, use a temporary copy of `IntegratedRenderer.olo` and point the manifest's
`Scene` at it. In `PostProcessSettings`, set `ReSTIRDIEnabled` and `ReSTIRGIEnabled`
to true, `ReSTIRDIInitialCandidates` to 2 and `ReSTIRDIEngagementMargin` to 0. Set both
`ReSTIR{DI,GI}SpatialReuse` and `ReSTIR{DI,GI}SpatialPasses` for the requested schedule.
The off control sets both enable fields to false. Compare matched fresh processes using
the same assets and alternate arm order. For the resize sequence, enable
`OLO_RG_VERIFY_DECLARATION_CACHE=1` and read the graph topology, declaration stats, memory
report and both ReSTIR stats after each transition.

## Representation checks

| Candidate | Theoretical saving at 1080p / 4K | Limiting case |
| --- | ---: | --- |
| Drop RT2 alpha by packing emissive into a 32-bit RGB format | 8,294,400 / 33,177,600 B per sample plane | Material flags have no remaining lane. Combining lit ClosureV2 code 2 and lit Legacy code 0 across an MSAA edge yields code 1, which denotes unlit. |
| Repack RT1 as 10:10:10:2 UNORM | 8,294,400 / 33,177,600 B per sample plane | The 2-bit AO lane has four values. AO 0.16 becomes 0 and 0.17 becomes 1/3. A 10-bit roughness lane maps 0.0003 to zero. |
| Replace the RT4 signed ID with a float32 lane | 0 / 0 B | The distinct IDs 16,777,216 and 16,777,217 collapse to the same float. |
| Store the existing ReSTIR encoding in RGBA16F | 82,944,000 / 331,776,000 B per five-plane history | DI punctual identity 2,049 rounds to 2,048, dropping the light kind. GI environment identity 4,098 rounds to 4,096, dropping the environment kind. A weight of 1e−8 rounds to zero and a moment of 1,000,000 overflows. |
| Halve both dimensions of the five-plane ReSTIR history | 124,416,000 / 497,664,000 B per effect | A 2×2 block containing both a near-mirror foreground and background has two depths and surface identities. One stored reservoir and validation record cannot preserve both independent histories. |

The identity examples use `(payload << 3) | kind` with independent IEEE binary16 conversion.
The DI example uses punctual kind 1 with payload 256. The GI example uses environment kind 2
with age 512. Values below half the smallest binary16 subnormal round to zero.
[Reservoir encoding](../../OloEngine/src/OloEngine/Renderer/ReSTIR/ReservoirCore.h)
