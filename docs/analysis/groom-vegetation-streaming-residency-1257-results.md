# Measured editor and shader observations (#1257)

See the [main residency analysis](groom-vegetation-streaming-residency-1257.md) for
runtime images, art limits, tests and remaining measurements. Exact raw hashes are
in the [public evidence ledger](evidence/groom-vegetation-streaming-residency-1257.json).

## Actual editor observations

Hardware: Intel Core i7-14700KF (28 logical processors), NVIDIA RTX 4090, driver
32.0.16.1714. Build: Release, clang-cl 23.1.0. `functional-gl-final` and
`functional-vk-final` share one Editor binary, built on `e473de4` with dirty source.
The ledger retains exact compiler revision, binary SHA-256, source commit and distinct
GL/Vulkan source-diff hashes; no clean-commit provenance is inferred.

| Backend | Family / path | PNG samples | Observed transition checkpoints | Complete UUID lists stable |
|---|---|---:|---:|---|
| OpenGL | Groom Forward / Forward+ / Deferred | 33 / 33 / 65 | 6/6 / 6/6 / 18/18 | 44 entities on each path |
| OpenGL | Vegetation Forward / Forward+ / Deferred | 33 / 33 / 70 | 6/6 / 6/6 / 19/19 | 4 entities on each path |
| Vulkan | Groom Forward / Forward+ / Deferred | 33 / 33 / 74 | 6/6 / 6/6 / 21/21 | 44 entities on each path |
| Vulkan | Vegetation Forward / Forward+ / Deferred | 33 / 33 / 79 | 6/6 / 6/6 / 22/22 | 4 entities on each path |

All 552 PNG files match their recorded hashes. No retained PNG reports stale or a
nonadvancing frame index; the first sample per cell has no preceding comparison.
PNG and diagnostic reads describe nearby sequential frames. UUID equality concerns
scene entities, not every canonical instance ID or component value.

| CSM cycle on all three paths | Detail | Pressure | Reload |
|---|---|---|---|
| Groom, both backends | 2,931,616 optional GPU bytes; 23 detail / 18 fallback draws | 0 bytes; 41 fallback draws | Same bytes and draw counts restored |
| Vegetation, OpenGL | 21,139,480 bytes; 3 resident / 1 fallback layer | 0 bytes; 4 fallback / 3 pending layers | 3 resident layers restored |
| Vegetation, Vulkan | 21,165,056 bytes; 3 resident / 1 fallback layer | 0 bytes; 4 fallback / 3 pending layers | 3 resident layers restored |

Groom pending requests were not observed by the sparse live sampler; the deterministic
gated fixture supplies separate pending evidence. Vegetation beauty submissions
record FoliagePass command counts 9 -> 4 -> 9. Later Deferred extras after the eager cycle
retain four detail layers; the ledger keeps those states separate from the earlier
three-layer CSM cycle.

Both groom Deferred cells record 41 grooms casting, 98 CSM cascade draws and 14 VSM
virtual-shadow level draws. GL VSM records 1,618 pages drawn and 2,086 resident, with
zero failed pages/cull overflows. Vegetation VSM records positive global page activity,
but its per-family shadow draw count is unavailable in these campaigns. VSM
`drawInstances` counts the mesh/skinned GPU cull output; foliage uses a separate
external caster route. Beauty command buckets and global pages do not establish
foliage shadow consumption. Separate post-fix sparse Deferred captures now retain
valid, non-stale `FoliageCasters` GPU brackets under GL CSM/VSM and Vulkan inline
VSM. Default Vulkan parallel CSM deliberately omits named family brackets; its
parent ShadowPass has fresh positive timing and four recorded secondaries. The
production draw route continues outside that timer guard. Brackets measure execution
spans, not exact draw counts or shadow pixel correctness. A separate Vulkan run with
`OLO_VK_PARALLEL_RECORDING=0` records actual parallel false and zero secondaries.
Its four valid non-stale CSM family brackets sum to 0.110 / 0.073 / 0.106 ms for
detail / pressure / reload (GPU frame IDs 169 / 272 / 372, age one). All 33 PNG
hashes match. This is diagnostic execution timing in a forced-inline configuration,
not default parallel named-family timing or a paired performance result.

Deferred MSAA4 samples latch scene sample count four on both backends. FSR spatial
requests resolve spatial. Temporal requests resolve temporal on GL, but Vulkan reports
`backendNotOpenGL` and resolves spatial at the same render scale. These are effective
renderer latches after rendered frames, not merely requests. TAA motion samples retain
valid surface history with advancing generations; sparse screenshots cannot establish
absence of ghosting at the representation boundary.

