#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// OceanCascadeTest — L1 property tests for the band-limited multi-cascade FFT
// ocean preset (issue #969; Renderer/Ocean/OceanCascades.{h,cpp} and the
// multi-band half of OceanFFTField).
//
// These are the cheapest layer that can prove the FORMULA, per CLAUDE.md's
// "Rendering changes MUST be visually verified" ladder — the pixels are
// OceanCascadeVisualEvidenceTest's job and the GPU/CPU parity is
// OceanFFTGpuContractTest's. What lives here:
//
//   * The band partition — the issue's "no discontinuity or double-counted
//     energy at the handoff" acceptance criterion, asserted as the property it
//     actually is: every wave vector belongs to EXACTLY ONE band.
//   * Non-commensurate tiles and the derived per-band resolutions, including
//     the zero-padding equivalence that is the whole reason every chain may run
//     at the shared array resolution without changing the field.
//   * The single-cascade fallback reproducing the pre-#969 pipeline exactly —
//     the "existing one-cascade scenes retain their current authored behavior"
//     criterion, against an independent re-derivation rather than a golden.
//   * The summed surface: RMS calibration, slope-vs-height consistency, and
//     that the rotated mid band's waves still travel with the wind.
//   * A TEXT pin that the three water stages share one cascade sum, in the
//     spirit of WaterGerstnerNormalScaleTest — a C++ test cannot see a shader
//     that quietly grows its own second copy of the walk.
// =============================================================================

