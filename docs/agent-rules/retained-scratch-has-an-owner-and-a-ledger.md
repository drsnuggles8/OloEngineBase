# Retained scratch has an owner and is counted once: never a function-local `thread_local`

**The rule:** working storage that keeps its capacity across frames is held by an owner,
which passes it in, gives it back by a stated rule and with itself. It is counted in the
memory report exactly once. A function-local `thread_local std::vector` meets neither
condition. It reuses capacity, but it is retained storage that nobody owns and no report
sees, and it lives until its thread exits.

## What happened (#1533 review)

The dog's caster pose (`PoseGroomCasterRunsBySurface` / `ByRoots`) reused its skinned
corners, per-triangle frames and per-piece sums from function-local `thread_local`s. The root
evaluation (`EvaluateGroomRootTransforms`) did the same with the skinned surface. Both avoided
per-frame allocation. But the groom memory breakdown counted the caster tables, the posed runs
and the pass's own scratch, and missed these. They also outlived every groom and pass that had
used them. Seven such vectors had come in with the same branch.

## The shape that replaced them

- **An owned workspace, passed in.**
  - `GroomCasterPoseScratch` is held by `GroomRenderPass`. Its arrays are private (pimpl) because
    their element types belong to the evaluation.
  - `GroomSurfaceSkinScratch` is held by `GroomRenderPass`, by `Scene` and by `GroomSurfaceCache`.
  - Each instance serves one call at a time. The evaluation's workers read the caller's instance.
- **A release rule per owner.**
  - The pass releases its scratch with its other pose scratch, once unused for
    `kScratchIdleFrames` frames or on a frame with no bound groom.
  - The scene releases its scratch when its last bound groom's state goes.
  - Every owner releases its scratch in its destructor.
- **One ledger.** Every instance publishes its capacity after each use and on release
  (`GroomEvaluationScratchRetainedBytes`). The memory report's evaluation-scratch row reads that
  ledger. No owner adds its scratch to its own rows, so a scratch shared by every groom an owner
  evaluates counts once. The pass's own figure is `MemoryBreakdown::CpuEvaluationScratchBytes`,
  which is kept out of `CpuBytes()` because `CpuBytes()` is the pass's CPU row.
- **The one-off overload.** `EvaluateGroomRootTransforms` without a scratch gives the call its
  own scratch, which dies with the call. Tests and bakes use it. A per-frame caller passes the
  scratch it owns.

## How to check a new one

A test should show four things:

1. Growth shows in the ledger.
2. A steady frame grows nothing (`Growths()`) and produces the same output.
3. Two uses through one owner count once.
4. Release and destruction return the ledger to its baseline.

`GroomGpuDeformation.TheCasterPosesScratchIs…` and `…SkinScratchIs…` check these at unit level.
`GroomBindingVisualEvidenceTest.TheEvaluationScratchIsReportedOnceAndGivenBackWhenItIdles`
checks them through the pass and the report. With the idle release taken out, that last test
fails: 37,248 bytes stay held in the pass and in the ledger.

See also [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md), which covers when to promote a
scratch to persistent state in the first place.
