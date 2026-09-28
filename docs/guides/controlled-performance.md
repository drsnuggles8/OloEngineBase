# Separate lower-bound cost from controlled frame regressions

Use `PerfRegressionTests.cpp` for favorable-case operation cost, and
`scripts/perf/controlled-benchmark.py` for paired run-level frame comparisons
(#1339). A minimum does not estimate typical latency or a frame deadline tail.
The historical `_ns_median` XML properties were mislabeled minima; new reports
name them `_ns_lower_bound`. Historical artifacts remain unchanged.

The controlled runner consumes the existing benchmark capture output. It runs
fresh processes in alternating AB/BA order, preserves their raw CSVs, and computes
each camera's statistics within each run. Uncertainty comes from independent
**run pairs**, using a distribution-free sign interval for the median paired
effect. It never resamples correlated individual frames. `familyAlpha` is divided
across every declared camera and gate (Bonferroni). Choose enough pairs for that
confidence level; insufficient pairs are rejected before captures start.

Configure tolerances and maximum acceptable interval widths from an A/A campaign
on the designated machine. These are fractions: `0.05` permits a 5% relative
increase; for `deadlineMissFraction`, it permits a 5 percentage-point increase.
P50/p95/p99 are separate gates. P99 requires 1,000 valid frames **per run**;
the existing integrated manifests' 180 frames do not qualify. Prepare a longer
measurement manifest while preserving the workload, quality and warmup.

Exit codes are part of the contract: 0 means the upper confidence bound is within
tolerance, 1 means a regression was established, and 2 means inconclusive or
unsupported. Excessive uncertainty never passes. A GPU gate excludes invalid,
missing and duplicate samples with reasons and requires enough remaining samples.
CPU execution, recording/presentation and fence waits remain separate channels;
only compare captures with the same host and measurement interval. Vulkan's
presentation interval includes command recording and submission, not just VSync.

## Prepare and run

Build both revisions in Release with symbols, prime their shader caches, and stop
other builds and editors. The runner records CPU/GPU driver identity and checks
for competing build/editor processes before and after each capture. This check
cannot certify isolation between probes or detect all other applications: reserve
the machine and inspect the retained host records. Total subprocess lifetime is
retained separately; it is not a startup measurement. The editor host additionally
records launch-to-responsive-MCP time, including initialization and shader loading,
before opening the benchmark workload. The test host does not instrument startup.

Use `host: "editor-mcp"`, `backend: "opengl"` or `"vulkan"`, and a free `mcpPort`
on each arm for whole-frame gates. Its `frameTimeMs` channel is the completed
editor-frame interval. The profiler records every completed interval between
measurement start and stop, including frames between MCP camera/motion steps;
tracing can perturb it, so keep the capture protocol identical between arms.
CPU frame IDs must be contiguous within each camera and every requested motion
step must finish. Raw frame counts can exceed the manifest's step count; buffer
overflow and older snapshot-only exports are rejected. The `test-binary` host only measures the scene-render call
and cannot supply `frameTimeMs`. Both hosts export resolved per-frame GPU pass
records with validity and frame IDs. A gate such as `passGpuMs:DeferredLightingPass`
uses each unique valid pass interval once; subpasses are never added to parents.

A plan is JSON. Each arm declares `repo`, `exe`, `buildConfig` (`Release`),
`compiler`, `shaderRoot`, `assetRoot` and `shaderCache` as absolute paths/values.
The runner hashes both binaries, complete declared shader/asset trees, each
manifest, tracked dirty diffs, and untracked files. Declare the complete asset
input tree, not just its scene file. Preserve the compiler/build logs next to the
report: source and binary hashes identify inputs but do not prove how a binary
was built. Keep the output directory outside the hashed trees.

Top-level fields:

```json
{
  "controlled": true,
  "machine": "YOUR-DESIGNATED-HOST",
  "cacheMode": "warm",
  "pairs": 24,
  "familyAlpha": 0.01,
  "timeoutSeconds": 600,
  "arms": {"A": {}, "B": {}},
  "workloads": [{
    "manifest": "OloEditor/assets/benchmark/manifests/integrated-deferred.yaml",
    "expectedFrames": 180,
    "cameras": ["stationary", "dolly", "rapid-turn"],
    "gates": [{
      "channel": "renderCallMs",
      "statistic": "p95",
      "tolerance": 0.05,
      "maxIntervalWidth": 0.10
    }]
  }]
}
```

Fill both arm objects as described above. The numeric tolerances in this example
illustrate the schema; they are **not calibrated budgets**. Include a startup/small
scene as well as the mixed/dense scene. Supported channels are `frameTimeMs`
(editor only), `renderCallMs`, `cpuMs`, `gpuMs`, `fenceWaitMs`, `presentWaitMs`,
`recordingWallMs`, `recordingJoinWaitMs`, and `passGpuMs:<pass name>`; statistics are
`p50`, `p95`, `p99`, and `deadlineMissFraction`.

`drift-menu-performance.yaml` supplies the small stationary workload with 1,000
frames. Pair it with `integrated-deferred-performance.yaml`, which preserves the dense
scene and entity motion while keeping the camera stationary for 1,000 frames. Do not simply lengthen the
integrated dolly/turn camera sequence to obtain more samples: its unbounded camera
motion would leave the original coverage region. Retain those finite sequences
for their intended workload, and use stationary captures for longer tail runs.

```powershell
python scripts/perf/controlled-benchmark.py --plan C:/perf/plan.json --output C:/perf/run-001
```

Test-binary OpenGL scene-call and editor OpenGL/Vulkan frame captures have distinct
intervals and cannot be mixed in one comparison. Requested upscalers are rejected because the
capture result does not yet retain downstream consumption proof; selected settings
alone are insufficient. Actual internal/display dimensions, camera counts, declared
sample counts and warmup completion are checked before any comparison.

The `Controlled performance comparisons` workflow runs deterministic decision-rule
and capture-validity tests on PRs. Its actual measurement job is manual, requires
the `olo-perf` Windows runner and `OLO_CONTROLLED_PERF_ENABLED=true`, and propagates
regressions **and inconclusive results** as failures while uploading raw evidence.
Dispatch without a configured runner explicitly fails as unavailable.

## Calibration controls

`python -m unittest discover -s scripts/perf -p 'test_controlled*.py' -v` exercises
a 20% injected slowdown with common drift and an outlier, 1,000 seeded no-change
simulations, interval coverage, noisy/insufficient input, zero deadline misses,
truncation and GPU validity. These test the instrument, not the engine or a
workstation's false-alarm rate. A real A/A run and deliberately slowed B arm on the
designated runner are still required before adopting a calibrated plan.

For a real rendered negative control, set B's optional `injectDelayMs` to a
known delay such as 20. The test binary sleeps inside the measured wall interval
without changing rendering, and writes `calibration.json`. The runner requires
that recorded delay to match the plan in both arms (default 0), so an old binary
that ignores the control cannot yield a passing calibration. This affects only
`renderCallMs`; GPU and CPU-execution channels are not artificial slowdowns.
