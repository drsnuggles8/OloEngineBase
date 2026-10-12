# Foliage rebuild stalls (#1582)

Warm placement and plant-ID reconciliation now run on one CPU worker while the renderer keeps
drawing the complete published generation. Fresh instance/cull buffers share a 2 MiB/frame transfer
budget and publish together at the pre-render boundary. Newest requests supersede old work;
failure, disable and teardown prevent stale publication. GPU resources stay on their owner thread;
large CPU results retire on workers. Streaming representation changes refresh metadata without
regenerating placements.

Whole-generation publication avoids mixing different authored layer states. Its cost is delayed
visibility and temporarily retaining both generations. Cold generation, height/extent/resolution
changes and global wind-envelope changes remain synchronous so roots follow already-changed ground.
Resource preparation is one layer/frame, not a time-bounded texture operation.

## Matched measurements

Release with symbols, clang-cl, i7-14700KF / RTX 4090 / 64 GiB, NVIDIA 617.42. The committed
`FoliageMeadowToWoodland.olo` retains 491,773 plants. Fresh uncontended processes use native
1920x1080, GL Forward/CSM, traversal Step4, 600 steady frames and 600 frames with regeneration
every 100. Two preserved control runs and three final runs have clean host/contention records.

| Metric | Control runs | Final run 1 | Final run 2 | Final run 3 |
|---|---|---:|---:|---:|
| Steady wall p50 (ms) | 15.70–15.71 | 14.31 | 14.29 | 14.33 |
| Steady wall p95 (ms) | 16.37–16.53 | 19.78 | 18.56 | 18.58 |
| Steady wall p99 (ms) | 17.03–17.20 | 26.61 | 19.82 | 19.55 |
| Steady wall maximum (ms) | 17.83–18.65 | 38.10 | 24.24 | 24.45 |
| Rebuild-window maximum (ms) | 519.39–544.25 | 22.76 | 20.62 | 20.79 |
| Largest request frame (ms) | synchronous rebuild | 18.43 | 14.53 | 18.40 |
| Largest publication frame (ms) | synchronous rebuild | 18.62 | 15.13 | 15.57 |
| Latest-edit latency (ms) | synchronous rebuild | 751–861 | 746–963 | 743–830 |

All five replacements publish in each final window. Control median stages are registry reconcile
332–336 ms, placement 133 ms, instance upload 5.2 ms, bounds 5.6 ms and cull preparation 14–15 ms;
unchanged geometry, impostors and alpha are cache hits. Final CPU work remains: median registry
276–333 ms, placement 133–136 ms, bounds 5.36–5.47 ms, cull preparation 15–16 ms. It is spread over
worker and owner work instead of one frame. Owner snapshots take 0.47–0.59 ms; staged instance
uploads total 4.56–4.88 ms per replacement.

Final steady GPU variation is greater than the prototype's; no outlier was filtered. These results
support removal of the large rebuild stall, not universally sub-16.67 ms frames or a precise steady
improvement. The bounds repair also changes conservative culling. All 600 warm GPU samples resolve
per run; one steady GPU sample in run 1 is missing and is not zero-filled.

Known staging arrays peak at 85.277 MiB, excluding map nodes/allocator overhead. Tracked physical
GPU residency peaks 24.419 MiB above window start. Whole-process commit, sampled once per rendered
frame, peaks at 3123.89–3133.41 MiB; it is not a dedicated foliage allocation high-water total.

## Other paths and remaining costs

One further uncontended GL process measures every path/shadow pair, with 600 steady and 600
rebuild frames per pair. All five requests publish in every cell.