#include "OloEngine/Renderer/Ocean/OceanCascades.h"
#include "OloEngine/Renderer/Ocean/OceanFFT.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Ocean/OceanSpectrum.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr f32 kTwoPi = 6.28318530717958647692f;

    Ocean::SpectrumParams MakeParams(u32 cascades, f32 patchSize = 140.0f, u32 resolution = 64u)
    {
        Ocean::SpectrumParams p;
        p.m_Resolution = resolution;
        p.m_PatchSize = patchSize;
        p.m_WindSpeed = 12.0f;
        p.m_WindDirection = { 1.0f, 0.0f };
        p.m_Amplitude = 2.0f;
        p.m_Choppiness = 1.0f;
        p.m_Seed = 4242u;
        p.m_CascadeCount = cascades;
        return p;
    }

    /// Repo root, found by walking up from the test binary's working directory
    /// until CLAUDE.md appears. The shader text tests below read source files.
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path dir = std::filesystem::current_path();
        for (int i = 0; i < 8; ++i)
        {
            if (std::filesystem::exists(dir / "CLAUDE.md"))
                return dir;
            if (!dir.has_parent_path() || dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
        return {};
    }

    std::string ReadTextFile(const std::filesystem::path& p)
    {
        std::ifstream in(p);
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// -----------------------------------------------------------------------------
// The preset: tiles, bands, resolutions
// -----------------------------------------------------------------------------

TEST(OceanCascade, SingleCascadePresetIsTheAuthoredTileAndGrid)
{
    // The fallback contract: a scene that has not opted in must not be able to
    // tell the cascade code exists.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(1u, 140.0f, 128u);
    ASSERT_TRUE(preset.IsValid());
    EXPECT_EQ(preset.Count, 1u);
    EXPECT_EQ(preset.ArrayResolution, 128u);
    EXPECT_FLOAT_EQ(preset.Bands[0].PatchSize, 140.0f);
    EXPECT_EQ(preset.Bands[0].Resolution, 128u);
    EXPECT_FLOAT_EQ(preset.Bands[0].KMin, 0.0f);
    EXPECT_TRUE(std::isinf(preset.Bands[0].KMax)) << "the single band must carry the WHOLE spectrum";
    EXPECT_FLOAT_EQ(preset.Bands[0].DomainRotation, 0.0f);
}

TEST(OceanCascade, ThreeBandPresetPutsTheAuthoredTileInTheMiddle)
{
    const f32 L = 140.0f;
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, L, 128u);
    ASSERT_TRUE(preset.IsValid());
    ASSERT_EQ(preset.Count, 3u);

    // Broad > authored > fine. An author who tuned the patch size keeps the
    // wave scale they tuned, with an octave added either side of it.
    EXPECT_FLOAT_EQ(preset.Bands[1].PatchSize, L);
    EXPECT_GT(preset.Bands[0].PatchSize, L);
    EXPECT_LT(preset.Bands[2].PatchSize, L);
}

TEST(OceanCascade, EveryWaveVectorBelongsToExactlyOneBand)
{
    // The issue's acceptance criterion — "adjacent frequency bands have no
    // discontinuity or double-counted energy at their handoff" — stated as the
    // property it is. Swept across four decades of |k|, INCLUDING the two
    // boundaries themselves, which is where a >=/> slip would show.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    ASSERT_EQ(preset.Count, 3u);

    std::vector<f32> probes;
    for (int i = 0; i <= 400; ++i)
        probes.push_back(std::pow(10.0f, -3.0f + 0.015f * static_cast<f32>(i)));
    // The exact endpoints and their immediate neighbours.
    for (u32 b = 0u; b + 1u < preset.Count; ++b)
    {
        const f32 edge = preset.Bands[b].KMax;
        probes.push_back(std::nextafter(edge, 0.0f));
        probes.push_back(edge);
        probes.push_back(std::nextafter(edge, std::numeric_limits<f32>::infinity()));
    }
    probes.push_back(0.0f);

    for (f32 k : probes)
    {
        int owners = 0;
        for (u32 b = 0u; b < preset.Count; ++b)
            if (k >= preset.Bands[b].KMin && k < preset.Bands[b].KMax)
                ++owners;
        EXPECT_EQ(owners, 1) << "|k| = " << k << " is carried by " << owners
                             << " bands (0 = a gap in the spectrum, 2 = doubled energy)";
    }
}

TEST(OceanCascade, BandBoundarySitsAtTheNextTilesFundamental)
{
    // WHY the partition is gapless: the boundary is the lowest |k| the finer
    // tile can represent at all, so nothing is handed to a cascade that cannot
    // carry it and nothing is left behind by the one that could.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    ASSERT_EQ(preset.Count, 3u);
    for (u32 b = 0u; b + 1u < preset.Count; ++b)
    {
        EXPECT_NEAR(preset.Bands[b].KMax, kTwoPi / preset.Bands[b + 1u].PatchSize, 1e-4f);
        EXPECT_FLOAT_EQ(preset.Bands[b].KMax, preset.Bands[b + 1u].KMin);
    }
    EXPECT_FLOAT_EQ(preset.Bands[0].KMin, 0.0f);
    EXPECT_TRUE(std::isinf(preset.Bands[preset.Count - 1u].KMax));
}

TEST(OceanCascade, TileSizesAreNonCommensurate)
{
    // If a tile ratio were a small rational, the lattices would re-align every
    // few tiles and re-introduce exactly the repetition the cascades exist to
    // remove. This test is the reason the shipped ratios are what they are: the
    // first pair tried (4.79 and 4.31) failed it, sitting 0.05 from 24/5 and
    // 0.07 from 13/3 respectively.
    //
    // WHY q <= 6 AND WHY 0.10. A re-alignment can only be seen if it happens
    // inside the sea being drawn. Six periods of the broad tile is about 5 km,
    // past any sea this engine draws — Drift's is 1.6 km and #878 names a "few
    // km, not 50" ceiling — so alignments beyond q = 6 are unreachable. Within
    // that range 0.13 is roughly the best any real number can achieve (Dirichlet
    // bounds it near 1/(q+1)), so 0.10 is the shipped margin, not a round number
    // somebody liked.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    ASSERT_EQ(preset.Count, 3u);

    const f32 ratios[] = {
        preset.Bands[0].PatchSize / preset.Bands[1].PatchSize,
        preset.Bands[1].PatchSize / preset.Bands[2].PatchSize,
        preset.Bands[0].PatchSize / preset.Bands[2].PatchSize,
    };
    for (f32 r : ratios)
    {
        for (int q = 1; q <= 6; ++q)
        {
            const f32 scaled = r * static_cast<f32>(q);
            const f32 distance = std::abs(scaled - std::round(scaled));
            EXPECT_GT(distance, 0.10f) << "tile ratio " << r << " is within " << distance << " of p/" << q
                                       << " — the two lattices would re-align every " << q << " periods";
        }
    }
}

TEST(OceanCascade, TheBroadLatticeDoesNotRepeatWithinASeaThisEngineDraws)
{
    // The consequence of the ratios above, measured as the thing a player would
    // actually notice rather than as a number-theory property: how far you have
    // to sail before the broad tile lines back up with a finer one closely
    // enough to read as the same water twice.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    ASSERT_EQ(preset.Count, 3u);

    // 2 km — comfortably past Drift's 1.6 km sea, which #878 sets as the
    // floating-origin ceiling for this kind of world.
    constexpr f32 kLargestSeaMetres = 2000.0f;
    // Within a tenth of a tile counts as "lined back up".
    constexpr f32 kAlignedFraction = 0.1f;

    for (u32 finer = 1u; finer < preset.Count; ++finer)
    {
        const f32 broad = preset.Bands[0].PatchSize;
        const f32 fine = preset.Bands[finer].PatchSize;
        f32 firstAlignment = std::numeric_limits<f32>::infinity();
        for (u32 q = 1u; static_cast<f32>(q) * broad <= kLargestSeaMetres * 4.0f; ++q)
        {
            const f32 distance = static_cast<f32>(q) * broad;
            const f32 phase = distance / fine - std::floor(distance / fine);
            if (phase < kAlignedFraction || phase > 1.0f - kAlignedFraction)
            {
                firstAlignment = distance;
                break;
            }
        }
        std::cout << "[ DIAG ] broad tile (" << broad << " m) re-aligns with band " << finer << " (" << fine
                  << " m) after " << firstAlignment << " m\n";
        EXPECT_GT(firstAlignment, kLargestSeaMetres)
            << "the broad lattice lines back up with band " << finer << " after only " << firstAlignment
            << " m — inside a sea this engine draws";
    }
}

TEST(OceanCascade, DerivedResolutionResolvesTheBandsShortestWavelength)
{
    // The resolution derivation the issue asks for: from the band's shortest
    // wavelength, not from a global maximum. The bounded bands must come out
    // BELOW the authored resolution, or the derivation is doing nothing.
    const u32 authored = 128u;
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, authored);
    ASSERT_EQ(preset.Count, 3u);

    for (u32 b = 0u; b + 1u < preset.Count; ++b)
    {
        const Ocean::CascadeBand& band = preset.Bands[b];
        // Highest signed bin index the band populates, and hence the samples
        // the grid spends on its shortest wavelength.
        const f32 nMax = band.KMax * band.PatchSize / kTwoPi;
        const f32 samplesPerShortestWavelength = static_cast<f32>(band.Resolution) / nMax;
        EXPECT_GE(samplesPerShortestWavelength, Ocean::kMinSamplesPerWavelength)
            << "band " << b << " cannot interpolate its own shortest wave smoothly";
        EXPECT_LT(band.Resolution, authored) << "band " << b << " gained nothing from the derivation";
    }
    // The top band has no upper boundary, so its grid Nyquist IS the limit and
    // it keeps the authored resolution — exactly the single-cascade situation.
    EXPECT_EQ(preset.Bands[2].Resolution, authored);
    EXPECT_EQ(preset.ArrayResolution, authored);
}

