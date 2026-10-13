# Per-frame CPU scope diagnosis: #1594

Use the in-process scope recorder to retain every frame during a natural editor run, and verify
rendered-frame ownership before selecting a camera's legacy history.

The recorder, per-pass labels, and sampling corrections are verified. The original recurring
0.65–2.1 s outdoor CPU-only hitch was not reproduced in the uncontended campaign, so this PR
references #1594 and leaves it open. Early post-arm/setup frames did show stalls, narrowed to
`ShadowPass` and `GroomPass`, with elevated same-frame GPU work as well. This is not a claim that
the original cause was fixed.

## Protocol and evidence

Windows, i7-14700KF, RTX 4090, driver 617.42; Release editor; 1920×1080; VSM enabled; debug
draw, edit/play throttling, autosave and frame cap disabled. Each campaign held an exclusive
build lease, with no competing editor/build/test process and zero decoder activity in recorded
before/after probes. Preferences were restored afterward. Requests were made per window,
without per-frame MCP polling during timing.

- [Original camera table](../testing/evidence/cpu-scope-ring-1594/idle-host-camera-table/report.json):
  all five original cameras, three sweeps, 60 Vulkan and 45 OpenGL windows, 120 retained samples
  per window. Its frozen update-counter verifier ran with both throttles disabled; the full
  scope audit confirms renderer progress on every captured frame.
- [Final-binary attribution](../testing/evidence/cpu-scope-ring-1594/final-binary-attribution/report.json):
  two 65-second natural Deferred traces plus 14 outdoor windows across all requested paths,
  using the corrected renderer counter and history generation.
- [Final-binary overhead](../testing/evidence/cpu-scope-ring-1594/idle-host-overhead-final/report.json):
  three fresh-process armed/disarmed pairs per backend, alternating order, at least 1,200
  completed render frames per run and the last 1,000 history samples per comparison.
- [Initial overhead](../testing/evidence/cpu-scope-ring-1594/idle-host-overhead/report.json) is retained
  separately under its original binary hash; it predates the additional per-pass labels.

These timing campaigns predate merge `23dbff0a8`, which imports master `23b502eec`'s groom
coverage and Deferred resolve changes. They verify the instrumented campaign binary; they are
not post-merge pacing proof. Recorder and sampler sources are unchanged by that merge.

The measured campaign executable SHA-256 is
`7A780F12BA0359D38761E8DD2F5050F88EA82121CEE6D32FB41C0CEE19B61631`.
Scope audits of the 105 original windows, 14 final windows, both final traces and all six final
armed overhead captures found zero dropped events, overwritten frames, late completions or
incomplete scopes. The camera/trace audits also found zero frames without renderer progress.

Legacy samples have no matching scope frame IDs. The reports keep these populations separate.
No exact recorder-frame camera-event marker was recorded, so early frames are labelled
post-arm/setup rather than asserted cut IDs. The 300 ms diagnostic threshold is separate from
a frame-budget deadline. Legacy GPU statistics can include older resolved queries; only
same-frame recorder values with valid status are used for attribution.

## Outdoor results

Ranges below are across the three original sweeps. Final values come from the subsequent
final-binary checks. Units are milliseconds; each row has 360 original and 120 final samples.

| Backend | Path / camera | Original p95 range | Original p99 range | Final p95 / p99 / max |
|---|---|---|---|---|
| Vulkan | Forward / population-dolly | 116.48–129.58 | 121.54–133.59 | 118.46 / 122.65 / 125.10 |
| Vulkan | Forward+ / population-dolly | 121.61–123.11 | 126.80–135.08 | 120.08 / 130.02 / 131.95 |
| Vulkan | Deferred / population-dolly | 120.89–128.43 | 126.78–142.44 | 118.25 / 124.88 / 131.07 |
| Vulkan | Hybrid / population-dolly | 185.79–186.74 | 188.37–199.55 | 190.56 / 209.09 / 209.09 |
| Vulkan | Forward / rapid-turn | 92.42–97.04 | 96.58–105.63 | 91.53 / 99.87 / 121.90 |
| OpenGL | Forward / population-dolly | 122.45–128.47 | 127.61–143.99 | 129.43 / 150.47 / 167.33 |
| OpenGL | Forward+ / population-dolly | 121.70–131.11 | 125.86–144.03 | 125.39 / 130.23 / 132.88 |
| OpenGL | Deferred / population-dolly | 118.19–126.18 | 123.71–139.65 | 124.84 / 143.18 / 149.15 |
| OpenGL | Forward / rapid-turn | 94.17–97.68 | 97.89–110.98 | 91.83 / 98.85 / 100.91 |

