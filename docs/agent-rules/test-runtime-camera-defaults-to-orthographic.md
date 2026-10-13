# A test's runtime camera must set its projection

**When a fixture adds a `CameraComponent` that `RunFrames` renders from, call
`Camera.SetPerspective(fov, near, far)` (or `SetOrthographic` with a depth range that covers the
scene).** A default `SceneCamera` is orthographic, size 10, with a near and far of -1 and +1. From a
few units away it sees nothing in front of it: anything farther than one unit along its view axis
is outside the frustum and is culled before any pass sees it.

## What it costs

A culled groom or plant is not an error. Its pass is skipped, its stats keep whatever the last
frame that did draw it wrote, and an assertion that reads them can pass or fail for reasons that
have nothing to do with the code under test. Found on #1595, in two fixtures that had been red on
master:

- `GroomSimulationVisualEvidenceTest` drives its motion through `RunFrames` from a camera sixty
  units back, so the solver steps. With the default projection the coat was never submitted, the
  solver never stepped, and every `GroomRenderStats` the suite read came from the previous
  editor-camera capture (`RunEditorFrames`, a different camera). So the stats lagged one arm: the
  simulated arm reported the control arm's `GroomsSimulated` of 0, and the next control arm the
  simulated arm's 1. Five of eight cases failed.
- `RendererMemoryAccountingEvidence.AGroomAndALawnLeaveNoPooledFramebufferBehind` put its tuft
  three units in front of the default camera. It passed only after another fixture had left the
  renderer drawing it, and failed alone and inside its own suite. That made an ordinary culling
  miss look like cross-test state ([cross-test-renderer-state.md](cross-test-renderer-state.md)).

## How to tell

- A stat that alternates with the previous arm's setting, rather than following this arm's, means
  the frames you think you measured did not run the pass.
- Print one runtime frame's submit count (`GroomRenderStats::GroomsSubmitted`, a plant's active
  draw info) before chasing the feature. Zero from a camera that "looks at" the subject is the
  frustum.
- A test that passes only after other tests is worth a look for a camera it never configured.

## Related

- [motion-claims-need-runtime-frames.md](motion-claims-need-runtime-frames.md): why motion tests
  must use `RunFrames`, which is what makes the runtime camera load-bearing.
- [cross-test-renderer-state.md](cross-test-renderer-state.md): the state that can make a culled
  subject draw anyway.
