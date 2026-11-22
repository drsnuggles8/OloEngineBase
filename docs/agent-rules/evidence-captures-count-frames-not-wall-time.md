# An evidence capture never sits a number of frames that depends on wall time

**Rule:** in an evidence test, wait for a streamed level or any worker load without ticking, then
tick a bounded number of frames, or tick until it has landed and settled before the frame-counted
arms begin. Never tick inside a wall-clock wait and then capture: every tick advances the pose,
the simulation and the temporal history, so the captured frame depends on how fast the load ran.

- To wait for a load, drain it without ticking: `RepresentationStreaming::Get().GetStats()` while
  `PendingLoads > CompletedUnretrievedLoads`, then tick at most a fixed count until the pass draws
  it (`GroomSimulationVisualEvidenceTest.BoundMovingCoatDemotesAndReloadsWithAccountedGpuState`).
- To begin arms on settled content, tick until the groom pass's `GetStreamingStats()` reports
  nothing pending and no fallback draw for a few frames in a row, within a deadline, before the
  first frame-counted step (`DogPlayEvidenceTest::SettleStreaming`).

## Why

A streamed groom level is prepared on a worker in wall time. A test that ticks while it waits, or
that counts N frames after a camera or settings change, meets the hand-over at a frame index set
by how long each frame took. So the result changes with the binary's speed, with other load on
the GPU, and from run to run.

## How it showed up (#1594)

- `DogPlayEvidenceTest.TheHandPartsTheCoatUnderItAndNowhereElse`'s upscaled cell had a noise
  floor that drifted run to run (#1692). On #1594's branch it read 4,437-10,280 changed pixels
  against master's 907-2,250, with every deterministic cell bit-identical. Per-tick logs showed
  why: the coat's full strand level landed at frame 69 of the cell on master and at frame 93 on
  the branch, whose frames ran faster. The second landing fell 10 frames before the "off" capture,
  with the temporal upscaler still converging on the new strands. With `SettleStreaming` both
  binaries give the same counts to the pixel over ten runs.
- `GroomBoundStreaming_GL_Deferred_*` differed from master in a different phase each sweep (Detail
  and Pressure in one, Reload in the next). Pending, captured before any wait, never did.

The trap cost hours because the branch really did change frame cost: a faster binary looked like
a noisier coat. Tabulate every cell first. When all deterministic cells are bit-identical and only
the temporal or post-wait cells move, suspect timing before the change.
