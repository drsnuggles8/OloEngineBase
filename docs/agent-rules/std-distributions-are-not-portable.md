# Never seed procedural content with a `std::` distribution — the engine is portable, the distribution is not

`std::mt19937` is specified bit for bit. `std::normal_distribution`,
`std::uniform_real_distribution` and every other distribution are specified only
by their *output distribution*, never by their algorithm, so libstdc++ and MSVC's
STL turn one seed into two different sequences. Anything generated from one is a
different thing on Linux and on Windows, from identical source.

Write the transform yourself over the raw engine output. Box-Muller for a
Gaussian is six lines and is exact to the last bit of the uniforms:

```cpp
std::mt19937 rng(seed);
const auto uniform01 = [&rng]                     // (0, 1], so log() never sees 0
{ return (static_cast<f32>(rng() >> 8) + 1.0f) * (1.0f / 16777216.0f); };
const f32 radius = std::sqrt(-2.0f * std::log(uniform01()));
const f32 theta  = 6.28318530717958647692f * uniform01();
```

`std::shuffle` and `std::sample` carry the same defect for the same reason.

## What it cost (issue #1015)

`GenerateSpectrumNoise` drew the ocean's Gaussian spectrum with
`std::normal_distribution`, under a comment promising that a given seed
reproduces a given sea. It did — *per platform*. At the same amplitude from the
same commit, displacement peaked at 3.4 m on the RTX 4090 and 4.6 m on the AMD CI
box.

The ocean spectrum evidence test then stood in that gap. Its camera sat at 3.0 m,
under crests that reach 4.6 m on one platform and not the other, so whether the
eye was above the surface or **inside a wave** was decided by the random draw. An
underwater frame is a near-uniform fogged blue that no two spectra can be told
apart in, so the failure arrived as *"Phillips and JONSWAP are nearly identical"*
— which points at the spectrum selector, the one part that was working. Five
hypotheses about Mesa were tested and discarded before the cause turned out not
to be a GPU difference at all.

The wider damage is quieter: while the sea differs by platform, **no cross-vendor
image comparison of it can mean anything**, because the two vendors are not
looking at the same ocean.

## How to catch it

- **Two vendors disagreeing about procedural content is a seed question first.**
  Before suspecting the driver, check whether both platforms are generating the
  same data. Reading the generated buffer back settles it in one run: on the box
  the ocean's displacement texture was healthy at every amplitude — GPU matching
  CPU to five decimals, exactly linear, no NaN — which ruled out the entire GPU
  half of the search space at once.
- **Vary the seed as a diagnostic.** Rendering five different seas at the failing
  pose showed two collapsing and three fine on the same GPU. A defect does not do
  that; a knife edge does.
- **Guard the generator by re-deriving it, not by pinning constants.** Constants
  drift an ULP with libm and explain nothing.
  `OceanSpectrumNoise.IsPortableBoxMullerAndNotALibraryDistribution` recomputes
  the intended Box-Muller in the test, so a swap back to a library distribution
  fails immediately and says why.

## The second rule this taught

**A visual test must not sit within a wave height of the water.** State the
clearance and assert the consequence: both spectrum frames now assert a minimum
luma spread, so a submerged camera reports *"this frame has no wave relief"*
rather than failing an opaque frame-difference comparison. The same applies to
any statistic of one random draw — two cascade assertions here were comparisons
between single seas, one of them landing 0.03% on the wrong side of its bar when
the draw changed. They average over five seas now, which is what they were always
trying to measure.
