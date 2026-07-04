# Render-pipeline caches must invalidate on every reconfigure, not just on a fingerprint change

Short rule for anyone adding a **process-wide cache** to `Renderer3D::s_Data`
(the `RenderPipeline`, the `RenderGraph`, or anything with their lifetime).

## The trap

`Renderer3D::s_Data` and everything it owns — `s_Data.Pipeline`
(`RenderPipeline`) and `s_Data.RGraph` (`RenderGraph`) — live for the whole
process: created once in `Renderer3D::Init`, torn down once in
`Renderer3D::Shutdown`. In the test binary that means **one instance shared by
every test**. A cache stored there therefore survives across path switches,
scene loads, and (in tests) across `TEST_F`s.

A `ConfigureRenderGraph` / path switch / AO-technique switch rebuilds the graph
topology via `RenderGraph::ResetTopology()`, which **wipes** the blackboard
(`m_Blackboard`) and the imported-resource maps. Any cache that assumes those
survived between calls is now stale — but a cache keyed on a hash of
*scene/settings inputs* can't see the wipe, because the inputs are identical.

That is exactly issue **#530**: the second consecutive `RenderingPath::Deferred`
entry in one process recomputed the *same* blackboard-populate fingerprint as
the first, so `PopulateBlackboard` short-circuited past the freshly-wiped
blackboard, every pass's `Setup()` then read empty handles, `RGBuilder`
silently dropped every declaration, and the whole 37-pass graph culled
(`reads=0/writes=0`) — a blank frame with no GL error.

## The rule

**A cache whose validity depends on the blackboard / imported-resource maps
surviving must be invalidated by the same event that wipes them — a topology
reset — not merely by a change in the inputs it happens to hash.**

Prefer coupling the cache key to the structural event over enumerating call
sites:

- `RenderGraph::GetTopologyGeneration()` is a monotonic counter bumped by every
  `ResetTopology()` / `Shutdown()` (the two places that wipe the blackboard).
  Hash it into any per-frame fingerprint (`ComputeBlackboardFingerprint` does)
  and the cache self-invalidates on **any** reconfigure — including a *future*
  reconfigure path that forgets to call an explicit invalidation hook.
- An explicit `InvalidateBlackboardCache()` at each settings-change site is the
  fragile alternative: it only invalidates the paths you remembered to wire.

If you add a new process-wide render cache, ask: *what wipes the state I'm
caching, and does my cache key move when that happens?* If the answer is "a
topology reset", fold `GetTopologyGeneration()` into the key.

## Guard

`RenderGraph.ResetTopologyAdvancesTopologyGenerationForCacheInvalidation`
(CPU, `RenderGraphTest.cpp`) pins the generation-bump contract.
`DeferredOccludedInstanceFieldScene.ReenteringDeferredPathDoesNotCullEntireGraph_Issue530`
(GPU, `OcclusionCullDeferredVisualEvidenceTest.cpp`, SKIPs headless)
reproduces the full reconfigure→reconfigure→rerender sequence and fails on a
blank frame if the cull returns.
