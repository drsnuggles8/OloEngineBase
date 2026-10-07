# Functional and shader observations (#1257)

See the [main analysis](groom-vegetation-streaming-residency-1257.md), [regions/performance record](groom-vegetation-streaming-residency-1257-performance.md) and [exact evidence ledger](evidence/groom-vegetation-streaming-residency-1257.json).

## Final-source functional matrix

Both complete Release campaigns use source `c519034ed18a0f654bc917c0e0b8c94f24dccd66`
and binary SHA-256 `8999466a6e6a874d724dbe5865cbb56b563400b12f5a580620530c85b9317272`.
Each recorded dirty-diff hash is retained. All 552 PNGs match hashes, PNG signatures and
1920x1080 dimensions; recorded metadata advances and is non-stale/ready. UUID/name sets
are unchanged, backend/path diagnostics match, and native logs have no error/fatal/SEH
or VUID matches under the audit expressions. Selected image inspection supplies no
blank-frame concern, but is not a blanket beauty or temporal verdict.

| Backend | Family | Forward / Forward+ / Deferred captures | Stable entities |
|---|---|---:|---:|
| opengl | groom | 33 / 33 / 65 | 44 |
| opengl | vegetation | 33 / 33 / 70 | 4 |
| vulkan | groom | 33 / 33 / 74 | 44 |
| vulkan | vegetation | 33 / 33 / 79 | 4 |

| Settled CSM cycle, all paths | Detail optional GPU bytes | Pressure detail / fallback | Reload |
|---|---:|---|---|
| opengl groom | 2931616 | 0 / 41 | Detail bytes/draws restored |
| vulkan groom | 2931616 | 0 / 41 | Detail bytes/draws restored |
| opengl vegetation | 21139480 | 0 / 3 | Detail bytes/draws restored |
| vulkan vegetation | 21165056 | 0 / 3 | Detail bytes/draws restored |

Every settled pressure snapshot has optional resident GPU bytes zero and physical
retiring bytes zero, while floors remain drawable. No sampled retiring peak is claimed.
Worker pendingLoads is zero: groom retains two completed payloads / 9,333,472 CPU
bytes; vegetation three / 60,796,496 bytes. These held staging leases are not in-flight
workers or GPU residency. Initial pending-opportunity samples were already settled.

Both deferred families latch MSAA four and spatial FSR. GL temporal requests resolve
temporal; Vulkan resolves spatial with `backendNotOpenGL` at the same scale. Later RT
phases upscale off/native, and do not establish temporal FSR coverage. TAA history
snapshots advance; valid history and scheduled passes do not prove ghost-free images.
VSM observations are positive, while global draw/page counts alone do not establish
every foliage shadow pixel. Earlier GL CSM/VSM and Vulkan inline VSM family brackets
retain their own provenance. Default Vulkan parallel CSM omits named family timers;
the separately forced-inline CSM check is narrower configuration evidence.

Vulkan vegetation RT detail/reload is ready/complete, represents 124/124 plants and
reports TLAS 128 / 4,203,264 AS bytes with valid positive Trace/Resolve/Filter brackets.
Pressure has noData, incomplete casters and two refusals (122/124 represented), so TLAS
counters are omitted and raster fallback is intentional. Groom detail/pressure/reload
represent all 41 coats with zero refusals, TLAS 83 / 12,047,744 AS bytes and valid trace
brackets. Diagnostics and PNGs are sequential nearby frames, not atomic samples.

The executable and 507 shaders were rehashed and match both campaigns. Asset snapshots
differ only in mutable imgui.ini and OloEngine.log; 4413 other entries agree between
campaigns. Full current asset rehash was not performed by the audit.

Historical 12-cell captures, two sparse follow-ups, Runtime runs and Debug probes remain
separate ledger entries with original hashes. Their observations are not reassigned to
the final-source binary. The wide groom view does not clearly distinguish fine fibres;
coarse foliage/dog pressure cards and sparse motion retain the art/temporal limits
described in the main analysis. Release shader diagnostics stay unavailable/null.

## Historical Debug shader reachability

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
performance comparison. Completed regions and qualified GL pairs are in the linked
performance record; paired Vulkan and accepted performance remain pending.

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
checks each pass 1/1, EXERCISED, zero skipped/VUID/errors. A separately provenanced
post-correction regional campaign now completes all four cells.

Both dumps and the original executable/PDB are preserved with exact hashes. Functional
runs overlapped Debug compilation, so their wall times supply no performance evidence.
The earlier live captures remain pre-correction records. The final-source matrix above
and linked regions/performance record supply separate newer evidence.
