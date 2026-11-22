# Skin stack: per-pass GPU cost and memory budget (#1396)

The measurement #1222's delivery contract asks for ("per-pass cost and memory on named hardware"),
taken so that the run-to-run spread is smaller than the effect. It is a **budget to plan against**,
not a target the stack was tuned to meet.

**Hardware:** NVIDIA GeForce RTX 4090 (driver 621.14), Intel Core i7-14700KF, 64 GB, Windows 11.
**Build:** Release, clang-cl, `build-cached` (OloEngine-Tests for OpenGL, OloEditor for Vulkan).
**Resolution:** native 1920×1080. The 384×384 fixture of `DigitalHuman_Timing.txt` (one head, a few
per cent of the frame) is not re-measured: it cannot resolve the stack, which is why this exists.

## The workload

`Scenes/Benchmark/SkinDominant.olo`: 28 copies of the scanned head with `ReferenceHead.oloskin`
(transport version 3: diffusion, transmission, layered specular), no backdrop, one shadowed key, one
fill. Skin covers **71 %** of the 1080p frame at the `face-fill` camera and **61 %** at `crowd`
(all 28 faces). Three arms per raster path, each its own manifest:

| arm | manifest | what it isolates |
|---|---|---|
| full stack | `skin-dominant-<path>` | — |
| identical repeat | the same manifest, a second cell | the run-to-run noise floor |
| diffusion off | `skin-dominant-<path>-diffusion-off` (same scene file, `RendererSettings.SkinDiffusion: false`) | SkinDiffusionPass and its two snapshot copies |
| skin off | `skin-dominant-<path>-skin-off` (`SkinDominantNoSkin.olo`: every head on a Generic material) | the skin shading the G-Buffer and lighting passes carry |

Campaign `C:/perf/skin-1396-001`, 2026-11-21: 26 cells × 3 interleaved rounds of fresh processes,
**78 of 78 runs counted** (quiet host before and during each, pinned settings applied, every
full-stack run with valid SkinDiffusionPass samples). After the campaign, only the header comment of
`SkinDominantNoSkin.olo` was reworded; no entity changed, but the skin-off manifests' scene hash
moved with it. Each cell value is the range over the three
runs of that run's p50 over 300 measured frames per camera. A delta is the difference of the arms'
medians over runs, ± its noise floor: the largest of the two arms' run ranges, the identical
repeat's run range, and the full stack's difference from that repeat. One whose ± is not smaller
than the delta is marked *not resolved* and is not a budget number.

**The raw records stay out of git**, as every generated capture does
([generated-evidence-stays-out-of-git.md](../../../agent-rules/generated-evidence-stays-out-of-git.md)):
`C:/perf/skin-1396-001` on the measuring box holds `plan.json`, `provenance.json`, `runs.json` and each
counted run's `measurement.csv` and `measurement-passes.jsonl`. The binaries were built from commit
`0bf756e85` (the working tree differed only in `EditorPreferences.yaml`'s throttle, which the Vulkan
host requires): test binary sha256 `328f0af5…`, editor `2d85ecac…`. The tables are
`scripts/perf/skin-stack-cost-report.py` over that directory; a re-run produces new observations to
compare against these, not the same samples.

## Per-pass GPU cost

### OpenGL (OloEngine-Tests capture host)

| path | camera | SkinDiffusionPass | snapshot copies | frame GPU | diffusion: on − off | identical arms: on − repeat |
|---|---|---:|---:|---:|---:|---:|
| Forward | face-fill | 0.187–0.188 | 0.071–0.072 | 2.303–2.314 | +0.272 ± 0.012 | +0.001 ± 0.011 |
| Forward | crowd | 0.165–0.166 | 0.072–0.073 | 2.977–2.989 | +0.261 ± 0.012 | +0.006 ± 0.012 |
| Forward+ | face-fill | 0.187 | 0.072 | 2.328–2.340 | +0.266 ± 0.012 | −0.007 ± 0.012 |
| Forward+ | crowd | 0.165–0.166 | 0.072–0.073 | 3.007–3.021 | +0.247 ± 0.019 | −0.005 ± 0.013 |
| Deferred | face-fill | 0.187–0.188 | 0.072 | 2.553–2.564 | +0.267 ± 0.016 | −0.004 ± 0.016 |
| Deferred | crowd | 0.167 | 0.073 | 2.755–2.770 | +0.240 ± 0.017 | −0.004 ± 0.017 |

