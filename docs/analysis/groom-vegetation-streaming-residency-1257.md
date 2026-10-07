# Groom and vegetation residency evidence (#1257)

Twelve full Release Editor cells, two separate sparse post-fix cells and two post-fix
Forward Runtime runs were retained on 7 October 2026. They show optional detail ->
pressure fallback -> reload behavior. Historical runs keep their original provenance;
new binaries are credited only by their own checks. Debug GL shader channels report
zero errors; Vulkan tracking remains unavailable. Region events and paired frame tails
remain pending. Both backends now have clean Runtime shutdown rechecks.

The [public evidence ledger](evidence/groom-vegetation-streaming-residency-1257.json)
records exact artifact hashes, effective states, latches and gaps. Raw artifacts remain
under ignored `artifacts/residency/`; eight unchanged post-fix Runtime PNGs are public.

## Editor and shader evidence

The [measured results](groom-vegetation-streaming-residency-1257-results.md) retain the
12-cell Release matrix, audited post-fix sparse transitions and four Debug shader
captures. Release samples cover both families on all six backend/path combinations;
optional GPU detail reaches zero under pressure and reloads. Complete scene UUID lists
remain stable. The sparse follow-up independently verifies 19 GL / 22 Vulkan
checkpoints and 149 PNG hashes. Vulkan sparse detail/reload has complete RT casters;
pressure intentionally withholds TLAS for two missing-impostor sources and falls back
to raster. Default parallel Vulkan CSM lacks named family timers; inline VSM is observed.
A separately forced-inline CSM diagnostic records valid positive family timing in all
three representation phases, with actual parallel recording false.

Debug GL groom/vegetation shader channels are available with zero reported errors
before/after. Vulkan returns unavailable/null/notInitialized, despite native logs
having no matching failures. Vegetation Debug PNGs include stale samples and are not
all accepted as fresh images. Cold-start failures and successful groom retries retain
separate provenance. No all-backend clean-shader verdict is claimed.

## Actual runtime observations

The authored bound Dog and Poly Haven fern export was launched through the real
Runtime/AssetPackManager pipeline, with support files staged as in game deployment.
Authoritative post-fix runs are `runtime-credible/cells/opengl-forward/capture-20261007-211533`
and `runtime-credible/cells/vulkan-forward/capture-20261007-211409`; both exit zero with
no surviving-allocation/error/VUID matches in the actual runtime logs. Historical
204018/204212 runs remain retained separately. The ledger records exact old/new binary,
pack and scene hashes. Runtime source commit was not recorded in the run ledgers;
no source revision is inferred from the binary.

| Backend / runtime path | Retained evidence | Limits |
|---|---|---|
| OpenGL Forward, post-fix 211533 | Initial, pressure, recovered, scene-reloaded PNGs; body/coat and fern visible; Lua identity markers stable | No GPU counter telemetry or runtime path selector |
| Vulkan Forward, historical 204212 | Same four phases, body/coat and fern visible, identity markers stable | Teardown reported 16 surviving allocations, 33,998,848 bytes |
| Vulkan Forward, post-fix 211409 | New four-phase run exits zero; actual runtime log has no surviving-allocation/error/VUID matches | Separate new binary; historical diagnostic remains retained |

Groom/plant UUID markers remain `1257001` / `1257002` through reload; the ledger keeps
exact packed handles and the 409,940-root CPU validation. Lua changes 256/160/4096 MiB
to one-byte resident pressure, restores and reloads. Markers are settings/identity proof,
not GPU counters. Runtime exposes only Forward here; other paths are unavailable.
Warmup PNGs and stale prelaunch log copies are excluded; actual runtime logs are hashed.

The historical allocation diagnostic remains retained despite its zero exit code.
New Vulkan 211409 and GL 211533 runs independently verify shutdown in actual Runtime.
Their unchanged public PNGs are
[GL initial](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_GL_Forward_initial.png),
[pressure](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_GL_Forward_pressure.png),
[recovered](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_GL_Forward_recovered.png),
[reloaded](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_GL_Forward_reloaded.png),
[Vulkan initial](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_Vulkan_Forward_initial.png),
[pressure](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_Vulkan_Forward_pressure.png),
[recovered](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_Vulkan_Forward_recovered.png), and
[reloaded](../../OloEditor/assets/tests/visual/GroomVegetationStreaming_Runtime_Vulkan_Forward_reloaded.png).
The ledger verifies their exact SHA-256 against the retained source PNGs.

