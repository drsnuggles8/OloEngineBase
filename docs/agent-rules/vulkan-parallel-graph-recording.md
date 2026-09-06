# Whole-pass parallel recording

Prepare each pass on the caller, record only frozen inputs, and publish after
joining. The underlying worker contract is in
[vulkan-parallel-recording.md](vulkan-parallel-recording.md).


`RenderGraphNode::PrepareParallelRecording` runs on the caller and returns an `RGPreparedPass`:
resolved physical resource uses, an independently recordable body, optional ordered publication,
and the largest model-instance upload it needs. `SupportsWholePassRecording` opts a node into
planning; an empty prepared body declines that frame. Resolve failures and lazy GPU allocations
belong in preparation. Clears and GPU copies belong in recording after incoming graph barriers.

`SubmissionCommand::RecordingGroup` and `RecordingLane` describe CPU recording ownership.
They do not select a GPU queue. The planner groups adjacent independent eligible passes, up to
16, and stops at dependency, fence or async-batch boundaries. The executor checks resolved
physical identities as well: a transient alias with a writer declines the group even when its
logical resource names have no edge. Read/read sharing is legal only after preparation freezes
all lazy backend state. In particular, call `UniformBuffer::PrepareForParallelRead` on every
captured shared UBO, including buffers displaced from their binding slots before the fork.

Preparation must be repeatable: a later member can decline or reveal an alias, after an earlier
member has prepared. Ordinary execution then prepares that earlier pass again. Keep preparation
to resolution, idempotent allocation, private snapshots and upload priming; advance counters and
publish shared settings only when the prepared body executes or publishes. A shared CPU datum
consumed after another pass changes it needs a graph dependency just as a GPU resource does;
physical GPU identity checks cannot infer that dependency.

Each item owns an `RGCommandContext` with its own active-pass name. Workers cannot resolve
graph resources: const resolver signatures can still mutate registry caches and diagnostics.
`RecordParallelOrdered` joins recording, then executes the secondaries in original pass order,
with primary-owned timestamp brackets and publication around each one. Consumers prepare only
after that join. A post-pass capture hook retains the ordinary executor and every original
capture boundary.

Prepared fullscreen and MRT nodes share one recording helper, including EASU and
DepthVelocityUpscale. SSAO keeps its raw/blur/export chain in one item. Froxel fog
snapshots clustered-light buffers and private parameters, then publishes history
only after the scatter/integrate recording joins. GTAO keeps its dependent HZB/classifier/
AO/denoise/copy chain inside one item, with private parameter uploads and ordered settings
publication. VSM page marking uses its own prepared globals and declares its page/request/
diagnostic writes; it can record alongside independent GTAO work in the same compute batch.
Nested intra-pass regions execute inline within their owning item and retain its state ownership.
Async GPU queue scheduling remains the separate #808 contract.

Validation of the new whole-pass paths and Release measurements are in progress for #1013;
the pass audit records the remaining evidence. Source eligibility is not a performance claim.

## Scheduling before resource planning

The topological scheduler groups ready prepared nodes of the same work type and
async-compute classification. A lone prepared node can wait while ordinary ready
producers advance toward an independent partner: particle color can complete
before DepthVelocityUpscale and EASU record together. Cohorts are fixed before
emitting any member, so newly unlocked consumers cannot enter their producer's
cohort. If no ordinary node remains, draining one ready prepared node guarantees
progress. Enabled state and nonempty Setup access declarations exclude disabled
candidates; compiled reachability also excludes culled submission-plan members.

This scheduling runs before reachability, barriers and transient lifetime/alias
planning. Never reorder only the final submission commands: their resource
lifetimes were compiled against an earlier order. CPU-only publication dependencies
must be declared explicitly, just like GPU dependencies. GPU draw order within
blended packet ranges remains unchanged.

Live Vulkan evidence includes DepthVelocityUpscale/EASU, SSAO/DepthVelocityUpscale,
and VolumetricFog/GTAO/VSM-marking groups. Their off/on beauty captures match exactly
with zero shader errors and recording conflicts. Passes retain inline execution
when dependencies, physical aliasing, queries, capture hooks or frame readiness
prevent a safe group. The [pass audit](vulkan-parallel-pass-audit.md) records the
remaining sequential dependency chains and existing backend limitations.
