# Measure the complete verification workload

Use Release with symbols for representative live verification, and retain targeted
Debug checks for assertions and Debug-specific behavior. Preserve the scene,
resolution, rendering path, warmup, and required simulation durations. Release
reduces CPU overhead; it cannot remove a GPU-heavy shadow workload.

## September 2026 workstation attribution (#1488)

On the Windows RTX 4090 workstation, clang-cl 23.1.0 builds containing
`b034075ba` and merged master `7c5aa1b98` ran the complete
Debug/Release x OpenGL/Vulkan x Forward/Forward+/Deferred x
DriftMenu/IntegratedRenderer matrix at 1920x1080. Edit/play throttling and autosave
were disabled, with no frame-rate cap. Each cell has three blocks of five live
snapshots, with active-path readback before sampling and after each block.
All 360 GPU snapshots were valid. An exclusive build gate and sampled process
monitor excluded known competing engine/build processes; neither certifies the
absence of all background activity.

These are descriptive block medians, not a randomized configuration comparison
or a regression gate. In particular, the faster Debug values in some dense cells
do not establish a Debug advantage. Vulkan presentation settings and validation
also differ from OpenGL and between configurations. Do not turn these numbers
into universal CI thresholds.

| Configuration/backend | Scene/path | Frame ms | GPU ms | Shadow GPU ms |
|---|---|---:|---:|---:|
| Debug/opengl | DriftMenu/forward | 3.11 | 0.89 | n/a |
| Debug/opengl | DriftMenu/forwardplus | 3.03 | 0.88 | n/a |
| Debug/opengl | DriftMenu/deferred | 3.30 | 0.88 | n/a |
| Debug/opengl | IntegratedRenderer/forward | 363.03 | 362.68 | 328.81 |
| Debug/opengl | IntegratedRenderer/forwardplus | 366.73 | 366.88 | 332.35 |
| Debug/opengl | IntegratedRenderer/deferred | 365.07 | 364.94 | 342.81 |
| Debug/vulkan | DriftMenu/forward | 16.77 | 3.78 | n/a |
| Debug/vulkan | DriftMenu/forwardplus | 16.57 | 6.05 | n/a |
| Debug/vulkan | DriftMenu/deferred | 16.74 | 4.15 | n/a |
| Debug/vulkan | IntegratedRenderer/forward | 332.61 | 332.51 | 308.93 |
| Debug/vulkan | IntegratedRenderer/forwardplus | 381.25 | 381.56 | 355.54 |
| Debug/vulkan | IntegratedRenderer/deferred | 413.39 | 344.91 | 321.88 |
| Release/opengl | DriftMenu/forward | 1.38 | 0.56 | n/a |
| Release/opengl | DriftMenu/forwardplus | 1.22 | 0.56 | n/a |
| Release/opengl | DriftMenu/deferred | 1.15 | 0.69 | n/a |
| Release/opengl | IntegratedRenderer/forward | 394.06 | 394.37 | 358.54 |
| Release/opengl | IntegratedRenderer/forwardplus | 410.71 | 410.13 | 373.35 |
| Release/opengl | IntegratedRenderer/deferred | 351.70 | 352.12 | 330.38 |
| Release/vulkan | DriftMenu/forward | 16.66 | 2.93 | n/a |
| Release/vulkan | DriftMenu/forwardplus | 16.52 | 4.44 | n/a |
| Release/vulkan | DriftMenu/deferred | 30.46 | 3.40 | n/a |
| Release/vulkan | IntegratedRenderer/forward | 356.13 | 356.16 | 331.02 |
| Release/vulkan | IntegratedRenderer/forwardplus | 378.75 | 378.76 | 352.58 |
| Release/vulkan | IntegratedRenderer/deferred | 365.21 | 365.23 | 340.63 |

ShadowPass is the largest GPU parent pass in every dense cell (309–373 ms).
OpenGL dense CPU medians were 14.2–15.0 ms in Debug and 3.1–3.3 ms in Release;
its frame interval remained close to GPU time. Debug/Vulkan Deferred is a
qualification: CPU median 353.1 ms and GPU 344.9 ms against a 413.4 ms frame.
The corresponding Release cell recorded CPU 44.1 ms and GPU 365.2 ms. Both CPU
and GPU work matter there. The scope is verification guidance, not removing the
scene's workload or promising an across-the-board frame-rate increase.

WPR sampling was unavailable because the host denied profiling privileges.
Thirty-second Tracy captures provided instrumented main-thread elapsed self time
instead. This includes waits and is not CPU execution time:

| Configuration/backend | Main-thread zone | Self seconds / calls | Mean self ms |
|---|---|---:|---:|
| Debug/opengl | `virtual OloEngine::OpenGLRendererAPI::ClientWaitFence` | 28.061 / 81 | 346.432 |
| Debug/opengl | `OceanFFTField::PhysicsProxyEvaluate` | 0.703 / 82 | 8.567 |
| Debug/vulkan | `OloEngine::Scene::ProcessScene3DSharedLogic` | 18.082 / 76 | 237.927 |
| Debug/vulkan | `OloEngine::Renderer3D::EndScene` | 5.293 / 77 | 68.736 |
| Release/opengl | `virtual OloEngine::OpenGLRendererAPI::ClientWaitFence` | 29.746 / 84 | 354.118 |
| Release/opengl | `OceanFFTField::PhysicsProxyEvaluate` | 0.077 / 85 | 0.912 |
| Release/vulkan | `virtual OloEngine::VulkanContext::SwapBuffers` | 26.051 / 87 | 299.439 |
| Release/vulkan | `OloEngine::Scene::ProcessScene3DSharedLogic` | 2.394 / 88 | 27.207 |