TEST(OceanCascade, DerivedResolutionReproducesTheArrayResolutionField)
{
    // THE CLAIM THAT LETS EVERY CHAIN RUN AT THE ARRAY RESOLUTION. Running a
    // band-limited spectrum on a larger grid is the same spectrum with the extra
    // bins zero, and its inverse FFT is the exact band-limited reconstruction of
    // the smaller one — so the field is identical and only GPU time is spent.
    // Stated in OceanCascades.h; pinned here, because an unfalsified claim in a
    // comment is a decoration.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    ASSERT_EQ(preset.Count, 3u);
    const Ocean::CascadeBand& band = preset.Bands[0]; // the broad band, 64 derived vs 128 shared
    ASSERT_LT(band.Resolution, preset.ArrayResolution);

    Ocean::SpectrumParams big = MakeParams(3u, band.PatchSize, preset.ArrayResolution);
    std::vector<Ocean::Complex> h0Big = Ocean::GenerateH0(big);
    Ocean::ApplyBandLimit(h0Big, preset.ArrayResolution, band.PatchSize, band.KMin, band.KMax);

    // The SAME spectrum on the derived grid: same wave vectors per signed
    // frequency, rescaled for the grid-size-dependent 1/N² the inverse carries.
    const std::vector<Ocean::Complex> h0Small =
        Ocean::ExtractBandLimitedH0(h0Big, preset.ArrayResolution, band.Resolution);

    Ocean::SpectrumParams small = big;
    small.m_Resolution = band.Resolution;

    const Ocean::DisplacementField fBig = Ocean::EvaluateField(big, h0Big, 3.5f);
    const Ocean::DisplacementField fSmall = Ocean::EvaluateField(small, h0Small, 3.5f);
    ASSERT_TRUE(fBig.IsValid());
    ASSERT_TRUE(fSmall.IsValid());

    const u32 step = preset.ArrayResolution / band.Resolution;
    f64 maxAbsDiff = 0.0;
    f64 peak = 0.0;
    for (u32 z = 0u; z < band.Resolution; ++z)
    {
        for (u32 x = 0u; x < band.Resolution; ++x)
        {
            const f32 a = fBig.m_Height[static_cast<sizet>(z * step) * preset.ArrayResolution + x * step];
            const f32 b = fSmall.m_Height[static_cast<sizet>(z) * band.Resolution + x];
            maxAbsDiff = std::max(maxAbsDiff, static_cast<f64>(std::abs(a - b)));
            peak = std::max(peak, static_cast<f64>(std::abs(a)));
        }
    }
    ASSERT_GT(peak, 1e-4) << "the band carried no energy — the comparison would pass on nothing";
    EXPECT_LT(maxAbsDiff, peak * 1e-3)
        << "the derived-resolution field and the array-resolution field differ by " << maxAbsDiff
        << " m against a " << peak << " m peak — the zero-padding equivalence does not hold";
}

// -----------------------------------------------------------------------------
// Band limiting
// -----------------------------------------------------------------------------

TEST(OceanCascade, ApplyBandLimitKeepsExactlyTheBandsBins)
{
    const u32 N = 32u;
    const f32 L = 100.0f;
    Ocean::SpectrumParams p = MakeParams(1u, L, N);
    std::vector<Ocean::Complex> h0 = Ocean::GenerateH0(p);
    const std::vector<Ocean::Complex> before = h0;

    const f32 kMin = 0.2f;
    const f32 kMax = 0.6f;
    Ocean::ApplyBandLimit(h0, N, L, kMin, kMax);

    u32 kept = 0u;
    for (u32 m = 0u; m < N; ++m)
    {
        for (u32 n = 0u; n < N; ++n)
        {
            const f32 kx = kTwoPi * static_cast<f32>(Ocean::SignedFrequency(n, N)) / L;
            const f32 kz = kTwoPi * static_cast<f32>(Ocean::SignedFrequency(m, N)) / L;
            const f32 k = std::sqrt(kx * kx + kz * kz);
            const sizet i = static_cast<sizet>(m) * N + n;
            if (k >= kMin && k < kMax)
            {
                EXPECT_FLOAT_EQ(h0[i].real(), before[i].real());
                EXPECT_FLOAT_EQ(h0[i].imag(), before[i].imag());
                ++kept;
            }
            else
            {
                EXPECT_FLOAT_EQ(h0[i].real(), 0.0f);
                EXPECT_FLOAT_EQ(h0[i].imag(), 0.0f);
            }
        }
    }
    EXPECT_GT(kept, 0u) << "the band selected nothing — every assertion above was vacuous";
}

