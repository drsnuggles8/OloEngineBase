# A motion claim is measured on runtime frames, and a pair comes from one held state

**Rule.** A test that claims something about a coat in motion (shimmer while walking, the coat
staying on through a clip, the long hair swinging) renders and reads back **runtime** frames:
`RunFrames`, through the scene's runtime camera, one frame per readback. A test that compares two
arms (coat on and off, a lever on and off) renders both from **one held state**: the scene paused
(`Scene::SetPaused`, the engine's own gate), and `Renderer3D::ResetFrameSequences` plus a cold
history before each arm, so the arm is the only difference. The arm that ships goes first.

**Why.** The dog showcase (#1533) had three tests that passed without testing what they named.

- **Editor frames do not step the guide solver.** `Scene::OnUpdateEditor` animates the skeleton
  but runs the coat's solver with a zero clock (#1250), so the coat rides its roots rigidly. The B6
  shimmer test "in motion" read back editor frames, so the walking coat it measured never swung.
  The fix measures runtime frames and checks they were alive: the clip time advanced, the solver
  stepped, the long hair moved against the targets its roots carry (`Scene::FindGroomGuideSimulation`,
  the particles against `LastTargets`). A second test switches the solver off and checks that this
  liveness check then fails.
- **Each editor capture advanced the clip.** `CoatPixels` captured the coat-off arm, then the
  coat-on arm, each after 24 editor frames with the clip playing. The two arms showed two different
  poses, and the difference was counted as "the coat's own pixels". The fix measures coverage from
  the coat's entity id in a single frame, and takes beauty pairs from a held state.
- **Hiding the coat re-seeds it.** A coat that is not submitted for a frame misses a deformation
  revision. When it comes back, its binding has no history and the solver re-seeds at the groomed
  shape, without the sag or the drape. With the scene held (zero clock) it stays that way. So every
  arm captured after a coat-off arm drew a different coat. The fix captures the shipped arm first and
  replays the clip (`StartClip`) before the next pair.

**Two more traps on the way.**

- **A replayed arm crosses an LOD step the other did not.** The guide budget follows the LOD, and a
  budget step re-selects the simulated guides. Settle the LOD at the view with held frames before
  every arm, so every replay starts from the same tier and budget. Then compare the replays' clip
  times and solver steps to show the state really was the same.
- **A frame-to-frame metric in motion measures the motion.** `MeasureShimmer` is defined on static
  sequences. In a walk, compare two draws of the same frames instead (the same clip times, every
  jitter and stochastic sample shifted by held frames before the clip starts) and check lag
  separately. See `DogShowcaseEvidenceTest`. Measured on the dog's walk from behind: the
  frame-to-frame change of the mean of six no-history draws, which has no lag and 2.4x less noise,
  is 0.59 of one draw's at the sparse fringe (`OLO_DOG_B6_MOTION_FLOOR=1`), so no resolve that
  keeps up with the walk gets that metric under half there. B6 asserted it in the walk anyway, and
  passed only while the coat's motion never reached the resolve (#1552).

See also [substituted-seams-compound.md](substituted-seams-compound.md) and
[visual-quality-criteria.md](visual-quality-criteria.md).