Skin shading in the passes it rides (full stack − skin off, ms of per-pass p50):

| path | camera | ScenePrepassPass | ScenePass | DeferredLightingPass |
|---|---|---:|---:|---:|
| Forward | face-fill | +0.000 ± 0.002 (not resolved) | +0.103 ± 0.006 | — |
| Forward | crowd | +0.001 ± 0.004 (not resolved) | +0.180 ± 0.008 | — |
| Forward+ | face-fill | +0.000 ± 0.002 (not resolved) | +0.092 ± 0.005 | — |
| Forward+ | crowd | +0.001 ± 0.001 (not resolved) | +0.165 ± 0.006 | — |
| Deferred | face-fill | — | +0.004 ± 0.004 (not resolved) | +0.035 ± 0.004 |
| Deferred | crowd | — | +0.015 ± 0.006 | +0.036 ± 0.002 |

Deferred, upscale Quality (0.67 render scale): SkinDiffusionPass 0.095 (face-fill) and 0.084
(crowd) ms, half the native cost, as the pixel count predicts (0.67² ≈ 0.45).

### Vulkan (OloEditor `--rhi=vulkan` capture host)

| path | camera | SkinDiffusionPass | snapshot copies | frame GPU | diffusion: on − off | identical arms: on − repeat |
|---|---|---:|---:|---:|---:|---:|
| Forward | face-fill | 0.349–0.372 | 0.057–0.058 | 2.304–2.402 | +0.348 ± 0.141 | +0.030 ± 0.098 |
| Forward | crowd | 0.318–0.337 | 0.057–0.058 | 2.467–2.538 | +0.079 ± 0.651 (not resolved) | −0.019 ± 0.072 |
| Forward+ | face-fill | 0.330–0.370 | 0.056–0.058 | 2.298–2.439 | +0.218 ± 0.152 | −0.016 ± 0.152 |
| Forward+ | crowd | 0.309–0.342 | 0.057 | 2.515–2.633 | +0.080 ± 0.235 (not resolved) | +0.042 ± 0.118 |
| Deferred | face-fill | 0.321–0.334 | 0.060–0.061 | 2.434–2.498 | +0.318 ± 0.079 | +0.014 ± 0.072 |
| Deferred | crowd | 0.309–0.312 | 0.062–0.063 | 2.576–2.602 | +0.140 ± 0.049 | +0.028 ± 0.035 |

Deferred, upscale Quality: SkinDiffusionPass 0.214–0.217 (face-fill) and 0.208–0.213 (crowd) ms.

The **skin-shading deltas on Vulkan are not resolved**: every ScenePrepassPass, ScenePass and
DeferredLightingPass delta is within ±0.05 ms with a run spread of 0.003–0.18 ms, and the two whose
spread is narrower come out negative (Forward+ crowd, −0.010 and −0.049), which skin cannot be. The
editor host's per-pass noise (run spreads up to 0.18 ms) is larger than that cost, so this campaign
gives **no Vulkan figure and no upper bound** for it: a delta inside the noise does not bound the
cost. The resolved measurement of the skin shading is OpenGL's.

## The budget

Native 1080p, RTX 4090, the frame made of skin (61–71 % coverage). Per-pass p50s:

| item | OpenGL | Vulkan |
|---|---:|---:|
| SkinDiffusionPass | 0.17–0.19 ms | 0.31–0.37 ms |
| its two snapshot copies | 0.07 ms | 0.06 ms |
| the diffusion's cost to the frame (on − off, resolved cells) | 0.24–0.27 ms | 0.14–0.35 ms |
| skin shading in the forward scene pass | 0.09–0.18 ms | not resolved (run spread up to 0.04 ms) |
| skin shading in deferred lighting | 0.035 ms | not resolved (run spread up to 0.18 ms) |
| skin-stack memory, skin on screen | 55.4 MiB | 55.4 MiB |