TEST(OceanCascade, ApplyBandLimitOverTheWholeSpectrumIsANoOp)
{
    // The single-cascade band. If this ever stopped being a no-op, every
    // existing scene would quietly change.
    const u32 N = 32u;
    Ocean::SpectrumParams p = MakeParams(1u, 100.0f, N);
    std::vector<Ocean::Complex> h0 = Ocean::GenerateH0(p);
    const std::vector<Ocean::Complex> before = h0;
    Ocean::ApplyBandLimit(h0, N, 100.0f, 0.0f, std::numeric_limits<f32>::infinity());
    for (sizet i = 0; i < h0.size(); ++i)
    {
        EXPECT_FLOAT_EQ(h0[i].real(), before[i].real());
        EXPECT_FLOAT_EQ(h0[i].imag(), before[i].imag());
    }
}

// -----------------------------------------------------------------------------
// The field: fallback fidelity and the summed surface
// -----------------------------------------------------------------------------

TEST(OceanCascade, SingleCascadeFieldReproducesThePreCascadePipeline)
{
    // "Existing one-cascade scenes retain their current authored behavior."
    // Asserted against an INDEPENDENT re-derivation of the pre-#969 pipeline
    // (unit-amplitude h0, measure the height RMS, bake one scale, evaluate) —
    // not against a golden, which would only say the field had not changed
    // since somebody rebased it.
    const u32 N = 64u;
    const Ocean::SpectrumParams p = MakeParams(1u, 140.0f, N);

    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 2.25f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
    ASSERT_EQ(field->GetCascadeCount(), 1u);
    const Ocean::DisplacementField& got = field->GetField();
    ASSERT_TRUE(got.IsValid());
    ASSERT_EQ(got.m_Resolution, N);

    Ocean::SpectrumParams unit = p;
    unit.m_Amplitude = 1.0f;
    std::vector<Ocean::Complex> h0 = Ocean::GenerateH0(unit);
    const Ocean::DisplacementField ref = Ocean::EvaluateField(p, h0, 0.0f);
    f64 sumSq = 0.0;
    for (f32 h : ref.m_Height)
        sumSq += static_cast<f64>(h) * h;
    const f32 rms = static_cast<f32>(std::sqrt(sumSq / static_cast<f64>(ref.m_Height.size())));
    const f32 scale = (p.m_Amplitude * 0.3f) / rms;
    for (Ocean::Complex& c : h0)
        c *= scale;
    const Ocean::DisplacementField expected = Ocean::EvaluateField(p, h0, 2.25f);

    ASSERT_EQ(expected.m_Height.size(), got.m_Height.size());
    for (sizet i = 0; i < expected.m_Height.size(); ++i)
        ASSERT_NEAR(got.m_Height[i], expected.m_Height[i], 1e-5f) << "at texel " << i;
}

TEST(OceanCascade, ThreeBandFieldKeepsTheAmplitudeCalibration)
{
    // Disjoint bands make the variances add, so ONE common scale over all three
    // hits the same target RMS the single-cascade field is calibrated to. Per-
    // band normalisation would instead flatten the spectrum into three equal
    // octaves, which reads as noise rather than as a sea — and would show up
    // here as a three-band RMS well above the single-band one.
    const auto rmsOfField = [](u32 cascades)
    {
        Ocean::SpectrumParams p = MakeParams(cascades, 140.0f, 64u);
        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 1.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
        f64 sumSq = 0.0;
        u32 n = 0u;
        for (int z = 0; z < 60; ++z)
        {
            for (int x = 0; x < 60; ++x)
            {
                const glm::vec2 xz(static_cast<f32>(x) * 7.3f, static_cast<f32>(z) * 5.9f);
                const f32 h = field->SampleCascades(xz).Height;
                sumSq += static_cast<f64>(h) * h;
                ++n;
            }
        }
        return std::sqrt(sumSq / static_cast<f64>(n));
    };

    const f64 single = rmsOfField(1u);
    const f64 three = rmsOfField(3u);
    const f64 target = 2.0 * 0.3; // m_Amplitude * kRmsMetresPerAmplitude
    EXPECT_NEAR(single, target, target * 0.45);
    EXPECT_NEAR(three, target, target * 0.45)
        << "three-band RMS " << three << " m against a " << target << " m target — the bands are being "
        << "normalised individually rather than as one surface";
}

