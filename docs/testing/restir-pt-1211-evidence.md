# ReSTIR PT prototype validation (#1211)

The prototype is off by default and integrates indirect reflection paths with up to
three secondary vertices. It supports finite-width Legacy and Closure V2 reflection,
including the shared near-specular roughness floor. Transmission, true delta
materials, participating media, unsupported scene geometry and finite-radius analytic
lights stand down. No probe-cache tail is added to the finite-depth integral.

The design was committed as `245273c3a` before the first production contract commit
`34c1a9b62`. See [the derivation](../design/restir-pt-shift-mappings.md).

## Build and test coverage

The final Debug editor, runtime, and test targets built successfully (exit 0),
including the review fixes. A focused run then passed all 30 tests with no skips
or failures (exit 0): PT device/contract suites, water coverage, and shader reload. The full test
executable ran 8,314 cases: 8,271 passed, 42 skipped, and one failed, with 12 more
cases disabled; exit 1. All nine PT device cases and the production Vulkan shader
compilation sweep passed. The single failure was
`VirtualClusterCullParity.SwListAppendNeverWritesPastAnUndersizedCapacity`: its
separate GPU append counter was left uninitialized. Production already clears
that header. Commit `64c5a0f43` initializes both fixture headers; the two-case
suite then passed five repetitions (10 passes, no skips or failures), exit 0.
The whole suite was not rerun after that correction or the later water-coverage
and review fixes. The water coverage regression and PT admission case both passed
on the rebuilt targets (two passed, none skipped or failed).

## Numerical device evidence

All nine Vulkan PT device cases passed in the broader test run. Its synthetic
primary G-buffer is an explicit substitution; these results
validate GPU suffix transport and resampling, not rasterization or editor integration.

The closed Cornell fixture compares the PT indirect signal against a paired CPU
reference with exactly matching finite-depth support, using 512 samples per pixel
in eight independent reference blocks. GPU blocks use eight frames of 64 candidates.
The combined mode reads previous initial records only, then performs spatial reuse.

| Material | Mode | Reference mean | PT mean | Combined standard error |
|---|---|---:|---:|---:|
| Diffuse | Initial | 0.194760 | 0.194166 | 0.00101187 |
| Diffuse | Temporal + spatial | 0.194760 | 0.194056 | 0.00116223 |
| Glossy | Initial | 0.115460 | 0.113181 | 0.000880025 |
| Glossy | Temporal + spatial | 0.115460 | 0.114192 | 0.00146609 |

The whole-integral environment fixture measured diffuse RGB
`(0.661190, 0.565109, 0.469028)` against reference
`(0.661059, 0.565078, 0.469098)`, and near-specular metal RGB
`(0.650354, 0.550300, 0.450246)` against
`(0.650039, 0.550032, 0.450027)`.

Production-shader negative controls deliberately remove the Jacobian after real
support tests. RMS HDR changes were 0.000711561 for reconnection, 0.0132077 for
replay and 0.0136552 for hybrid. Omitting the mapped path changed reconnection by
0.00424311, replay by 0.0730945 and hybrid by 0.0735928. These controls test the final answer rather
than merely checking that a computed Jacobian differs from one.

## Live Vulkan GPU-oracle comparisons

Both matrices use 160x90 rendering and a fully covered 16x16 floating-point ROI,
with 32 distinct sampled frames per seed/pose block and a 512-spp GPU oracle.
Each material covers four modes, mapping masks 1/2/4/7, three stepped poses, and
four seeds: 192 blocks per material, 384 per scene family. Standard errors below
collapse each seed across poses and masks first; temporal frames are not treated
as independent. The raw per-case results remain available, including outliers.

| Scene | Material | Mode | PT mean | GPU oracle | Difference standard error |
|---|---|---|---:|---:|---:|
| Furnace sphere | Diffuse | Initial | 0.982330 | 0.982780 | 0.001021 |
| Furnace sphere | Diffuse | Temporal | 0.983916 | 0.982780 | 0.000734 |
| Furnace sphere | Diffuse | Spatial | 0.982698 | 0.982780 | 0.002259 |
| Furnace sphere | Diffuse | Combined | 0.983954 | 0.982780 | 0.001175 |
| Furnace sphere | Glossy | Initial | 1.001384 | 1.000128 | 0.001317 |
| Furnace sphere | Glossy | Temporal | 1.000164 | 1.000128 | 0.000989 |
| Furnace sphere | Glossy | Spatial | 0.997960 | 1.000128 | 0.001660 |
| Furnace sphere | Glossy | Combined | 1.001578 | 1.000128 | 0.000836 |
| Open corner | Diffuse | Initial | 0.479643 | 0.478354 | 0.000328 |
| Open corner | Diffuse | Temporal | 0.478711 | 0.478354 | 0.000522 |
| Open corner | Diffuse | Spatial | 0.479127 | 0.478354 | 0.000517 |
| Open corner | Diffuse | Combined | 0.478509 | 0.478354 | 0.000522 |
| Open corner | Glossy | Initial | 0.292655 | 0.293173 | 0.000546 |
| Open corner | Glossy | Temporal | 0.292984 | 0.293173 | 0.000122 |
| Open corner | Glossy | Spatial | 0.292549 | 0.293173 | 0.000327 |
| Open corner | Glossy | Combined | 0.293837 | 0.293173 | 0.000292 |

