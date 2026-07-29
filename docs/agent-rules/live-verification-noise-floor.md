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

## Corollary: intermediate targets do not look like the frame

Do not sanity-check a crop of `SceneColor` against an `olo_screenshot`. The
screenshot is `UIComposite` — post-fog, post-tonemap, at the viewport's
resolution — while `SceneColor` is pre-fog HDR clamped to [0,1] and may be at a
different render scale (measured: 1411×942 SceneColor vs a 941×628 viewport).
They are legitimately different images of the same scene; a crop that "looks
wrong" against the screenshot is usually just the wrong reference. Compare a
target against **itself**.