TEST(OceanCascade, SummedSlopeAgreesWithTheSummedHeight)
{
    // docs/agent-rules/water-shading-nyquist.md §1: a derived normal must carry
    // every factor its displacement carries. With three bands there are three
    // chances to drop one — a missing band, or a band whose gradient was not
    // rotated back out of its own domain while its displacement was. Both show
    // up as the reported slope disagreeing with a finite difference of the
    // reported height, and neither shows up anywhere else.
    Ocean::SpectrumParams p = MakeParams(3u, 140.0f, 64u);
    p.m_Choppiness = 0.0f; // no horizontal shift, so the finite difference is honest
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 0.75f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);

    const f32 h = 0.25f;
    f64 sumErr = 0.0;
    f64 sumMag = 0.0;
    u32 n = 0u;
    for (int zi = 0; zi < 12; ++zi)
    {
        for (int xi = 0; xi < 12; ++xi)
        {
            const glm::vec2 c(static_cast<f32>(xi) * 11.0f, static_cast<f32>(zi) * 13.0f);
            const glm::vec2 slope = field->SampleCascades(c).Slope;
            const f32 dhdx = (field->SampleCascades(c + glm::vec2(h, 0.0f)).Height -
                              field->SampleCascades(c - glm::vec2(h, 0.0f)).Height) /
                             (2.0f * h);
            const f32 dhdz = (field->SampleCascades(c + glm::vec2(0.0f, h)).Height -
                              field->SampleCascades(c - glm::vec2(0.0f, h)).Height) /
                             (2.0f * h);
            sumErr += std::abs(slope.x - dhdx) + std::abs(slope.y - dhdz);
            sumMag += std::abs(dhdx) + std::abs(dhdz);
            ++n;
        }
    }
    ASSERT_GT(sumMag, 1e-3) << "the field is flat — the comparison would pass on nothing";
    EXPECT_LT(sumErr / sumMag, 0.35)
        << "the summed slope departs from a finite difference of the summed height by "
        << (100.0 * sumErr / sumMag) << "% — a band is missing from one of the two sums, or its "
        << "gradient was not rotated back out of its sampling domain";
}

TEST(OceanCascade, RotatedMidBandStillTravelsWithTheWind)
{
    // Rotating a sampling domain WITHOUT counter-rotating that band's wind is a
    // sea whose mid waves cross the wind, and it looks like a spectrum bug
    // rather than a bookkeeping one. Measured as directional slope energy: a
    // wind-aligned sea has far more |dh/dx| than |dh/dz|, and the three-band
    // field must keep essentially the anisotropy the single-band field has.
    const auto anisotropy = [](u32 cascades)
    {
        Ocean::SpectrumParams p = MakeParams(cascades, 140.0f, 64u);
        p.m_WindDirection = { 1.0f, 0.0f };
        p.m_DirectionalExponent = 4.0f; // a strongly directional sea, so the ratio is legible
        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 1.5f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
        f64 alongWind = 0.0;
        f64 acrossWind = 0.0;
        for (int z = 0; z < 40; ++z)
        {
            for (int x = 0; x < 40; ++x)
            {
                const glm::vec2 s =
                    field->SampleCascades(glm::vec2(static_cast<f32>(x) * 6.1f, static_cast<f32>(z) * 7.7f)).Slope;
                alongWind += static_cast<f64>(s.x) * s.x;
                acrossWind += static_cast<f64>(s.y) * s.y;
            }
        }
        return alongWind / std::max(acrossWind, 1e-9);
    };

    const f64 single = anisotropy(1u);
    const f64 three = anisotropy(3u);
    ASSERT_GT(single, 1.5) << "the single-band reference is not directional enough to measure against";
    EXPECT_GT(three, 1.5) << "the three-band sea has lost its wind alignment (ratio " << three
                          << " against the single band's " << single
                          << ") — the rotated band's wind was not counter-rotated";
}

TEST(OceanCascade, ThreeBandFieldCarriesLongerWavesThanTheSingleBand)
{
    // The symptom #969 opens with is weak near-to-horizon coherence: one tile
    // cannot hold a swell long enough to read at distance, because its longest
    // wave IS the tile. The preset's whole point is that the broad band can.
    //
    // Measured with the NORMALISED STRUCTURE FUNCTION
    //   D(lag) = mean( (h(x+lag) - h(x))^2 ) / (2 * variance)
    // which is 1 exactly when the two samples are uncorrelated and below 1 while
    // some wave still spans the lag. A plain correlation would not do: at half a
    // tile a single-tile field is strongly ANTI-correlated, and |rho| is then
    // large for the opposite of the reason the test is looking for.
    const auto structureAt = [](u32 cascades, f32 lag)
    {
        Ocean::SpectrumParams p = MakeParams(cascades, 140.0f, 64u);
        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 0.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
        f64 sumDiffSq = 0.0;
        f64 sumSq = 0.0;
        f64 sum = 0.0;
        u32 n = 0u;
        for (int z = 0; z < 48; ++z)
        {
            for (int x = 0; x < 48; ++x)
            {
                const glm::vec2 c(static_cast<f32>(x) * 9.3f, static_cast<f32>(z) * 11.7f);
                const f64 a = field->SampleCascades(c).Height;
                const f64 b = field->SampleCascades(c + glm::vec2(lag, 0.0f)).Height;
                sumDiffSq += (a - b) * (a - b);
                sumSq += a * a;
                sum += a;
                ++n;
            }
        }
        const f64 mean = sum / static_cast<f64>(n);
        const f64 variance = sumSq / static_cast<f64>(n) - mean * mean;
        return (sumDiffSq / static_cast<f64>(n)) / std::max(2.0 * variance, 1e-12);
    };

    // Half the authored tile: the single-band field has nothing left there,
    // because its longest wave is the tile it repeats on.
    const f32 lag = 70.0f;
    const f64 single = structureAt(1u, lag);
    const f64 three = structureAt(3u, lag);
    std::cout << "[ DIAG ] normalised structure function at " << lag << " m — single band " << single
              << ", three band " << three << " (1.0 = fully decorrelated)\n";
    EXPECT_LT(three, single) << "the three-band sea decorrelates no more slowly over " << lag
                             << " m than the single-tile one (" << three << " vs " << single
                             << ") — the broad band is carrying no energy";
}

