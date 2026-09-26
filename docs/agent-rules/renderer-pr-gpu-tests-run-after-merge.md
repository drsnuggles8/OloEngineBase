# A renderer PR's GPU tests run after it merges, if at all

**Run the GPU suites a renderer change reaches on your own box before the PR, and list them in its
verification matrix: PR CI runs none of them.** When the change fixes a defect in what a neutral
input reads (an open plane, an unlit surface, an empty buffer), also run every test that *measures*
that feature, because some of them may have been calibrated on the defect.

## What CI actually runs

| suite | PR CI | first automated run |
|---|---|---|
| GL evidence / property tests (`*Evidence*`, `CrossPathLightingMatrix`, goldens) | hosted runners have no GPU: they **skip**, and the job is green | the nightly AMD conformance job (`gpu-conformance-amd.yml`) and the llvmpipe nightly (`cross-vendor.yml`), after the merge |
| Vulkan device tests (`VulkanPassSuite.*`, `*Device.*`) | skip | **none**: the lavapipe nightly skips them too, since it fails the ADR 0010 contract ([vulkan-software-driver-ci.md](vulkan-software-driver-ci.md)) |

A green PR says nothing about any of these. A `Windows` run lists its skipped cases in the job
summary; read it once to see how much of your change's surface it covers.

## What to run for a renderer PR

For the feature you touched, at least:

- `--gtest_filter='*<Feature>*'`, which catches its `*Evidence*`, `*Math*` and parity suites;
- `VulkanPassSuite.<Feature>*` on a Vulkan-capable box;
- `AllRowsAllArms/CrossPathLightingMatrix.*/<Row>*`, for every row whose term the change moves;
- every other suite whose scene uses the feature, even though it is not named after it (VRCS reads
  GTAO's buffer and names neither).

Then read the numbers, not only the verdicts (next section).

## Two ways a feature's tests go stale when the feature gets *more* correct

**A test calibrated on the defect.** #1482 (issue #1463) made GTAO read an unoccluded plane as
exactly 1.0. Before it, a floor at a grazing angle read 0.7 to 0.3. Two tests had been using that
phantom occlusion as their signal:

- `VRCSVisualEvidenceTest` measured tile seams on "open floor, smooth by construction". The
  gradient it relied on was the phantom occlusion. With it gone, the band held one constant value
  and the seam ratio was undefined. Fix: measure on the one real gradient in the frame (the contact
  falloff in front of the cube) along the axis it varies in, and check that a synthetic seam
  lattice still trips the metric (it read 5.0 against a pass line of 1.8).
- The `ScreenSpaceAO` row of `CrossPathLightingMatrix` looked straight down at a 2 m wall. From
  above, GTAO sees only the wall's top face, and every sample on it is beyond the 0.5 m radius. The
  row's small nonzero term had been phantom floor occlusion; after the fix it read exactly 0.
  Fix: a 0.25 m kerb, whose top face lies inside the radius.

**A defect the old defect masked.** #1482 also made
`VulkanPassSuite.GtaoIsOpenOnUniformDepthAndDarkensACrease` fail, and that test was right. GTAO
clamped off-screen samples to uv [0, 1]. The HZB is sampled with the texture's own sampler,
`Repeat` by default. Wherever the viewport reaches the texture edge (uv 0, and every side of a
power-of-two viewport), a linear fetch therefore blended in the opposite edge's depth. That drew
phantom occlusion along the screen border: 103/255 against 240 on the test's top row. The old
horizon convention had hidden it. Fix: near the texture edge, `SampleHZBDepth` reads each trilinear
level with a four-tap `texelFetch` whose taps are clamped to that level. The first attempt clamped
both levels at the coarse level's half-texel. The device test passed, but a live sloped floor showed
the fine level reading 16 px inside the frame and darkening the border (173 against master's 176).
A flat test input cannot catch that; a live A/B against the base shader did. Review then found why
a sampler-based clamp is unsafe on GL: the pooled HZB has no mipmap min filter there, so every
`textureLod` on it reads level 0.

How the three were told apart: a per-pixel CPU port of `GTAO.comp` (the mirror in
`GTAOMathTest.cpp`) was run against the device test's exact input. Its pre-#1463 arm reproduced
the old recorded readings to within 3/255, which is what made its prediction for the fixed
shader worth trusting. The GPU matched the prediction at the crease and diverged at the border,
and that divergence located the third bug.

## A separated term of exactly zero passes every cross-arm check

The CrossPath row read 0 on all six GL arms. Only the reference arm failed, on its minimum-signal
check. The other five compared 0 with 0 and passed. The failure report named one arm, so the fault
looked path-specific; it was not. When a matrix row fails on its reference arm, read the `[matrix]`
lines of the other arms before deciding the problem is local to that arm.
