# ReSTIR PT live motion and integration evidence

Read this alongside [the numerical validation](restir-pt-1211-evidence.md).
Measurements used a Debug Vulkan editor on an NVIDIA GeForce RTX 4090. The
prototype remains experimental, off by default, and visibly noisy at eight
initial candidates. PNGs are display-clamped; quantitative comparisons use
floating-point ROI statistics. The exported furnace HDRs use quantized RGBE.

## Motion

Each material ran 14 cases: initial, temporal, spatial and combined with mask7,
plus combined with each mapping separately (masks1/2/4), each with seeds1211/974.
The camera moved continuously for 20 seconds per case along
`x=0.3*sin(t*12 degrees/second), y=1.5, z=3`, looking at the origin.
Camera requests and readbacks were asynchronous; these are not exact pose/frame
pairs or a claim to sample every rendered frame. Seven debug views were captured
in a separate phase while motion continued.

| Material | Distinct numeric frames | Actual camera request rate | Range of case means |
|---|---:|---:|---:|
| Diffuse | 2,296 | 19.05–20.00 Hz | 0.47410–0.48054 |
| Glossy | 2,098 | 16.13–19.97 Hz | 0.29156–0.29556 |

Every sampled ROI channel was finite. Nonfinite and clamped counters remained
zero. Temporal history remained valid in modes that enable temporal reuse;
initial and spatial-only modes correctly report no temporal history. There was
no sustained brightness growth in these runs. The camera changes the actual
integral, so quarter-interval differences and temporal standard deviations in
the data are descriptive, not error bars against a moving reference.

For diffuse mask7, the observed standard deviation of the ROI mean was
0.0318–0.0334 initially, 0.0246–0.0251 with temporal reuse, 0.0350–0.0410 with
spatial reuse and 0.0299–0.0303 combined. This configuration does not demonstrate
a universal variance improvement from spatial reuse. Temporal frames are correlated.
Data: [diffuse](evidence/restir-pt-1211/motion-diffuse.json),
[glossy](evidence/restir-pt-1211/motion-glossy.json).

The first glossy command stopped when its editor window was minimized and the
render loop parked; stale data was rejected. Only its two completed initial cases
were saved. The complete 14-case rerun above used a separate background editor
and exited 0. The diffuse command also exited 0.

## Inspected images

The following are actual pipeline captures, without a denoiser. Raw signal,
history ancestry, candidate variance, reservoir lineage, Jacobian conditioning,
and clamp state are separately inspectable.

| View | Diffuse motion | Glossy motion |
|---|---|---|
| Radiance | [PNG](evidence/restir-pt-1211/motion-diffuse-radiance.png) | [PNG](evidence/restir-pt-1211/motion-glossy-radiance.png) |
| Selected raw sample | [PNG](evidence/restir-pt-1211/motion-diffuse-raw.png) | [PNG](evidence/restir-pt-1211/motion-glossy-raw.png) |
| History | [PNG](evidence/restir-pt-1211/motion-diffuse-history.png) | [PNG](evidence/restir-pt-1211/motion-glossy-history.png) |
| Variance | [PNG](evidence/restir-pt-1211/motion-diffuse-variance.png) | [PNG](evidence/restir-pt-1211/motion-glossy-variance.png) |
| Lineage | [PNG](evidence/restir-pt-1211/motion-diffuse-lineage.png) | [PNG](evidence/restir-pt-1211/motion-glossy-lineage.png) |
| Conditioning | [PNG](evidence/restir-pt-1211/motion-diffuse-conditioning.png) | [PNG](evidence/restir-pt-1211/motion-glossy-conditioning.png) |
| Clamp | [PNG](evidence/restir-pt-1211/motion-diffuse-clamp.png) | [PNG](evidence/restir-pt-1211/motion-glossy-clamp.png) |

Green history means the selected sample has temporal ancestry; red means it
does not. Red is not an error count. Variance uses `v/(1+v)` display encoding.
Conditioning encodes signed log-J. Clamp red marks clamped pixels and green marks
the enabled bias mode; the disabled view is black.