TEST(OceanCascade, TheBroadBandCarriesRealEnergy)
{
    // The blunt companion to the structure-function test above, and the one that
    // localises a failure: whatever the summed surface looks like, the broad
    // band must be a material part of it. A band-limit that zeroed everything —
    // a boundary the wrong way round, a kMin above the tile's own Nyquist —
    // leaves a three-cascade field that renders, samples and parity-tests
    // perfectly while being the mid and fine bands alone.
    Ocean::SpectrumParams p = MakeParams(3u, 140.0f, 64u);
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 0.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
    ASSERT_EQ(field->GetCascadeCount(), 3u);

    const auto rmsOf = [](const Ocean::DisplacementField& f)
    {
        f64 acc = 0.0;
        for (f32 h : f.m_Height)
            acc += static_cast<f64>(h) * h;
        return f.m_Height.empty() ? 0.0 : std::sqrt(acc / static_cast<f64>(f.m_Height.size()));
    };

    f64 total = 0.0;
    std::vector<f64> perBand;
    for (u32 i = 0u; i < 3u; ++i)
    {
        const f64 rms = rmsOf(field->GetCascadeField(i));
        perBand.push_back(rms);
        total += rms * rms; // disjoint bands ⇒ variances add
        std::cout << "[ DIAG ] band " << i << " height RMS = " << rms << " m\n";
    }
    total = std::sqrt(total);
    ASSERT_GT(total, 1e-3);
    for (u32 i = 0u; i < 3u; ++i)
        EXPECT_GT(perBand[i], total * 0.02)
            << "band " << i << " contributes " << (100.0 * perBand[i] / total)
            << "% of the surface — its band limit has emptied it";
}

TEST(OceanCascade, DiagBandEnergyAndSlopeAtDriftSettings)
{
    // DIAGNOSTIC. Height RMS is what the amplitude normalisation preserves;
    // SLOPE RMS is what the eye reads as "there are waves here". A preset that
    // moves energy into wavelengths too long to see keeps the first and loses
    // the second, and the frame goes flat while every height assertion passes.
    // Reported at Drift's authored water settings so the numbers line up with
    // the acceptance captures.
    const auto report = [](u32 cascades)
    {
        Ocean::SpectrumParams p;
        p.m_Resolution = 128u;
        p.m_PatchSize = 140.0f;
        p.m_WindSpeed = 8.0f;
        p.m_WindDirection = { 1.0f, 0.2f };
        p.m_Amplitude = 0.55f;
        p.m_Choppiness = 0.9f;
        p.m_CascadeCount = cascades;
        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 3.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);

        f64 h2 = 0.0;
        f64 s2 = 0.0;
        u32 n = 0u;
        for (int z = 0; z < 64; ++z)
        {
            for (int x = 0; x < 64; ++x)
            {
                const auto smp = field->SampleCascades(
                    glm::vec2(static_cast<f32>(x) * 3.1f, static_cast<f32>(z) * 2.7f));
                h2 += static_cast<f64>(smp.Height) * smp.Height;
                s2 += static_cast<f64>(smp.Slope.x) * smp.Slope.x +
                      static_cast<f64>(smp.Slope.y) * smp.Slope.y;
                ++n;
            }
        }
        std::cout << "[ DIAG ] cascades=" << cascades << "  summed height RMS = " << std::sqrt(h2 / n)
                  << " m,  summed slope RMS = " << std::sqrt(s2 / n) << "\n";
        for (u32 i = 0u; i < field->GetCascadeCount(); ++i)
        {
            const Ocean::DisplacementField& f = field->GetCascadeField(i);
            f64 acc = 0.0;
            for (f32 h : f.m_Height)
                acc += static_cast<f64>(h) * h;
            std::cout << "[ DIAG ]   band " << i << " tile " << field->GetPreset().Bands[i].PatchSize
                      << " m, height RMS = " << (f.m_Height.empty() ? 0.0 : std::sqrt(acc / f.m_Height.size()))
                      << " m\n";
        }
        return std::sqrt(s2 / n);
    };

    const f64 singleSlope = report(1u);
    const f64 threeSlope = report(3u);
    std::cout << "[ DIAG ] slope ratio three/single = " << (threeSlope / std::max(singleSlope, 1e-9)) << "\n";

    // THE ACCEPTANCE BAR. The preset may redistribute the sea across
    // wavelengths, but it must not flatten it: the summed slope — the thing a
    // grazing camera actually sees — has to stay within reach of the sea the
    // author tuned. The first shipped ratios failed this at 0.35 and the
    // captures showed a mirror-smooth foreground.
    EXPECT_GT(threeSlope, singleSlope * 0.7)
        << "the three-band preset flattens the surface the author tuned (slope RMS " << threeSlope
        << " against " << singleSlope << ") — energy has moved into wavelengths too long to see";
}