| GL path / shadow | Steady p50/p95/p99/max (ms) | Rebuild p50/p95/p99/max (ms) | Edit latency (ms) |
|---|---|---|---|
| Forward / CSM | 13.26/17.27/18.35/22.34 | 13.29/17.76/18.79/22.23 | 744–772 |
| Forward / VSM | 29.62/34.04/36.04/40.61 | 30.06/35.53/37.95/42.69 | 1097–1202 |
| Forward+ / CSM | 13.43/17.90/19.29/22.52 | 13.42/18.09/19.67/20.58 | 722–815 |
| Forward+ / VSM | 30.05/34.73/38.64/41.47 | 29.65/34.15/35.58/39.37 | 1093–1176 |
| Deferred / CSM | 12.84/13.50/14.47/17.61 | 12.82/13.60/14.71/16.08 | 707–722 |
| Deferred / VSM | 17.99/19.08/19.91/22.41 | 18.03/19.36/20.52/21.48 | 797–876 |

The final Vulkan editor uses natural frames, throttles/frame cap off and a continuous foreign-GPU
watch. Each cell fills a 1,024-frame profiler ring after 1,100 frames, then issues five rebuilds
separated by 100 frames and waits 100 more plus completion. Every cell publishes all five with
491,773 plants before/after. Edited rings include preceding same-cell steady frames; this is not
the GL fixture's 600-frame protocol. Profiler wall times include live application pacing.

| Vulkan path / shadow | Steady p50/p95/p99/max (ms) | Edited p50/p95/p99/max (ms) |
|---|---|---|
| Forward / CSM | 13.25/13.60/13.80/14.47 | 13.22/13.59/13.89/16.28 |
| Forward / VSM | 15.77/16.19/16.35/16.55 | 15.77/16.18/16.37/16.75 |
| Forward+ / CSM | 13.21/13.55/13.71/14.01 | 13.21/13.56/13.68/13.95 |
| Forward+ / VSM | 15.75/16.15/16.36/16.68 | 15.75/16.09/16.27/16.51 |
| Deferred / CSM | 13.06/13.42/13.64/19.21 | 13.05/13.37/13.57/13.94 |
| Deferred / VSM | 15.43/15.87/16.11/18.93 | 15.42/15.79/16.04/16.40 |

Residuals are measured separately; none uses the cached warm result as a substitute:

- Cold enable remains synchronous: ten samples per final matched process, median 373.73–449.57 ms,
  maximum 453.41–599.18 ms.
- Alternating every layer's cutoff between its original value and original +0.02 reaches
  117.60–121.32 ms request frames, 13.86–15.27 ms publication frames and 1280–1316 ms edit latency.
  Private alpha snapshot/cloning/mips total 621–630 ms across resource-layer frames; impostor work
  totals about 3 ms. Extra tracked GPU residency reaches 106.46 MiB. The 2 MiB transfer budget
  covers instance/cull data, not texture mips.
- Adding 0.0001 to each normalized height every 100 frames keeps roots attached synchronously,
  with identical request/publication frames of 427.34–506.11 ms. `SetHeights` runs outside the
  fixture's wall timer, so height-upload API cost is excluded. This measures foliage following
  a height update, not full sculpt-tool latency.

## Verification and provenance

Final Release passes 34 CPU/automation, 13 real-pipeline GPU and four loose/staged-content tests,
with no skips. Earlier 34 Debug checks passed before final source/history/upload additions. The
locked build completed and checked dependencies for 2,072 objects; pre-commit passes.

- Owned/deterministic snapshots, ID survival/retirement, cancellation, bounds composition,
  streaming payloads, cull sizing and MCP write consent are pinned by CPU contracts.
- GPU regressions cover whole-generation/newest-edit publication, disable, optional-detail
  fallback, destruction, cull-mode changes, runtime tick, height/mask supersession, private alpha
  base/every-mip pixels, gray albedos and checked-upload rejection without corrupting backing.
- Terrain/mask revisions invalidate without a foliage dirty flag. Height changes supersede pending
  work and attach roots; mask paint matches an independent placement oracle. Accepted reloads
  refresh the initialized CPU mirror; differently sized reloads explicitly retain the old plane.
- Authoring undo/redo retains the live renderer; fresh scene copies start without one. Real
  Deferred TAA/SSR/SSGI history ages prove no-op preservation and reset at changed publication;
  unscoped explicit reset includes FSR2 private history.
