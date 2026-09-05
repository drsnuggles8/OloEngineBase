# Verifying a diagnostic against a live scene: measure the noise floor first

Distilled from the issue #607 region-capture slice, where the obvious live check
looked rigorous, ran green, and proved nothing.

## The trap

A new read-back tool needs verifying against the running editor, and the obvious
check is a self-consistency one: capture the whole target at 1:1, capture a
sub-rect, assert the sub-rect's pixels equal the corresponding part of the full
image. If the coordinate math (here: a top-left-origin rect through GL's
bottom-up rows) were wrong, the crop would come from the wrong rows and the
comparison would fail.

It doesn't work, because **the two captures are different frames**. On the
vehicles/water sandbox scene, two consecutive whole-target captures of the *same
96×64 rect* of `SceneColor` differed by up to **185/255 per channel**, every
pixel. FFT water, temporal accumulation and auto-exposure all move between
frames.

The measured result:

| comparison | maxChannelDiff |
|---|---|
| correct position | 166 |
| deliberately **mirrored** position (the control) | **70** |
| noise floor (same rect, two full captures) | 185 |

The wrong position scored *better* than the right one. A test written without
the noise-floor control would have printed PASS for a correct implementation and
would equally have printed PASS for a broken one — and a green result that
proves nothing is worse than a red one, because the next investigation trusts
it.

## The rules

1. **Measure the noise floor before attributing any difference.** Capture the
   same thing twice, compare, and report that number alongside the finding. If
   the floor is comparable to the signal, the run is INCONCLUSIVE — say so and
   exit non-zero-but-distinct; never fold it into a PASS.
2. **Include a control that must fail.** Compare against the position a broken
   implementation would have produced (here: the mirrored row range). A check
   where both the right and wrong answers pass is not a check. This is the same
   rule as "an A/B lever must PROVE it acted" in
   [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md).
3. **Pin coordinate/format math deterministically instead.** Upload a synthetic
   texture whose value *encodes its own coordinates* (`R = x/(W-1)`,
   `G = y/(H-1)` with y top-down), read it back through the real path, and assert
   per texel. Now a mirrored row range shifts G by ~114 levels against a
   tolerance of 1 — it cannot pass by accident, and it runs in CI on any GL
   machine. That is `OloEngine/tests/Rendering/CaptureRegionReadbackTest.cpp`.
   Choose the probe rect **off-centre and off-axis**: a centred or full-height
   rect makes a vertical mirror cancel out, so such a probe exonerates the bug
   it exists to catch.
4. **Live verification still earns its keep** — just for different questions.
   On this slice it caught a real bug no unit test would have: the tool reported
   an empty transient-pool acquire order, because `TransientPool::ReleaseAll()`
   empties the acquired lists at end of frame and *every* MCP read marshals onto
   the game thread at a frame boundary. Live runs answer "is it wired up, does
   it see real data, does the reply make sense"; deterministic tests answer "is
   the math right".

## The same rule, inside a capture test

A multi-angle visual-evidence test must measure its own noise floor and assert
against it, for the same reason a live diagnostic must: without it, a set of
captures that are all the *same frame* is indistinguishable from a set that
covers the scene. Issue #931 is that failure. A test posed its camera with
`EditorCamera::SetPosition`/`SetYaw`/`SetPitch`, which at the time only stashed
the members and never rebuilt the view matrix, so every "angle" rendered from
the constructor's default orbit view. Over an open sea the result is a perfectly
plausible frame of sky and water with the subject a quarter of a kilometre
off-screen — three identical PNGs that would have been baked as goldens and gone
green forever. It was caught only because that test happened to measure its floor
and happened to mask for its subject's colour.

Both checks now live in
[`VisualEvidenceGuards.h`](../../OloEngine/tests/Rendering/PropertyTests/VisualEvidenceGuards.h):
`ExpectCapturesAreDistinct` (every pair of poses differs by more than a multiple
of the *measured* floor) and `ExpectFrameHasSubject` (a content mask finds the
subject). Call both from any capture test with more than one pose. Rule 2 above
applies to the guards themselves — `MeshVisibilityEvidenceTest` hands the
distinctness guard one pose captured twice and requires it to fail.

## The suite's own evidence PNGs are nondeterministic — measured values

`OloEngine-Tests.exe` **rewrites** many of the PNGs under
`OloEditor/assets/tests/visual/` on every run. So `git status` after a test run
routinely shows a handful of modified PNGs that have **nothing to do with your
change**, and the tempting readings are both wrong: committing them as
"regenerated evidence" launders noise into the repo, and reading them as a
regression sends you debugging a change you did not make.