## Appearance and test scope

AnimalPopulation grooms are unbound to the fox rig and too small in the live views
to demonstrate finished coat appearance. The forest is dark; pressure visibly replaces
fine silhouettes with huge dark cutout cards/blobs, including foreground coverage.
The authored Runtime dog has visible fine coat fibers initially and conspicuously coarse
cards under pressure; fern geometry also changes. Recovery and reload restore finer
structure. These are functioning fallback observations with art-quality gaps, not
seamless or finished AAA appearance claims. Sparse motion frames do not prove temporal
quality.

The GL fixtures verify queued/prepared/resident cleanup, staging release, draw-handle
retirement, physical retiring charge, bound GPU deformation, history recovery and UUIDs.
The bound fixture's PNGs show a coat without a body. Prior CPU 64-case and isolated
Vulkan device passes are separate from live acceptance.

Post-fix Release/Debug builds passed 2,050/2,049-object dependency checks. GL passed 7/7;
Debug Vulkan shutdown, proxy and AS device runs each passed 1/1, exercised, with zero
VUID/error matches; application smoke passed 8/8 and latest CPU streaming regressions
passed 57/57. XML/log hashes are in the ledger.
Debug shader reachability is recorded separately; the earlier 12 cells keep their
original source provenance.

## Region integration correction milestone

The initial region campaign retained groom captures but both vegetation integrations
crashed at the same null `bakeVBO` call. Matching dump/PDB UUIDs and a failing upload-
refused regression establish CPU payload readiness incorrectly used as GPU mesh readiness.
The correction requires this layer's published buffers/material parts and mesh path,
uses its retained bounds, and waits for admitted cutoff updates before rebaking.

Release all four targets passed with 2,050 dependency objects. The red SEH regression
now passes 1/1; GL residency passes 8/8 with zero skipped, CPU streaming 57/57 and
application smoke 8/8. Exact dump, preserved executable/PDB and completed build/test
hashes are in the ledger; [measured results](groom-vegetation-streaming-residency-1257-results.md)
retain the diagnosis and limits. These functional runs overlapped Debug compilation;
their elapsed times are not performance evidence. Debug now passes 2,049 dependency
objects; Vulkan shutdown, proxy and AS checks each pass 1/1, EXERCISED, no VUID/errors.
Live region reruns remain pending. Earlier 12-cell, sparse and Runtime retain their own
pre-correction provenance and do not verify this new binary in those live workloads.

## Workloads and remaining measurements

Regenerate with `python scripts/perf/generate-streaming-residency-fixtures.py`.
Variants reference existing cooked grooms and scan vegetation; dog source assets are
unchanged. All three rendering paths have manifests under
`OloEditor/assets/benchmark/manifests/streaming-*.yaml`.

| Family | Normal resident / upload per frame / staging | Pressure | Region load / unload radius |
|---|---|---|---|
| Groom | 64 / 8 / 128 MiB | Resident 0.001 MiB | 45 / 55 m |
| Vegetation | 128 / 32 / 128 MiB | Resident 0.001 MiB | 125 / 135 m |

Each manifest requests 1,000 stationary, 180 degrees/s turn and traversal steps. Two real region
files and linked point-distance volumes exist per family. Their traversals reach both
load circles and leave the first unload circle: eligibility, not an observed event.
Pending region execution must record loaded-count changes and eviction increments;
individual loaded IDs are not exported. Zero budgets are unlimited; all three zero is eager.

The [controlled-performance](../guides/controlled-performance.md) workflow requires warm
caches, two pairs, fresh AB/BA processes, identical inputs and 1,000 samples per camera.
The descriptive report records tails, GPU exclusions and CPU/GPU backing without a
significance verdict:

```powershell
python scripts/perf/streaming-residency-report.py --plan C:/perf/streaming-plan.json --output C:/perf/streaming-run
```

Use the build-lock wrapper; `--summarize-capture` reads retained output. AB/BA and region
measurements are pending; no speedup is claimed. Read wall time includes OS caching,
preparation includes CPU build work, and neither is isolated disk latency. Optional
groom preparation uses an already resident source, not a separate tier file.

The ledger retains exact supporting commits `d3e7307` (hash refresh), `b6ee5b4` (buffer
backing), `e473de4` (texture backing), and `4931416` (runtime title). Historical Editor
captures predate the last commit; later dirty source is credited only by new captures.