Vulkan SwapBuffers includes command recording, submission and waiting. The
Debug/Vulkan trace attributes large elapsed time to shared scene logic and
EndScene; this does not isolate a particular function's CPU instruction cost.
OpenGL mostly waited on the GPU. Its OceanFFT physics proxy averaged 8.57 ms in
Debug and 0.91 ms in Release in those traces, below the shadow bottleneck.

## Capture and log evidence

Four Release captures retained all three finite camera sequences (stationary,
dolly, rapid-turn), 180 requested motion steps each: Vulkan on all three paths,
and OpenGL Deferred. Every camera exported 360 contiguous completed CPU-frame
intervals and 360 valid GPU samples, with no attachment failures, exclusions or
warmup timeout. Wall times were 512.3/469.5/468.3 seconds for Vulkan F/F+/D and
489.6 seconds for OpenGL D. All 12 Beauty images and all 24 editor screenshots
were inspected. The rapid-turn endpoint intentionally looks away from the herd;
the diagnostic unbound grooms are not an art-quality reference.

OpenGL's reflection AO/depth binding leak was reproduced with a failing real-GPU
test, fixed in `b034075ba`, then checked with the on/off reflection test in both
configurations. The final live GL logs contain no planar-pass GLStateGuard
warnings. See [the binding restoration guide](planar-reflection-ao-restoration.md).

Debug/Vulkan logged ten unused-fragment-output warnings at the menu's Deferred
transition: `PBR_MultiLight` locations 3 (`o_Velocity`) and 4 (`o_SkinDiffuse`)
had no matching color attachment. These writes are discarded by the
[Vulkan fragment-output contract](https://docs.vulkan.org/spec/latest/chapters/interfaces.html#interfaces-fragmentoutput).
The warnings remain in the evidence; this is not a zero-warning claim.

Dense Vulkan also reported refused ray-tracing builds. The retained post-capture
scene diagnostic showed vegetation at its roughly 64 MiB geometry budget:
642 dispatched, 1,317 refused out of 1,959 requests, and readiness false.
Grooms were complete (41 represented, none refused). The scene used shadow maps;
RT shadows were disabled. This is capacity-limited, fail-closed RT coverage,
not proof that all vegetation was ray traceable.

Raw evidence is retained under the task worktree's
`build-cached/live-evidence/`: `attribution-summary.json` records block ranges
and parent-pass rankings; `<Config>-<backend>/<Scene>-<path>.json` contains the
snapshots and path readbacks; each configuration/backend folder retains binary
hashes, source-diff hashes, logs, host monitoring and CPU traces. Capture folders
contain reader validation; `visual-inspection.json` records viewed images.
Earlier invalid query-generation and mislabeled Forward+ captures are preserved
separately and excluded from these results. The binaries were built before the
test-only packed-groom frame-count instrumentation; source and binary provenance
are recorded separately rather than inferred from a clean checkout.

## Shorten only the packed-asset smoke check

Keep stride, integration and temporal-convergence durations in their dedicated tests.
`LooseAndPackedGroomsAreTheAssetsThatWereCooked` checks cooked asset identity and
attachment, so its packed runtime now uses eight frames: two reseed frames and six
integrating frames. All three subjects must still draw and deform, with zero binding
refusals and more than 1% packed-coat coverage. The actual frame count is emitted as
`packed_runtime_frames` in GoogleTest XML.

Three alternating AB/BA fresh-process pairs per configuration compared the preserved
45-frame executable with the eight-frame candidate. All 12 runs passed without skips.
Process wall times in seconds (median and range):

| Configuration | 45 frames | 8 frames |
|---|---:|---:|
| Release | 23.228 [23.219, 26.011] | 20.699 [20.504, 20.701] |
| Debug | 204.197 [204.054, 204.934] | 171.310 [170.913, 171.425] |

These are descriptive local measurements, not a universal CI threshold. The single
full baseline per configuration passed all ten acceptance cases plus frame-rate
invariance and over-budget logging checks. Acceptance process times were 521.139 s
Release and 5,854.534 s Debug; this is not a paired whole-suite speedup claim.
The 60-frame integration run, full stride, 24-frame resolve, 30/60/144 Hz comparison
and adaptive near/far convergence remain unchanged. The existing near/far test
reported ShortCoat energy as unmeasurable; its energy oracle is not claimed as passed.

Every comparison run produced fresh ShortCoat, LongCoat and Human PNGs. Viewed
representative A/B images retain coats and hair, with the expected earlier walk/head
pose at eight frames. Both arms retain noisy strands and sparse scalp coverage;
these images establish functional attachment, not art quality or convergence.
The three committed `GroomAnimalsPacked_GL_Forward_*.png` images are the final
Debug candidate captures. Raw XML, image hashes and executable hashes are retained
under `build-cached/live-evidence/groom-frame-comparison/`; its host monitor sampled
known competing processes every five seconds with no contention observed.