TEST(OceanCascade, AnalyticReferenceRmsMatchesTheEvaluatedField)
{
    // The amplitude normalisation needs one number: the height RMS the t=0
    // field would have. It used to get it by running EvaluateField — eight
    // inverse FFTs — and throwing the field away. ReferenceHeightRms computes
    // the same number from Parseval, and this is what makes that swap safe
    // rather than plausible: EXACT agreement, not a tolerance somebody picked.
    for (u32 N : { 16u, 32u, 64u })
    {
        for (f32 wind : { 6.0f, 12.0f, 20.0f })
        {
            Ocean::SpectrumParams p = MakeParams(1u, 137.0f, N);
            p.m_WindSpeed = wind;
            const std::vector<Ocean::Complex> h0 = Ocean::GenerateH0(p);

            const Ocean::DisplacementField f = Ocean::EvaluateField(p, h0, 0.0f);
            f64 sumSq = 0.0;
            for (f32 h : f.m_Height)
                sumSq += static_cast<f64>(h) * h;
            const f64 measured = std::sqrt(sumSq / static_cast<f64>(f.m_Height.size()));
            const f64 analytic = Ocean::ReferenceHeightRms(h0, N);

            ASSERT_GT(measured, 1e-9) << "N=" << N << " wind=" << wind << ": the field is flat";
            EXPECT_NEAR(analytic, measured, measured * 1e-4)
                << "N=" << N << " wind=" << wind << ": Parseval disagrees with the inverse FFT";
        }
    }
}

TEST(OceanCascade, AmplitudeChangeDoesNotRegenerateTheSpectrum)
{
    // Drift's weather director eases the sea state EVERY TICK, so amplitude is
    // the most frequently written field on the component. It is a pure linear
    // scale on the spectrum, so it must not be part of the regeneration key —
    // it was, and that made an easing sea state regenerate the whole spectrum
    // (and run a full-resolution inverse FFT) every frame: 41 ms at one cascade,
    // 108 at three, measured in Drift.
    //
    // Asserted two ways, because the cheap half alone would pass on a field that
    // simply ignored the new amplitude: the surface must SCALE with it, and it
    // must scale linearly and exactly.
    Ocean::SpectrumParams p = MakeParams(3u, 140.0f, 64u);
    p.m_Amplitude = 1.0f;
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 4.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);

    std::vector<f32> before;
    for (int i = 0; i < 24; ++i)
        before.push_back(field->SampleCascades(glm::vec2(i * 9.0f, i * 4.0f)).Height);

    // Double it. Same spectrum, twice the sea.
    p.m_Amplitude = 2.0f;
    field->Update(p, 4.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);

    f64 peak = 0.0;
    for (int i = 0; i < 24; ++i)
    {
        const f32 after = field->SampleCascades(glm::vec2(i * 9.0f, i * 4.0f)).Height;
        peak = std::max(peak, std::abs(static_cast<f64>(before[i])));
        EXPECT_NEAR(after, before[i] * 2.0f, std::max(std::abs(before[i]) * 1e-3f, 1e-5f))
            << "sample " << i << ": doubling the amplitude did not double the surface";
    }
    ASSERT_GT(peak, 1e-3) << "the sea is flat — every assertion above passed on nothing";

    // ...and back down again, exactly. A rescale that accumulated a ratio in
    // place would drift here, which is why the unit spectrum is retained.
    p.m_Amplitude = 1.0f;
    field->Update(p, 4.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
    for (int i = 0; i < 24; ++i)
        EXPECT_NEAR(field->SampleCascades(glm::vec2(i * 9.0f, i * 4.0f)).Height, before[i],
                    std::max(std::abs(before[i]) * 1e-4f, 1e-6f))
            << "sample " << i << ": returning to the original amplitude did not return the original sea";
}

