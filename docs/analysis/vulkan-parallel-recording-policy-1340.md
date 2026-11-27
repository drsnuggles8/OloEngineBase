# Vulkan parallel recording: the measured fork policy (#1340)

Fork a recording region only when its measured work repays the fork, and cap shaded replay at
three items. The fixed rule it replaces (fork any bucket replay that splits into two 32-packet
items, up to 16 items; fork every multi-view shadow region) lost time on small scenes and, on
large ones, left most of the shaded-replay gain on the table, because shaded items contend.

## Method

RTX 4090, Vulkan, clang-cl Release, live editor at 1920 x 1080, Deferred, VSM off, MCP
`olo_perf_pass_timings`. Every arm runs in the same process and binary; only console variables
change, so the arms are interleaved in rotated order (ABC, BCA, CAB, twice), 60 warm frames and
40 distinct sampled frames a block. Block values are medians; a cell is the median of its block
medians.

| arm | levers |
|---|---|
| off | `OLO_VK_PARALLEL_RECORDING=0` |
| legacy | `OLO_VK_PARALLEL_MIN_PACKETS=64`, `_PACKETS_PER_ITEM=32`, `_MAX_ITEMS=16`, `_SHADOW_MIN_WORK=0` (the pre-#1340 rule) |
| policy | all unset (the shipped defaults) |

`scripts/perf/parallel-recording-policy-live.py` drives it and records every other engine or
compiler process seen during each scene. **The host was shared**: sibling worktrees were
compiling and another test binary used the GPU in every run. The untouched `BloomPass` CPU time
is the drift control; it stayed within ±0.004 ms in most cells and is quoted where it did not.
Regions with identical decisions in two arms are a second control: their difference is drift.

Scenes: `ParallelRecording.olo` (dense: 3,600 mesh windows) at its authored pose and three
narrower poses that cull it to ~193, ~451 and ~822 scene packets; `MaterialLab.olo` (small);
`Courtyard.olo` and `IntegratedRenderer.olo` (mixed), each at its benchmark manifest camera.

## Scene replay: threshold and item cap

Region wall time, ms (final interleaved run, `final-ab.json`):

| scene packets | depth off | depth legacy | depth policy | colour off | colour legacy | colour policy |
|---:|---:|---:|---:|---:|---:|---:|
| 110 (Courtyard) | 0.191 | 0.208 | 0.189 (inline) | 0.225 | 0.225 | 0.228 (inline) |
| 117 (MaterialLab) | 0.133 | 0.134 | 0.142 (inline) | 0.229 | 0.189 | 0.234 (inline) |
| 193 | 0.214 | 0.258 | **0.209** | 0.358 | 0.377 | **0.274** |
| 451 | 0.482 | 0.375 | **0.358** | 0.837 | 0.865 | **0.561** |
| 822 | 0.847 | 0.498 | **0.494** | 1.477 | 1.512 | **0.843** |
| 1,580 | 1.959 | 0.834 | **0.792** | 3.101 | 2.625 | **2.167** |

The earlier off/legacy reproduction showed both families losing at 110-117 packets (depth
0.21 -> 0.30-0.35 ms) and both winning at 193. **Shaded replay contends**: summed worker time
grows faster than the item count (1,580 packets: 32.8 ms of worker time at 16 items for 3.1 ms
of inline work; 5.2 ms at 3 items). An item-cap sweep at 451/822/1,580 packets put colour's
optimum at three items at every size; depth kept scaling to about eight. So:

- every family forks at **192 packets**, in items of at least 64;
- depth-only replay (`SceneDepth`) takes up to **8** items, shaded families **3**;
- families no measured scene took above the threshold (foliage, water, overlay, decal,
  occlusion, generic) take the conservative shaded policy, not a guess at a better one.

The contention's source is not identified here: the per-region `pipelineLookupMs` probe
(`OLO_VK_RECORDING_COSTS=1`) attributes the lookup share, see [Observability](#observability).

## Shadow regions: a visible-work estimate

A shadow region's items cull their own casters, so submitted-caster counts do not predict the
work: MaterialLab's 464 view x caster pairs lose when forked (most are culled per cascade) while
Courtyard's 428 win. The pass now counts, on the render thread before the fork, the view x
caster pairs that survive the same CPU frustum test the items run (mesh and skinned casters;
other kinds once per view), stopping at 4,096 pairs or 16,384 tests. Visible pairs measured:
MaterialLab 218; IntegratedRenderer 20-176; Courtyard 137-428; dense >= 4,096.

Totals of all recording regions, ms (`shadow-ab.json`):

| scene | off | legacy | policy (256 pairs) | policy control drift |
|---|---:|---:|---:|---|
| MaterialLab | 0.522 | 0.674 | **0.523** | Bloom 0.0398 vs 0.0400 |
| Courtyard | 4.360 | 5.366 | **4.026** | legacy blocks contended (Bloom +50%) |
| IntegratedRenderer | 1.485 | 1.811 | 1.579 | every region inline; the 0.09 is drift on identical work |
| Dense | 31.752 | 18.717 | 19.012 | Bloom +70% in policy blocks |

At 256 pairs the small scenes lose nothing to forking, the dense scene keeps its gain, and
Courtyard forks only its largest region. Courtyard's ten 137-247-pair regions each won about
0.05 ms forked; keeping them inline is the conservative side of a threshold that also has to
keep MaterialLab's 218 pairs inline.

## Quality

G-buffer albedo, normal and scene depth captured per arm (`--capture`) are byte-identical
off vs legacy vs policy on every deterministic scene (dense at four poses, MaterialLab,
Courtyard). IntegratedRenderer is animated: two captures of the off arm differ by 1.1 M pixels,
as much as the arms differ, so its comparison is at the scene's own noise floor.

## Observability

`olo_perf_pass_timings` region entries now carry `decision` (`parallel`, `inlineBelowThreshold`,
`inlineRejectedWork`, `inlineSingleItem`, `inlineLeverOff`, `inlineNested`, `inlineUnsupported`,
`inlineNoSecondary`), `family`, `workUnits`, `minWorkUnitsToFork` and `itemCount`, beside the
existing `regionWallMs` (elapsed) and `workerRecordMs` (summed worker CPU). With
`OLO_VK_RECORDING_COSTS=1` they add the preparation and contention probes
(`frontendPrepareMs`, `selectionSeedMs`, `attachmentPrepareMs`, `sampledImagePrepareMs`,
`pipelineLookupMs`).

## Inline paths that stay inline

Unchanged and still tested: a nested region, an open occlusion query or conditional-render
span, recording onto the async compute queue, no begun frame, a secondary the pool cannot hand
out, the lever off and a rejected packet each keep the region inline, now with their own
`decision` value. Capture hooks still select sequential graph execution.

## Reproduce

```
python scripts/perf/parallel-recording-policy-live.py --port <p> --editor-pid <pid> \
    --scenes dense dense-low dense-fov40 dense-fov30 small courtyard integrated \
    --arms off legacy policy --rounds 2 --capture GBufferAlbedo GBufferNormal SceneDepth \
    --output <out.json>
```