Per-pass cost scales with skin pixels (face-fill vs crowd, the upscale cell), so a frame with less
skin pays proportionally less. A scene that has resolved no diffusing profile since its load pays
only the hand-off attachment below; once one has held a slot, the diffusion's targets and copies
stay until the scene reloads (the gate is sticky, `SkinProfileTable::AnyAssignedSlotDiffuses`).

Vulkan's SkinDiffusionPass is about 1.8× OpenGL's for the same kernel and pixels. Its cause is not
established here.

**The harness's own verdict is FAIL**, and not because of skin: one run of
`gl-forward-plus-diffusion-off` had a `face-fill` p95 **wall-clock** frame time of 23.7 ms against
the acceptance harness's 16.667 ms gate (a CPU-side hitch in one fresh process; its GPU timings are
in line with the other two runs). Every other cell passes the gate in every run. The gate is #1259's,
not part of this budget.

## Memory

From the renderer memory report (`result.json` → `memory.capacity`, the same rows the editor's memory
window and `olo_memory_report` read), identical on both backends because the rows are format
estimates of what the graph declares. `SkinMemoryEvidence` pins every row byte for byte against its
prediction, in the GL and the Vulkan test process, and at a render scale and under 4× MSAA.

| row (owner / category) | bytes at 1080p | held when |
|---|---:|---|
| RenderGraph / Skin hand-off (SceneColor attachment 4, RGBA16F) | 16 588 800 | **every scene**, skin or not |
| SkinDiffusion / diffusion scratch (RGBA16F) | 16 588 800 | a diffusing profile holds a slot |
| SkinDiffusion / hand-off + depth snapshots (RGBA16F + D24S8) | 24 883 200 | the same |
| SkinDiffusion / kernel UBO | 6 272 | always (allocated with the pass) |
| SkinProfileTable / deferred profile table (slice of the lighting controls UBO) | 1 200 | always; demanded on Deferred |
| **total with skin on screen** | **58 068 272** (55.4 MiB) | |
| **total in a scene with no diffusing profile since its load** | **16 596 272** (15.8 MiB) | the hand-off attachment and the two buffers; removing skin later keeps the targets until a reload |

The scratch and the snapshots are render-graph transients, so their bytes are inside the
TransientPool row and may share an aliased slot with a transient outside skin's lifetime; the rows say
what skin is responsible for, not extra memory on top. They follow the scene band, so an upscale
shrinks them by the render scale squared, and they are single-sample, so MSAA adds nothing.

The hand-off attachment is the one cost every scene pays. It is attachment 4 of the SceneColor MRT,
written by every scene shader (zero where there is no skin) and loaded with SceneColor by
`ParticlePass` and `GPUDrivenOcclusionPass`; [skin-diffusion.md](../../../guides/skin-diffusion.md)
says why it is an attachment rather than a per-draw draw-buffer switch.

Before this issue the scratch and the snapshots were **also** paid in every scene without skin, along
with their two full-screen snapshot copies: the diffusion's gate did not look at whether any profile
diffused. That is fixed in the same PR; `SkinMemoryEvidence.ASceneWithoutSkinDemandsNothingOnAnySkinRow`
read 491 520 + 737 280 bytes at 320×192 before the fix and reads 0 after it.

## Re-run procedure

On an otherwise idle Windows box with the GPU above:

1. Build Release: `pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command 'cmake --build build-cached --target OloEngine-Tests OloEditor --config Release --parallel 6'`.
2. In `OloEditor/SandboxProject/EditorPreferences.yaml` set `ThrottleEditMode: false` (the Vulkan
   host refuses to run otherwise; see [controlled-performance.md](../../../guides/controlled-performance.md)).
   Restore it afterwards.
3. Close anything that uses the GPU, other OloEngine binaries and games (the driver waits for a quiet
   host and refuses contended runs, naming the process).
4. `python scripts/perf/acceptance-benchmark.py --plan scripts/perf/skin-stack-cost-plan.json --output C:/perf/<name>`
   (27 minutes on this box; three interleaved rounds of fresh processes, the start cell
   rotating per round).
5. `python scripts/perf/skin-stack-cost-report.py C:/perf/<name>` prints the tables above from the
   counted runs.

The scene and manifests are generated: `python scripts/perf/generate-skin-dominant-benchmark.py`
(`--check` fails if the committed copies are stale).