The original #974 100-sphere furnace also rendered at 640x360 from three wider
camera poses, with 100 TLAS instances and zero nonfinite counters at each pose:
[front raw](evidence/restir-pt-1211/furnace-angle-0-raw.png),
[right beauty](evidence/restir-pt-1211/furnace-angle-1-beauty.png),
[left raw](evidence/restir-pt-1211/furnace-angle-2-raw.png).
The white background is the furnace environment. Full radiance lies outside
the PNG range; [diffuse HDR](evidence/restir-pt-1211/furnace-diffuse.hdr) and
[glossy HDR](evidence/restir-pt-1211/furnace-glossy.hdr) retain linear RGBE values.

## Measured cost

These final snapshots use the diffuse three-instance corner at 160x90, eight
initial candidates, mask7. A second visible editor remained active, and the host
also had unrelated build work. These are measured diagnostic costs, not isolated
performance rankings or production frame-time forecasts.

| Mode | PT parent GPU median ms | AS build/refit scope median ms | Closest-hit queries | Visibility queries |
|---|---:|---:|---:|---:|
| Initial | 8.301 | 0.017 | 125,451.5 | 0 |
| Temporal | 19.080 | 0.016 | 592,743 | 341,268 |
| Spatial | 18.386 | 0.016 | 559,378.5 | 319,936 |
| Combined | 29.497 | 0.017 | 1,183,908.5 | 778,706 |

Query counts are medians of actual shader counters; fractional medians average
the middle two frames. Ten snapshots were retained per mode. One spatial GPU
timer returned zero; it remains in the artifact as unresolved and is excluded
from the positive-timing median. Counter and GPU timer snapshots have separate
frame provenance. The parent PT scope includes readback/submission gaps; do not
add parent and child timings or call the parent pure shader execution time.

Four suffix pools consume 34,099,200 bytes (32.52 MiB), excluding attachments and
counters. Steady static frames report one TLAS update and no BLAS builds. The AS
scope above is a GPU timestamp; the dedicated `blasBuildGpuNs`/`tlasBuildGpuNs`
fields remain unpopulated, so their zero values do not mean free work. Full
snapshots, counters, subpass times and the 100-sphere profile are in
[performance.json](evidence/restir-pt-1211/performance.json).

## Fallback, clamp and graph checks

The live deferred editor rejected enabled water both on camera and far outside
the view, with `incomplete GPU scene geometry coverage` and procedural omission
count1. Disabling water cleared the count and resumed PT. Forward and Forward+
stood down with `deferred path required`; enabling fog also stood down. The shared
omission-accounting fix does not change the water renderer or projected grid.
The final OpenGL editor reported `hardware ray queries unavailable`, inactive PT,
invalid counters and zero reservoir allocation. MCP `available` means the pass's
diagnostics exist; execution support is reported by `active` and `fallbackReason`.

With radiance clamp0.01, the pass reported `biasedClamp=true`, 13,246 clamped pixels,
valid temporal history and an unchanged scene epoch. The
[clamp-on image](evidence/restir-pt-1211/clamp-enabled.png) exposes that bias.
Clamp and debug mode were then reset.

Live shader errors were zero. PT-on and PT-off graph validation both reported
`ok=true`, zero hazards, no unbacked consumed resources, no build diagnostics,
no resolve failures and no Vulkan stubs. The diagnostic stream is not empty:
the baseline has 18 missing-producer reports for aliases/imports, and PT adds
three analogous reports for GBufferResolved, GBufferBakedGI and PrefilterMap.
These are retained rather than described as a completely clean graph.
See [admission and graph data](evidence/restir-pt-1211/live-admission.json).

The temporary 160x90 override displayed a small image in the full editor panel.
It was removed from the visible editor, both tracers disabled, and a fresh target
confirmed the panel's normal 4291x2403 size. Remaining low-resolution validation
ran in a separate background instance, which was stopped after capture.
The matrices precede the final epoch/cache-key review fixes; the rebuilt shader
and contracts subsequently passed all 30 focused tests. No estimator shader math
changed between the matrices and that rebuild.