Vulkan groom RT-on samples report ready, 41 represented / zero refused coats, 83 TLAS
instances and 12,047,744 AS bytes; the subsequent per-light readback records one
ray-traced light and zero fallback lights. The viewport ray answers with a hit, without
proving it hit the groom. Vulkan vegetation RT-on instead reports `noData`, incomplete
caster preparation, zero ray-traced lights and one raster fallback because no TLAS was
available. Its viewport ray is unavailable; that dense cell is not credited as successful
vegetation RT lighting. New sparse post-fix Vulkan detail/reload samples are ready with
128 TLAS instances, 4,203,264 AS bytes, complete casters and one ray-traced light after
reload. The sampled pressure phase intentionally withholds TLAS and uses raster fallback:
two `missing-impostor-source` representations are refused and casters are incomplete.
The independent audit verifies 19 GL / 22 Vulkan transition checkpoints and all 149
PNG hashes. Fresh trace/resolve/filter brackets are positive in detail/reload; the
final query answers with a miss, proving query dispatch rather than a vegetation hit.

GL vegetation eager -> bounded samples move 5,586,944 live bytes from pinned to detail
ownership while combined physical backing stays 57,473,179 bytes. No transient retiring
charge was captured. Family accounting and physical owner backing remain separate;
neither is a total device VRAM census.

All Release shader diagnostics return `available:false`, `count:null`. They provide no
clean-shader verdict. Separate final Debug results are below. Settings, scheduled
graph topology and AS presence alone are never promoted to pixel-correct consumption.

## Debug shader reachability

Four Deferred Debug captures use one binary (exact hash and each dirty source diff
are in the ledger). Each retains 33 PNGs and before/after shader diagnostics.

| Backend / family | Before and after shader channel | Screenshot freshness | Controller result |
|---|---|---|---|
| OpenGL vegetation | available true, count 0, ready | 22/33 reported stale | Exit 0; tracked set reports no errors |
| OpenGL groom retry | available true, count 0, ready | 0 stale / 0 nonadvancing | Exit 0; tracked set reports no errors |
| Vulkan vegetation | available false, count null, notInitialized | 27/33 stale; 5 nonadvancing | Expected exit 2: capability unavailable |
| Vulkan groom retry | available false, count null, notInitialized | 0 stale / 0 nonadvancing | Expected exit 2: capability unavailable |

All 132 PNG hashes match. Stale vegetation screenshots are retained but are not
accepted as fresh visual checks. Shader reads are separate diagnostic responses;
zero reported errors on GL applies to the tracked set. Native logs for these four
captures have no `ERROR`, `VUID` or shader-failure matches. Vulkan tracking is compiled
in but uninitialized: OpenGL-only tooling initialization and missing Vulkan shader
registration leave this channel unavailable. Log absence does not supply a Vulkan
tracked-shader clean verdict.

The initial GL/Vulkan groom attempts hit the 300-second private MCP startup deadline
during cold shader compilation. Their logs remain retained and they supply no accepted
cell. Retries used 900 seconds with the same staging/cache. Debug captures provide no
performance comparison. Regions and fresh-process paired tails remain pending.

## Region integration diagnosis and green regression

Both native dumps read address zero in `UpdateImpostorAtlas+3651`, old source line 2583,
with `movq (%r14), %rax` and R14 zero. PDB GUID
`11A621E9-A0F7-265D-4C4C-44205044422E`, age one, matches both loaded-image UUIDs.
The shared stack runs through GenerateInstances, ProcessScene3DSharedLogic,
RenderScene3D, OnUpdateEditor, EditorLayer::OnUpdate and RenderFrameLayers. Prepared
CPU payloads are nonnull. OpenGL typed locals have 75,438 indices but empty material
parts and null bake VBO; optimized `this=0` and uncaptured heap values are not evidence.

The upload-refused prepared-layer rebuild test reproduced SEH `0xc0000005`: 1 failure
in 1.747 seconds. After the correction, it passes 1/1. The current layer must own
published VBO/IBO/material parts with matching mesh path; a sibling Ready payload is
insufficient. Baking consumes that GPU geometry and retained MeshBounds after CPU
payload release. Material rebakes wait for admitted alpha-cutoff mip updates, and
pressure keeps the pinned atlas/card. The regression also exercises later publication,
rebake framing/identity and pinned atlas retention. Wider GL residency is 8/8, CPU 57/57,
application smoke 8/8, all zero skipped. Release all four targets passed with 2,050
recorded dependency objects; Debug passes 2,049. Actual Vulkan shutdown, proxy and AS
checks each pass 1/1, EXERCISED, zero skipped/VUID/errors. Vegetation region reruns
remain pending; no completed full-family region campaign is claimed.

Both dumps and the original executable/PDB are preserved with exact hashes. Functional
runs overlapped Debug compilation, so their wall times supply no performance evidence.
The earlier live captures remain pre-correction records. Future completed region events
and AB/BA measurements can be added in a separately linked performance document;
no timing comparison is invented here.