- The earlier expanded matrix covers 144 GL spatial + 144 Vulkan spatial + 144 GL temporal-request
  cells: near/overview/far, CSM/VSM, 1920x1080/1281x721, upscale Off/Quality/Performance and supported
  sample counts. Every replacement retains 491,773 plants. All 25 inspection sheets were viewed.
  This is earlier-build evidence, before final source/alpha/history/upload corrections.
- The final editor repeats 18 lifecycle cells across all three paths and CSM/VSM: GL/Vulkan spatial
  with MSAA4 requested, plus GL MSAA1 FSR2 Quality. Before/during/after, cold disable-enable,
  moving interaction and full Play/Stop regeneration pass. All final panels and selected native
  frames were inspected. Forward/Forward+ actually retain one sample; Deferred actually runs four.
  GL Deferred MSAA4 refuses temporal upscaling; Vulkan temporal upscaling is unsupported. Dynamic
  Deferred scale remains #1537. Broader odd-resolution/Performance/three-camera proof is explicitly
  inherited; all six native path/shadow timing cells run on final source.
- All-path near/far GL loose/staged pixel parity includes a planted missing-mesh negative control
  and stable-ID cutoff replacements. Actual final OloRuntime apps render on GL/Vulkan from a staged
  game cwd with Mono/shaders/resources. This path-based flora layout has no asset pack; actual app
  defaults Forward and exposes no path selector. Actual-app Forward+/Deferred are unreachable
  through that front door; fixture and editor Play coverage are separately identified.
- Final logs have no shader/SPIR-V failures, GL errors, VUIDs or validation errors. Release
  `olo_shader_errors` is unavailable, so its null count is not zero. Counted Vulkan cull-stream
  startup/camera-jump growth warnings and inactive runtime BrushPreview null uniforms are retained.
- Authored serialized fields, scene/save-game/asset-pack formats and C#/Lua entry points do not
  change. Terrain transforms continue through the existing owner update; physics is not moved.

Final test SHA256: `f202f3ac29ad8cce6f2fed83a68b2a8e74a25649a9f19651b33b5b2bce080603`.
Final editor: `a22291d65d1da49333a92475e130991585d59b362a44059dee7540e283835dbd`.
Final runtime: `2f55820e72da3c10dfdab578c2769ec4f6490b37cd8c2ccacf1a7add05bcbb6a`.
Recorded production source is `a1eb7d038`; later commits contain documentation only.

Raw samples, host records, XML, logs, hashes and captures are **local, ignored evidence**, under
`.v-build/pr-evidence-retained/`, with full protocol notes in `.v-build/pr-evidence-report-full.md`.
Named subdirectories: `control-gl-forward-csm-step4`, `final-matched-step4`,
`final-gl-allpaths-step4`, `final-cutoff-step4`, `final-height-step4`, `final-vulkan-tails-step4`,
`checks`, `live-matrix` and `final-live`. They are deliberately excluded from the PR and its history.

## Existing RT budget refusal

Six RT-enabled Vulkan near/overview/far CSM/VSM cells retain coherent canonical/GPU-scene census,
but default vegetation budgets withhold the shadow TLAS. Step8 plans 239 casting groups and refuses
173; admitted `castingDemandVertices=992031` is not the full representation cost. The preserved
synchronous control has the same refusals, demand, withheld TLAS and 491,773 plants. Raster fallback
is active; these are not successful RT shadow-execution cells. Existing [#1578](https://github.com/drsnuggles8/OloEngineBase/issues/1578)
needs an owner decision about budgets, grouping or error/representation-quality bounds. This change
preserves the complete-representation policy.

## Side repairs and review

Separate commits repair sampler child-PID isolation, repeatedly expanded bounds, authoring undo,
terrain/mask source observation and temporal history at publication. Each has a regression; the
sampler also passes all 73 performance-tool tests. Standards and Spec reviews found no remaining
production issues. GitHub CI/review is a separate gate from this local proof.