**That directory holds two kinds of file, and only one of them is disposable.**
A test that writes unconditionally is producing *evidence* — nothing compares it,
and a modified one after a run is noise. A test that writes only under
`--olo-golden-rebase` (`Water_*`, `Drift_*`, `MeshVisibility_*`, and the rest of
the sibling set) is producing a *golden*: a normal run reads it back and fails on
drift, so deleting or overwriting one removes a baseline the suite needs. The
`git checkout --` advice below is safe for both, because a golden is never
written by a normal run in the first place — a golden that shows as modified
means you passed `--olo-golden-rebase`, and then the question is whether you
meant to.

Measured on one machine (RTX 4090, Debug) by running the identical binary
twice and diffing run 1 against run 2 — i.e. with *no code change at all*:

| Evidence PNG | Run-to-run delta (same binary) |
| --- | --- |
| `Fluid_Side` / `Fluid_ThreeQuarter` / `Fluid_Waterline` | 0.26–0.53% of pixels, max channel 19–28 |
| `WorldOriginRebase_rebased_after` | **4.43% of pixels, max channel 111** |
| `WorldOriginRebase_far_before` | 0.18% of pixels, max channel 8 |
| `VirtualGeometry_Debug_ClusterId` | 2 pixels, max channel 158 |
| `OcclusionCull_Deferred_VisualEvidence` | 0–1 pixels, max channel 8 |

Two things to take from the table. The GPU fluid solver is genuinely
order-nondeterministic, so its three images always differ. And
`WorldOriginRebase_rebased_after` moves **4.4% of its pixels between two runs of
the same executable** — larger than most real regressions would be, which is
precisely the situation §The trap warns about.

**The procedure, and it is cheap.** Stash nothing; just take three snapshots:
`HEAD` (via `git show HEAD:<path>`), your run, and a *second* run of the same
binary. Then compare `HEAD→run1` against `run1→run2`. If the first is not
clearly larger than the second, you have measured noise. On the descriptor-heap
work this was unambiguous: two images landed **byte-identical to HEAD** on the
second run after "differing" on the first, and the largest `HEAD→run1` delta was
smaller than that image's own `run1→run2` delta.

When the answer is noise, `git checkout --` the directory. A minimal diff is
worth more to a reviewer than a folder of re-encoded PNGs.

## Corollary: intermediate targets do not look like the frame

Do not sanity-check a crop of `SceneColor` against an `olo_screenshot`. The
screenshot is `UIComposite` — post-fog, post-tonemap, at the viewport's
resolution — while `SceneColor` is pre-fog HDR clamped to [0,1] and may be at a
different render scale (measured: 1411×942 SceneColor vs a 941×628 viewport).
They are legitimately different images of the same scene; a crop that "looks
wrong" against the screenshot is usually just the wrong reference. Compare a
target against **itself**.

## Corollary: check the editor is running frames BEFORE you interpret anything

Everything above assumes the frame you are looking at is *this* frame. A minimized
editor breaks that assumption while leaving every symptom intact (issue #607).

`Application::Run` guards the entire layer-update / ImGui / render block with
`if (!m_Minimized)`, so an iconified window never reaches `EditorLayer::OnUpdate`:
the frame counter stops, the synthetic-input queue never drains, and the viewport
framebuffer keeps whatever was last drawn. But `MarshalRead` still works — game-thread
tasks are pumped *before* that guard — so **every read tool answers normally**, HTTP
200, no error, plausible numbers. Two `olo_screenshot` calls either side of a 10 m
walk came back byte-identical, which reads as "the camera didn't move": the exact
opposite of the truth. It took an out-of-band `IsIconic()` P/Invoke and a CPU-time
delta (0.17 s vs 0.69 s per 3 s) to see it, because nothing in the MCP surface
exposed window state.

This is the general shape worth remembering: **when a subsystem's read path and its
refresh path are gated by different conditions, the read path will confidently
report stale state.** Look for the asymmetry rather than for an error.

The in-band signals now exist, so use them:

- `olo_perf_snapshot` carries a `liveness` block (`ticking`, `frameIndex`,
  `msSinceLastFrame`, `iconified`, `focused`). One call, and it distinguishes
  *stalled* from *slow* — a frame index alone cannot, without a second sample.
- `olo_screenshot` sets `stale: true` and emits a leading `STALE FRAME:` text block
  ahead of the image, and reports the `frameIndex` the capture came from. Two
  captures with the same `frameIndex` are the same frame.
- `olo_input_inject` refuses up front instead of queueing a plan that can never
  drain (which used to leave the queue occupied, so every later call blamed the
  earlier one).
- `driver.ps1 -Action attach` un-minimizes the editor it launches.

Note `focused` is reported but gates nothing: injection feeds the editor's own
GLFW/ImGui stream, not the OS input queue, so an unfocused editor is perfectly
drivable. Only iconify (and a genuinely stalled frame clock) stop things working.