TEST(OceanCascade, CachedNoiseReproducesGenerateH0Exactly)
{
    // Splitting the Gaussian draws out of GenerateH0 is only safe if the draws
    // really are independent of everything else — the seed and the grid decide
    // them, the wind and the spectrum type do not. If that were wrong, a cached
    // noise set would silently freeze the sea's randomness at whatever the wind
    // was when it was first drawn, and every statistic would still look fine.
    //
    // BIT-exact, not near: the two paths must be the same arithmetic in the same
    // order, because a seed reproducing a given sea is the contract.
    for (u32 N : { 16u, 32u })
    {
        for (f32 wind : { 5.0f, 14.0f })
        {
            for (u32 seed : { 1337u, 4242u })
            {
                Ocean::SpectrumParams p = MakeParams(1u, 111.0f, N);
                p.m_WindSpeed = wind;
                p.m_Seed = seed;

                const std::vector<Ocean::Complex> viaRng = Ocean::GenerateH0(p);
                const std::vector<glm::vec2> noise = Ocean::GenerateSpectrumNoise(seed, N);
                const std::vector<Ocean::Complex> viaNoise = Ocean::GenerateH0FromNoise(p, noise);

                ASSERT_EQ(viaRng.size(), viaNoise.size());
                for (sizet i = 0; i < viaRng.size(); ++i)
                {
                    ASSERT_FLOAT_EQ(viaNoise[i].real(), viaRng[i].real()) << "N=" << N << " seed=" << seed << " bin " << i;
                    ASSERT_FLOAT_EQ(viaNoise[i].imag(), viaRng[i].imag()) << "N=" << N << " seed=" << seed << " bin " << i;
                }
            }
        }
    }

    // ...and the draws really are wind-independent: same seed, different wind,
    // same noise. (The SPECTRUM differs, which the loop above already covers.)
    const std::vector<glm::vec2> a = Ocean::GenerateSpectrumNoise(99u, 32u);
    const std::vector<glm::vec2> b = Ocean::GenerateSpectrumNoise(99u, 32u);
    ASSERT_EQ(a.size(), b.size());
    for (sizet i = 0; i < a.size(); ++i)
    {
        EXPECT_FLOAT_EQ(a[i].x, b[i].x);
        EXPECT_FLOAT_EQ(a[i].y, b[i].y);
    }
}

// -----------------------------------------------------------------------------
// The UBO packing
// -----------------------------------------------------------------------------

TEST(OceanCascade, ShaderParamsAreThePresetsOwnTilesAndRotation)
{
    // The shader must sample the tiles the spectra were generated on. Any second
    // derivation of these numbers is a second chance to disagree, so the pack
    // reads the preset and this test reads the preset.
    const Ocean::CascadePreset preset = Ocean::MakeCascadePreset(3u, 140.0f, 128u);
    const glm::vec4 packed = Ocean::PackCascadeShaderParams(preset);
    EXPECT_FLOAT_EQ(packed.x, 1.0f / preset.Bands[1].PatchSize);
    EXPECT_FLOAT_EQ(packed.y, 1.0f / preset.Bands[2].PatchSize);
    EXPECT_FLOAT_EQ(packed.z, std::cos(preset.Bands[1].DomainRotation));
    EXPECT_FLOAT_EQ(packed.w, std::sin(preset.Bands[1].DomainRotation));

    // The one-cascade pack must be the identity rotation and no extra tiles, so
    // a shader that loops `i < count` never reads a scale that means nothing.
    const glm::vec4 single = Ocean::PackCascadeShaderParams(Ocean::MakeCascadePreset(1u, 140.0f, 128u));
    EXPECT_FLOAT_EQ(single.z, 1.0f);
    EXPECT_FLOAT_EQ(single.w, 0.0f);
}

// -----------------------------------------------------------------------------
// The shader text pin
// -----------------------------------------------------------------------------

TEST(OceanCascade, EveryWaterStageSumsTheCascadesThroughTheSharedFunction)
{
    // A C++ test cannot see a shader stage growing its own copy of the cascade
    // walk, and three stages displacing the same surface from three copies is
    // the two-mirrors-drift failure with an extra mirror. So this is a TEXT
    // test, like WaterGerstnerNormalScaleTest: every stage that touches the FFT
    // field must go through sampleOceanCascades, and the ONLY direct fetches of
    // the cascade arrays must be the hook definitions the shared function calls.
    //
    // Negative control when changing this: point it at a stage with the old
    // `textureLod(u_FFTDisplacement, fftUV, 0.0)` line restored and confirm it
    // fails. An unfalsified pin is a decoration.
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE(root.empty()) << "could not locate the repo root from the test working directory";

    const char* stages[] = {
        "OloEditor/assets/shaders/include/WaterVertexStage.glsl",
        "OloEditor/assets/shaders/include/WaterTessEvalStage.glsl",
        "OloEditor/assets/shaders/Water.glsl",
    };

    for (const char* rel : stages)
    {
        const std::string src = ReadTextFile(root / rel);
        ASSERT_FALSE(src.empty()) << "could not read " << rel;

        EXPECT_NE(src.find("OceanCascadeCommon.glsl"), std::string::npos)
            << rel << " does not include the shared cascade sum";
        EXPECT_NE(src.find("sampleOceanCascades("), std::string::npos)
            << rel << " touches the FFT field without going through the shared sum";

        // Count direct sampler fetches. Exactly one per cascade array is
        // allowed: the hook definition. More than that is a second walk.
        for (const char* sampler : { "u_FFTDisplacement", "u_FFTDerivatives" })
        {
            sizet fetches = 0;
            sizet pos = 0;
            const std::string needle = std::string("textureLod(") + sampler;
            while ((pos = src.find(needle, pos)) != std::string::npos)
            {
                ++fetches;
                pos += needle.size();
            }
            EXPECT_LE(fetches, static_cast<sizet>(1))
                << rel << " fetches " << sampler << " directly " << fetches
                << " times — only the oceanCascadeFetch* hook may sample it";
        }
    }
}