The corner reuses #974 cube/material/environment assets with no emitters or
analytic lights. GPU max-bounces 5, Russian roulette off and NEE off match PT's
four-scatter environment-escape support. A max-bounces-2 control gives means
0.378154 diffuse and 0.095170 glossy, establishing substantial higher-order
transport. The convex furnace sphere has only zero-secondary support. Raster
primary samples and jittered oracle rays have different pixel quadrature; these
measurements do not establish exact per-pixel equality or a general unbiasedness
proof. All sampled PT counters reported zero nonfinite and clamped events.

See [furnace case data](evidence/restir-pt-1211/furnace.json) and
[corner case data](evidence/restir-pt-1211/corner.json). The full corner command and
the glossy furnace command exited 0. The diffuse furnace numeric stage completed
before an optional HDR export rejected a missing manifest tolerance. That
manifest was corrected; the separate export succeeded with zero attachment
failures and no warmup timeout. RGBE exports are linear but quantized.

The original diffuse combined/mask7/pose0 outlier is retained: difference
0.005408 with standard error 0.000834 over four seeds. A separate run with 16
fresh seeds and 128 frames per block measured initial difference 0.000584
(SE 0.000887) and combined difference 0.002169 (SE 0.001455), exit 0. The original
standardized effect did not reproduce. See [follow-up data](evidence/restir-pt-1211/furnace-followup.json);
this is a statistical check, not a general unbiasedness verdict.

See [live motion, profiles, images and fallback checks](restir-pt-1211-live.md)
for the remaining runtime acceptance evidence.

## Integration defects caught during validation

The first editor enable exposed an imported-buffer lifetime validator that only
recognized native IDs; Vulkan buffer identities now receive live-registry checks.
The furnace HDR also exposed Git text conversion on a small binary without NUL
bytes; HDR files now explicitly retain binary bytes. Shader compilation honors the
per-stage `#pragma optimize(off)` required by this prototype's nested mapping/MIS
call graph. Other shaders retain their existing optimization policy.

Enabled water now contributes to the shared GPU Scene omission count, including
off-camera surfaces; PT visibly stands down instead of tracing an incomplete scene.
Scene reset also advances the PT suffix epoch, and shader cache keys distinguish
performance optimization from the explicit optimization opt-out.

Successful shader reloads invalidate sampler-dependent PT history. Record and
parameter layout versions fail closed before incompatible buffer access. Changes
to the byte ABI require a rebuild and restart.

## Reproducing live measurements

Build the editor through the repository build lock, then start it on Vulkan with
MCP write consent enabled. The scripts below connect to port 18311 by default;
use `--port` for another session. Without `--run`, they only prepare scratch inputs
or print the motion plan. Each run needs a fresh output directory.

```powershell
python OloEngine/tests/scripts/capture_restir_pt_furnace.py --run --matrix --materials diffuse glossy --seeds 1211 974 5423 9821 --samples 32 --oracle-spp 512 --max-seconds 14400 --prefix final --output final-furnace --hdr
python OloEngine/tests/scripts/capture_restir_pt_corner.py --run --matrix --materials diffuse glossy --poses 3 --seeds 1211 974 5423 9821 --samples 32 --oracle-spp 512 --max-seconds 14400 --prefix corner-final --output final-corner
python OloEngine/tests/scripts/capture_restir_pt_motion.py --run --matrix --material diffuse --seeds 1211 974 --seconds 20 --prefix final --reference OloEditor/assets/benchmark/captures/restir-pt-1211/final-furnace/results.json --output final-motion-diffuse
```

Repeat the motion command with `--material glossy` and another output directory.
For multibounce motion, pass `--corner --prefix corner-final` and omit
`--reference`; this performs a lateral sweep through the open-corner scene.
That sweep records moving estimates and history diagnostics without claiming a
matched oracle for every moving frame.
Outputs live under `OloEditor/assets/benchmark/captures/restir-pt-1211/` and retain
raw floating-point ROI statistics, distinct frame IDs, counters, and GPU timing
provenance. RGBE HDR exports quantize RGB and discard alpha; numerical comparisons
use the original floating-point statistics. Error bars use independent seed blocks,
not correlated temporal frames. The sphere has only zero-secondary support; the
open corner separately measures higher-order transport against a depth-matched
GPU oracle. Neither scene substitutes for a general unbiasedness proof.

The scripts leave their scratch scene and 160x90 viewport override active. After
measurement, disable PT and the reference tracer and call `olo_viewport_set_size`
with `{"reset":true}` to return rendering to the editor panel size.