Hybrid enabled RT and populated the RT scene, but vegetation pressure withheld the shadow
TLAS (`shadowTlasWithheld: true`, existing #1578). These are measurements of that configuration's
fallback; they do not establish full RT-shadow dispatch coverage. OpenGL Hybrid RT is unsupported.

The final natural Vulkan trace retained 612 scope frames and 609 legacy samples: p95 119.50,
p99 122.87, max 159.64. Its early 727.23 ms scope frame contained `ShadowPass` 423.07 and
`GroomPass` 224.07 ms, with valid same-frame GPU 231.40 ms. OpenGL retained 597 scope frames and
594 legacy samples: p95 124.60, p99 135.13, max 188.13. Its early 721.22 ms frame contained
`ShadowPass` 409.85 and `GroomPass` 230.31 ms, with valid same-frame GPU 653.67 ms. The named
scope values are CPU wall spans and can include waits; they do not isolate pure CPU computation.

## Recorder cost

These are descriptive run-level comparisons, not calibrated confidence intervals or a pooled
frame bootstrap. The final recorder includes per-pass labels.

| Backend | Paired p50 deltas | Paired p95 deltas | Paired p99 deltas | Median p50 / p95 / p99 delta |
|---|---|---|---|---|
| Vulkan | +2.16, +2.61, −0.12 | +4.29, −3.10, +0.26 | +17.97, +5.00, −0.43 | +2.16 / +0.26 / +5.00 |
| OpenGL | +0.02, −0.45, +0.40 | +0.27, −0.40, +0.80 | +4.25, +1.90, −0.15 | +0.02 / +0.27 / +1.90 |

Typical Vulkan cost was about 2.1% of the disarmed median; the third pair was near baseline.
The tail variation, especially Vulkan's first +17.97 ms p99 delta, prevents an unqualified
negligible-tail claim. All final armed and disarmed retained histories stayed below 300 ms.
Both states lacked the original recurring hitch; its absence cannot be credited to arming.

## Contracts, builds and bundled #607 check

The Release contract run has 87 passes, one Windows symlink-privilege skip and zero failures
([XML](../testing/evidence/cpu-scope-ring-1594/final-recorder-contracts.xml)). A real NTFS-junction
probe separately reproduced and fixed the sandbox's outside-directory creation. The UTF-8
export regression failed before the fix with JSON type error 316 and passed afterward
([before log](../testing/evidence/cpu-scope-ring-1594/utf8-export-before.txt)). Ten Python
regressions cover camera-window ownership, throttling, long hitches and resets; two review
regressions failed before correction
([before log](../testing/evidence/cpu-scope-ring-1594/sampler-review-before.txt)).

Release editor/tests and Debug editor built through the build lock. Dependency audits passed
for all 2,075 Release and 956 Debug objects. Retained camera and final trace logs confirmed
their explicit backend, with zero `[error]` or VUID entries. Final population-dolly screenshots
from both backends were visually inspected. Pre-commit passed after formatting.

The retained uninstrumented `b76e5b4be` executable completed all four bundled
`olo_render_frame_breakdown` cells: Vulkan Forward+ 0.491 s, Vulkan Deferred 0.419 s, OpenGL
Forward+ 0.486 s, OpenGL Deferred 0.443 s. The #607 body checkbox is ticked with
[baseline evidence](../testing/evidence/cpu-scope-ring-1594/baseline-frame-breakdown.json).
An earlier minimized OpenGL attempt was invalid because the frame loop was parked.

The reusable sampling rule is in
[completed-frame-perf-windows.md](../agent-rules/completed-frame-perf-windows.md).
