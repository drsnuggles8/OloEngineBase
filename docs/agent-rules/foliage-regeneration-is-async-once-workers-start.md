# A foliage regeneration is asynchronous once any suite has started the workers

**Rule:** after setting `FoliageComponent::m_NeedsRebuild`, don't assume a fixed number of frames
shows the new layers. `RendererAttachedTest::RunFrames` and `RunEditorFrames` finish a regeneration
in flight before returning; use them. To watch a rebuild in flight, as the staged-rebuild tests do,
drive the scene through `RunEditorFramesOn`, which does not wait. A test that drives
`Scene::OnUpdate*` itself has to wait the same way: until `FoliageRenderer::IsRebuilding()` is false.

## Why

`FoliageRenderer` publishes a regeneration synchronously when the task scheduler has no workers.
When it has workers (`LowLevelTasks::FScheduler::Get().GetNumWorkers() > 0`), the rebuild is
staged: it runs on the workers and publishes some frames later, and the old layers keep drawing
until then. The workers are process-wide, and several fixtures start them in `SetUp` and never
stop them: `FoliageCostBaselineTest`, `VulkanPassSuite`, the Functional fixtures and others. So
whether a regeneration is synchronous depends on which suites ran earlier in the same process.

## How it showed up (#1590 PR)

Evidence tests that passed alone failed in a touched-area sweep: alpha-coverage warning, wind,
leaf transmission, instance identity, habitat, LOD coverage and the new lamp-atlas test. They had
nothing in common except a fixed frame count after a regeneration, and an earlier suite that had
started the workers. Some symptoms:

- the foliage-off arm drew the shipped plants, so a positive control moved 0 px;
- a "warns once per entry" counter read 1 instead of 2, because the second warning was raised at
  publication, one frame later;
- a "first frame after a representation change has zero velocity" check measured the frame
  before the change.

Which tests failed changed between two sweeps of the same binary, because the race is timing
dependent. Running the failing test after a single `FoliageCostBaselineTest`
(`--gtest_filter=FoliageCostBaselineTest.NoDensityLodLeverReachesTheCull:<test>`) reproduces it
reliably.

## What not to do

- **Don't stop the workers in a fixture's `TearDown` to make the next suite synchronous.** Other
  fixtures rely on them, and `FScheduler::StopWorkers` has its own teardown hazards (see the
  `Scheduler.h` comments).
- **Don't wait on `m_NeedsRebuild` alone.** The scene never clears it for a component it doesn't
  regenerate (no terrain, no layers, no terrain data yet; with empty layers it sets it again every
  frame), so a wait on it lasts until the timeout. Wait on `IsRebuilding()`; the first tick turns
  the flag into a rebuild in flight or a finished one.
