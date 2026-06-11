#include "OloEnginePCH.h"

// =============================================================================
// OceanFFTSpectrumTest — L1 property tests for the CPU Tessendorf FFT ocean
// math (Renderer/Ocean/OceanFFT.{h,cpp} + OceanSpectrum.{h,cpp}).
//
// These pin the *reference* spectrum + inverse-FFT pipeline that the GPU
// butterfly compute port (docs/WATER_FUTURE_IMPROVEMENTS.md §1.2) is validated
// against, and that the physics/buoyancy sampler can read without a GPU
// readback. The discipline mirrors WaterSurfaceSamplerTest's CPU/GPU mirror:
// nail the math on the CPU so the GPU copy can't silently drift.
//
// Coverage:
//   * FFT correctness — round-trip identity, impulse/constant pairs, Parseval
//     energy conservation, 2D separability.
//   * Spectrum shape — Phillips zero at k=0 and perpendicular to wind, wind
//     alignment, high-frequency falloff, small-wave suppression.
//   * Field reality — the h̃(k,t) construction is Hermitian ⇒ the inverse FFT
//     is real (the property the whole pipeline depends on).
//   * Field behaviour — determinism, time evolution, amplitude monotonicity,
//     unit upward normals, Jacobian == 1 when not choppy, bilinear sampling.
// =============================================================================

#include "OloEngine/Renderer/Ocean/OceanFFT.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Ocean/OceanSpectrum.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity
using OloEngine::Ocean::Complex;

namespace
{
    f32 Rms(const std::vector<f32>& v)
    {
        if (v.empty())
            return 0.0f;
        f64 acc = 0.0;
        for (f32 x : v)
            acc += static_cast<f64>(x) * x;
        return static_cast<f32>(std::sqrt(acc / static_cast<f64>(v.size())));
    }
} // namespace

// ---------------------------------------------------------------------------
// FFT building blocks
// ---------------------------------------------------------------------------

TEST(OceanFFT, IsPowerOfTwo)
{
    for (u32 v : { 1u, 2u, 4u, 8u, 16u, 256u, 1024u })
        EXPECT_TRUE(Ocean::IsPowerOfTwo(v)) << v;
    for (u32 v : { 0u, 3u, 5u, 6u, 7u, 100u, 1023u })
        EXPECT_FALSE(Ocean::IsPowerOfTwo(v)) << v;
}

TEST(OceanFFT, SignedFrequencyOrdering)
{
    constexpr u32 N = 8u;
    // FFT order: 0,1,2,3 are non-negative; 4 is Nyquist (-N/2); 5,6,7 negative.
    EXPECT_EQ(Ocean::SignedFrequency(0u, N), 0);
    EXPECT_EQ(Ocean::SignedFrequency(1u, N), 1);
    EXPECT_EQ(Ocean::SignedFrequency(3u, N), 3);
    EXPECT_EQ(Ocean::SignedFrequency(4u, N), 4); // N/2 stays positive
    EXPECT_EQ(Ocean::SignedFrequency(5u, N), -3);
    EXPECT_EQ(Ocean::SignedFrequency(7u, N), -1);
}

TEST(OceanFFT, RoundTrip1D)
{
    constexpr u32 N = 64u;
    std::vector<Complex> original(N);
    std::mt19937 rng(42u);
    std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
    for (auto& c : original)
        c = Complex(dist(rng), dist(rng));

    std::vector<Complex> data = original;
    Ocean::FFT1D(data, /*inverse=*/false);
    Ocean::FFT1D(data, /*inverse=*/true);

    for (u32 i = 0; i < N; ++i)
    {
        EXPECT_NEAR(data[i].real(), original[i].real(), 1e-4f) << "idx " << i;
        EXPECT_NEAR(data[i].imag(), original[i].imag(), 1e-4f) << "idx " << i;
    }
}

TEST(OceanFFT, ImpulseTransformsToConstant)
{
    // FFT of a unit impulse at n=0 is all-ones (every bin equal).
    constexpr u32 N = 16u;
    std::vector<Complex> data(N, Complex(0.0f, 0.0f));
    data[0] = Complex(1.0f, 0.0f);
    Ocean::FFT1D(data, false);
    for (u32 i = 0; i < N; ++i)
    {
        EXPECT_NEAR(data[i].real(), 1.0f, 1e-5f) << "idx " << i;
        EXPECT_NEAR(data[i].imag(), 0.0f, 1e-5f) << "idx " << i;
    }
}

TEST(OceanFFT, ConstantTransformsToDCImpulse)
{
    // FFT of a constant signal concentrates all energy in the DC bin.
    constexpr u32 N = 16u;
    std::vector<Complex> data(N, Complex(2.0f, 0.0f));
    Ocean::FFT1D(data, false);
    EXPECT_NEAR(data[0].real(), 2.0f * N, 1e-4f);
    EXPECT_NEAR(data[0].imag(), 0.0f, 1e-4f);
    for (u32 i = 1; i < N; ++i)
    {
        EXPECT_NEAR(data[i].real(), 0.0f, 1e-3f) << "idx " << i;
        EXPECT_NEAR(data[i].imag(), 0.0f, 1e-3f) << "idx " << i;
    }
}

TEST(OceanFFT, ParsevalEnergyConservation)
{
    // Unnormalised forward DFT: sum|X|^2 == N * sum|x|^2.
    constexpr u32 N = 32u;
    std::vector<Complex> x(N);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
    for (auto& c : x)
        c = Complex(dist(rng), dist(rng));

    f64 spatialEnergy = 0.0;
    for (const auto& c : x)
        spatialEnergy += static_cast<f64>(std::norm(c));

    std::vector<Complex> X = x;
    Ocean::FFT1D(X, false);
    f64 freqEnergy = 0.0;
    for (const auto& c : X)
        freqEnergy += static_cast<f64>(std::norm(c));

    EXPECT_NEAR(freqEnergy, static_cast<f64>(N) * spatialEnergy, spatialEnergy * 1e-3);
}

TEST(OceanFFT, RoundTrip2D)
{
    constexpr u32 W = 16u, H = 8u;
    std::vector<Complex> original(static_cast<sizet>(W) * H);
    std::mt19937 rng(99u);
    std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
    for (auto& c : original)
        c = Complex(dist(rng), dist(rng));

    std::vector<Complex> data = original;
    Ocean::FFT2D(data, W, H, false);
    Ocean::FFT2D(data, W, H, true);

    for (sizet i = 0; i < original.size(); ++i)
    {
        EXPECT_NEAR(data[i].real(), original[i].real(), 1e-4f) << "idx " << i;
        EXPECT_NEAR(data[i].imag(), original[i].imag(), 1e-4f) << "idx " << i;
    }
}

// ---------------------------------------------------------------------------
// Phillips spectrum shape
// ---------------------------------------------------------------------------

TEST(OceanSpectrum, PhillipsZeroAtOrigin)
{
    Ocean::SpectrumParams p{};
    EXPECT_FLOAT_EQ(Ocean::PhillipsSpectrum(p, glm::vec2(0.0f)), 0.0f);
}

TEST(OceanSpectrum, PhillipsZeroPerpendicularToWind)
{
    // The |k̂·ŵ|^p directional term kills energy travelling across the wind.
    Ocean::SpectrumParams p{};
    p.m_WindDirection = glm::vec2(1.0f, 0.0f);
    const f32 perp = Ocean::PhillipsSpectrum(p, glm::vec2(0.0f, 0.5f)); // k ⟂ wind
    EXPECT_NEAR(perp, 0.0f, 1e-6f);
    const f32 aligned = Ocean::PhillipsSpectrum(p, glm::vec2(0.5f, 0.0f)); // k ∥ wind
    EXPECT_GT(aligned, 0.0f);
}

TEST(OceanSpectrum, PhillipsFavoursWindAlignment)
{
    Ocean::SpectrumParams p{};
    p.m_WindDirection = glm::vec2(1.0f, 0.0f);
    const f32 aligned = Ocean::PhillipsSpectrum(p, glm::vec2(0.4f, 0.0f));
    const f32 diagonal = Ocean::PhillipsSpectrum(p, glm::vec2(0.4f, 0.4f));
    EXPECT_GT(aligned, diagonal);
}

TEST(OceanSpectrum, PhillipsHighFrequencyFalloff)
{
    // Energy ~ exp(-1/(kL)^2)/k^4 — the 1/k^4 makes short waves much weaker.
    Ocean::SpectrumParams p{};
    p.m_WindDirection = glm::vec2(1.0f, 0.0f);
    p.m_SmallWaveSuppression = 0.0f; // isolate the 1/k^4 falloff
    const f32 lowK = Ocean::PhillipsSpectrum(p, glm::vec2(0.3f, 0.0f));
    const f32 highK = Ocean::PhillipsSpectrum(p, glm::vec2(3.0f, 0.0f));
    EXPECT_GT(lowK, highK * 10.0f);
}

TEST(OceanSpectrum, SmallWaveSuppressionDampsHighFrequencies)
{
    Ocean::SpectrumParams on{};
    on.m_WindDirection = glm::vec2(1.0f, 0.0f);
    on.m_SmallWaveSuppression = 2.0f;
    Ocean::SpectrumParams off = on;
    off.m_SmallWaveSuppression = 0.0f;

    const glm::vec2 highK(5.0f, 0.0f);
    EXPECT_LT(Ocean::PhillipsSpectrum(on, highK), Ocean::PhillipsSpectrum(off, highK));
}

TEST(OceanSpectrum, DispersionMonotonicAndDeepWater)
{
    EXPECT_FLOAT_EQ(Ocean::Dispersion(0.0f, 9.81f), 0.0f);
    EXPECT_NEAR(Ocean::Dispersion(1.0f, 9.81f), std::sqrt(9.81f), 1e-5f);
    EXPECT_GT(Ocean::Dispersion(4.0f, 9.81f), Ocean::Dispersion(1.0f, 9.81f));
}

// ---------------------------------------------------------------------------
// h0 generation
// ---------------------------------------------------------------------------

TEST(OceanSpectrum, GenerateH0IsDeterministic)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto a = Ocean::GenerateH0(p);
    const auto b = Ocean::GenerateH0(p);
    ASSERT_EQ(a.size(), b.size());
    for (sizet i = 0; i < a.size(); ++i)
        EXPECT_EQ(a[i], b[i]) << "idx " << i;
}

TEST(OceanSpectrum, GenerateH0SeedChangesField)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto a = Ocean::GenerateH0(p);
    p.m_Seed = 9001u;
    const auto b = Ocean::GenerateH0(p);
    sizet diff = 0;
    for (sizet i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            ++diff;
    EXPECT_GT(diff, a.size() / 2);
}

TEST(OceanSpectrum, GenerateH0ZeroAtDC)
{
    // Phillips(0) == 0 ⇒ the DC bin (n=m=0) carries no energy.
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto h0 = Ocean::GenerateH0(p);
    EXPECT_FLOAT_EQ(std::abs(h0[0]), 0.0f);
}

// ---------------------------------------------------------------------------
// Field reality (the property the inverse FFTs rely on)
// ---------------------------------------------------------------------------

TEST(OceanSpectrum, AnimatedSpectrumIsHermitianSoFieldIsReal)
{
    // Reconstruct h̃(k,t) exactly as EvaluateField does and confirm the inverse
    // FFT is real to floating-point precision — i.e. the height field carries no
    // spurious imaginary component that the pipeline would be silently dropping.
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const u32 N = p.m_Resolution;
    const auto h0 = Ocean::GenerateH0(p);

    const f32 time = 3.0f;
    std::vector<Complex> hTilde(static_cast<sizet>(N) * N);
    constexpr f32 kTwoPi = 6.28318530718f;
    for (u32 m = 0; m < N; ++m)
        for (u32 n = 0; n < N; ++n)
        {
            const f32 kx = kTwoPi * static_cast<f32>(Ocean::SignedFrequency(n, N)) / p.m_PatchSize;
            const f32 kz = kTwoPi * static_cast<f32>(Ocean::SignedFrequency(m, N)) / p.m_PatchSize;
            const f32 kMag = std::sqrt(kx * kx + kz * kz);
            const f32 w = Ocean::Dispersion(kMag, p.m_Gravity) * time;
            const Complex pp(std::cos(w), std::sin(w));
            const Complex pm(std::cos(w), -std::sin(w));
            const u32 mn = (N - n) % N;
            const u32 mm = (N - m) % N;
            const Complex h = h0[static_cast<sizet>(m) * N + n] * pp +
                              std::conj(h0[static_cast<sizet>(mm) * N + mn]) * pm;
            hTilde[static_cast<sizet>(m) * N + n] = h;
        }

    Ocean::FFT2D(hTilde, N, N, /*inverse=*/true);
    f32 maxImag = 0.0f;
    for (const auto& c : hTilde)
        maxImag = std::max(maxImag, std::abs(c.imag()));
    EXPECT_LT(maxImag, 1e-3f) << "inverse FFT left a non-trivial imaginary part";
}

// ---------------------------------------------------------------------------
// EvaluateField behaviour
// ---------------------------------------------------------------------------

TEST(OceanSpectrum, EvaluateFieldIsDeterministic)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto h0 = Ocean::GenerateH0(p);
    const auto a = Ocean::EvaluateField(p, h0, 1.5f);
    const auto b = Ocean::EvaluateField(p, h0, 1.5f);
    ASSERT_TRUE(a.IsValid());
    for (sizet i = 0; i < a.m_Height.size(); ++i)
        EXPECT_FLOAT_EQ(a.m_Height[i], b.m_Height[i]) << "idx " << i;
}

TEST(OceanSpectrum, EvaluateFieldAnimatesOverTime)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto h0 = Ocean::GenerateH0(p);
    const auto a = Ocean::EvaluateField(p, h0, 0.0f);
    const auto b = Ocean::EvaluateField(p, h0, 2.0f);

    f32 maxDelta = 0.0f;
    for (sizet i = 0; i < a.m_Height.size(); ++i)
        maxDelta = std::max(maxDelta, std::abs(a.m_Height[i] - b.m_Height[i]));
    EXPECT_GT(maxDelta, 1e-3f) << "the surface did not evolve with time";
}

TEST(OceanSpectrum, ZeroAmplitudeProducesFlatSea)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    p.m_Amplitude = 0.0f;
    const auto h0 = Ocean::GenerateH0(p);
    const auto field = Ocean::EvaluateField(p, h0, 1.0f);
    ASSERT_TRUE(field.IsValid());

    for (sizet i = 0; i < field.m_Height.size(); ++i)
    {
        EXPECT_NEAR(field.m_Height[i], 0.0f, 1e-5f);
        EXPECT_NEAR(field.m_HorizontalDisplacement[i].x, 0.0f, 1e-5f);
        EXPECT_NEAR(field.m_HorizontalDisplacement[i].y, 0.0f, 1e-5f);
        // Flat sea ⇒ normals point straight up, no folding.
        EXPECT_NEAR(field.m_Normal[i].y, 1.0f, 1e-4f);
        EXPECT_NEAR(field.m_Jacobian[i], 1.0f, 1e-4f);
    }
}

TEST(OceanSpectrum, HigherAmplitudeRaisesRoughness)
{
    Ocean::SpectrumParams calm{};
    calm.m_Resolution = 64u;
    calm.m_Amplitude = 1.0f;
    Ocean::SpectrumParams rough = calm;
    rough.m_Amplitude = 4.0f;

    const auto fCalm = Ocean::EvaluateField(calm, Ocean::GenerateH0(calm), 1.0f);
    const auto fRough = Ocean::EvaluateField(rough, Ocean::GenerateH0(rough), 1.0f);
    EXPECT_GT(Rms(fRough.m_Height), Rms(fCalm.m_Height));
}

TEST(OceanSpectrum, NormalsAreUnitLengthAndMostlyUpward)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    const auto field = Ocean::EvaluateField(p, Ocean::GenerateH0(p), 1.0f);
    ASSERT_TRUE(field.IsValid());

    f64 meanUp = 0.0;
    for (const auto& n : field.m_Normal)
    {
        EXPECT_NEAR(glm::length(n), 1.0f, 1e-4f);
        meanUp += static_cast<f64>(n.y);
    }
    meanUp /= static_cast<f64>(field.m_Normal.size());
    EXPECT_GT(meanUp, 0.5) << "ocean normals should on average face upward";
}

TEST(OceanSpectrum, NoChoppinessGivesUnitJacobianAndNoHorizontalShift)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    p.m_Choppiness = 0.0f; // λ=0 ⇒ no horizontal displacement ⇒ J ≡ 1
    const auto field = Ocean::EvaluateField(p, Ocean::GenerateH0(p), 1.0f);
    ASSERT_TRUE(field.IsValid());

    for (sizet i = 0; i < field.m_Jacobian.size(); ++i)
    {
        EXPECT_NEAR(field.m_HorizontalDisplacement[i].x, 0.0f, 1e-5f);
        EXPECT_NEAR(field.m_HorizontalDisplacement[i].y, 0.0f, 1e-5f);
        EXPECT_NEAR(field.m_Jacobian[i], 1.0f, 1e-4f) << "idx " << i;
    }
}

TEST(OceanSpectrum, ChoppinessIntroducesFolding)
{
    // A choppy sea must fold somewhere (Jacobian < 1) — that is the foam source.
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    p.m_Amplitude = 6.0f;
    p.m_Choppiness = 2.0f;
    const auto field = Ocean::EvaluateField(p, Ocean::GenerateH0(p), 1.0f);
    ASSERT_TRUE(field.IsValid());

    f32 minJ = std::numeric_limits<f32>::max();
    for (f32 j : field.m_Jacobian)
        minJ = std::min(minJ, j);
    EXPECT_LT(minJ, 1.0f) << "a choppy ocean should fold somewhere";
}

TEST(OceanSpectrum, SampleHeightBilinearMatchesGridPoints)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto field = Ocean::EvaluateField(p, Ocean::GenerateH0(p), 1.0f);
    const u32 N = field.m_Resolution;
    const f32 L = p.m_PatchSize;

    // World XZ that lands exactly on grid node (x=5, z=7).
    const f32 wx = 5.0f * L / static_cast<f32>(N);
    const f32 wz = 7.0f * L / static_cast<f32>(N);
    const f32 expected = field.m_Height[static_cast<sizet>(7) * N + 5];
    EXPECT_NEAR(Ocean::SampleHeightBilinear(field, L, glm::vec2(wx, wz)), expected, 1e-4f);
}

TEST(OceanSpectrum, SampleHeightBilinearIsPeriodic)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto field = Ocean::EvaluateField(p, Ocean::GenerateH0(p), 1.0f);
    const f32 L = p.m_PatchSize;

    const glm::vec2 q(13.3f, -4.7f);
    const f32 base = Ocean::SampleHeightBilinear(field, L, q);
    // Shifting the query by one full patch in either axis must wrap to the same.
    EXPECT_NEAR(Ocean::SampleHeightBilinear(field, L, q + glm::vec2(L, 0.0f)), base, 1e-4f);
    EXPECT_NEAR(Ocean::SampleHeightBilinear(field, L, q + glm::vec2(0.0f, -L)), base, 1e-4f);
}

// ---------------------------------------------------------------------------
// OceanFFTField — the runtime CPU field provider (no GPU upload here, so these
// run headless in CI; the texture upload path is exercised by the GPU visual
// evidence test instead).
// ---------------------------------------------------------------------------

TEST(OceanFFTField, UpdateProducesValidFieldWithoutGpu)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 1.0f, /*uploadToGpu=*/false);
    EXPECT_TRUE(field->GetField().IsValid());
    EXPECT_EQ(field->GetField().m_Resolution, 64u);
    // No GPU upload requested → no texture IDs.
    EXPECT_EQ(field->GetDisplacementTextureID(), 0u);
    EXPECT_EQ(field->GetDerivativesTextureID(), 0u);
}

TEST(OceanFFTField, SampleHeightIsFiniteAndDeterministic)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    p.m_PatchSize = 100.0f;
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 2.0f, false);

    for (glm::vec2 xz : { glm::vec2(0.0f), glm::vec2(33.0f, -12.0f), glm::vec2(250.0f, 250.0f) })
    {
        const f32 h = field->SampleHeight(xz);
        EXPECT_TRUE(std::isfinite(h));
        EXPECT_FLOAT_EQ(h, field->SampleHeight(xz)); // deterministic
    }
}

TEST(OceanFFTField, NormalisesToMetreScaleWaveHeight)
{
    // OceanFFTField normalises the raw (unitless, ~cm-flat) Phillips amplitude so
    // m_Amplitude maps to a predictable RMS wave height in metres — otherwise the
    // rendered ocean looks mirror-flat. Pin that the normalised height lands in a
    // sensible metre band and scales with m_Amplitude.
    Ocean::SpectrumParams p{};
    p.m_Resolution = 128u;
    p.m_PatchSize = 64.0f;
    p.m_WindSpeed = 18.0f;
    p.m_WindDirection = glm::vec2(1.0f, 0.3f);
    p.m_Choppiness = 1.4f;

    auto rmsOf = [](const Ocean::DisplacementField& f)
    {
        f64 sumSq = 0.0;
        for (f32 h : f.m_Height)
            sumSq += static_cast<f64>(h) * h;
        return f.m_Height.empty() ? 0.0 : std::sqrt(sumSq / static_cast<f64>(f.m_Height.size()));
    };

    auto field = Ref<Ocean::OceanFFTField>::Create();
    p.m_Amplitude = 3.0f;
    field->Update(p, 1.0f, /*uploadToGpu=*/false);
    const f64 rms3 = rmsOf(field->GetField());
    std::cout << "[ DIAG ] normalised RMS at amplitude 3 = " << rms3 << " m\n";
    EXPECT_GT(rms3, 0.3); // not mirror-flat
    EXPECT_LT(rms3, 3.0); // not absurdly tall

    // Doubling the amplitude doubles the RMS (the normalisation is linear).
    auto field2 = Ref<Ocean::OceanFFTField>::Create();
    p.m_Amplitude = 6.0f;
    field2->Update(p, 1.0f, false);
    const f64 rms6 = rmsOf(field2->GetField());
    EXPECT_NEAR(rms6, rms3 * 2.0, rms3 * 0.15);
}

TEST(OceanFFTField, FlatSeaSamplesNearZero)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 64u;
    p.m_Amplitude = 0.0f; // no energy ⇒ flat
    auto field = Ref<Ocean::OceanFFTField>::Create();
    field->Update(p, 1.0f, false);
    EXPECT_NEAR(field->SampleHeight(glm::vec2(10.0f, 20.0f)), 0.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// ExtractBandLimitedH0 — the low-res CPU physics proxy used while the GPU
// compute butterfly owns the rendered field (WATER_FUTURE_IMPROVEMENTS.md
// §1.2). The proxy must be the SAME ocean (same wave vectors, same phases),
// just band-limited — not a statistically-different re-roll.
// ---------------------------------------------------------------------------

TEST(OceanSpectrum, BandLimitedH0CopiesMatchingFrequencyBins)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 128u;
    const auto h0 = Ocean::GenerateH0(p);
    const auto small = Ocean::ExtractBandLimitedH0(h0, 128u, 32u);
    ASSERT_EQ(small.size(), static_cast<sizet>(32u) * 32u);

    constexpr u32 N = 128u, Ns = 32u;
    // Bins are scaled by (Ns/N)² so the small grid's 1/Ns² inverse-FFT factor
    // lands on the same spatial metres as the full grid's 1/N².
    constexpr f32 kScale = (static_cast<f32>(Ns) / static_cast<f32>(N)) * (static_cast<f32>(Ns) / static_cast<f32>(N));
    for (u32 m = 0u; m < Ns; ++m)
    {
        const i32 fm = Ocean::SignedFrequency(m, Ns);
        for (u32 n = 0u; n < Ns; ++n)
        {
            const i32 fn = Ocean::SignedFrequency(n, Ns);
            const Complex got = small[static_cast<sizet>(m) * Ns + n];
            if (fn == static_cast<i32>(Ns / 2u) || fm == static_cast<i32>(Ns / 2u))
            {
                // Small-grid Nyquist bins are zeroed (the full grid keeps ±Ns/2
                // distinct; the small grid can't).
                EXPECT_EQ(got, Complex(0.0f, 0.0f)) << "bin (" << n << "," << m << ")";
                continue;
            }
            const u32 srcN = static_cast<u32>((fn + static_cast<i32>(N)) % static_cast<i32>(N));
            const u32 srcM = static_cast<u32>((fm + static_cast<i32>(N)) % static_cast<i32>(N));
            const Complex expected = kScale * h0[static_cast<sizet>(srcM) * N + srcN];
            EXPECT_NEAR(got.real(), expected.real(), 1e-7f) << "bin (" << n << "," << m << ")";
            EXPECT_NEAR(got.imag(), expected.imag(), 1e-7f) << "bin (" << n << "," << m << ")";
        }
    }
}

TEST(OceanSpectrum, BandLimitedH0SameOrLargerResolutionIsIdentity)
{
    Ocean::SpectrumParams p{};
    p.m_Resolution = 32u;
    const auto h0 = Ocean::GenerateH0(p);
    const auto same = Ocean::ExtractBandLimitedH0(h0, 32u, 32u);
    ASSERT_EQ(same.size(), h0.size());
    for (sizet i = 0; i < h0.size(); ++i)
        EXPECT_EQ(same[i], h0[i]) << "idx " << i;
}

TEST(OceanSpectrum, BandLimitedFieldTracksFullResolutionSurface)
{
    // The proxy evaluated from the extracted band must follow the full-res
    // surface point-for-point in world space (it IS the same surface low-pass
    // filtered), not merely share its statistics. Phillips energy ~ 1/k⁴, so
    // the dropped high-frequency band carries little height — the per-point
    // error must be a small fraction of the wave RMS.
    Ocean::SpectrumParams p{};
    p.m_Resolution = 128u;
    p.m_PatchSize = 64.0f;
    p.m_WindSpeed = 18.0f;
    p.m_WindDirection = glm::vec2(1.0f, 0.3f);
    p.m_Choppiness = 0.0f; // compare raw heights (no horizontal shift)
    const auto h0 = Ocean::GenerateH0(p);
    const auto full = Ocean::EvaluateField(p, h0, 5.0f);

    Ocean::SpectrumParams ps = p;
    ps.m_Resolution = 64u;
    const auto h0Small = Ocean::ExtractBandLimitedH0(h0, 128u, 64u);
    const auto proxy = Ocean::EvaluateField(ps, h0Small, 5.0f);
    ASSERT_TRUE(full.IsValid());
    ASSERT_TRUE(proxy.IsValid());

    const f32 fullRms = Rms(full.m_Height);
    ASSERT_GT(fullRms, 0.0f);

    f32 maxErr = 0.0f;
    for (u32 z = 0u; z < 64u; ++z)
        for (u32 x = 0u; x < 64u; ++x)
        {
            const glm::vec2 world(static_cast<f32>(x) * p.m_PatchSize / 64.0f,
                                  static_cast<f32>(z) * p.m_PatchSize / 64.0f);
            const f32 hFull = Ocean::SampleHeightBilinear(full, p.m_PatchSize, world);
            const f32 hProxy = Ocean::SampleHeightBilinear(proxy, p.m_PatchSize, world);
            maxErr = std::max(maxErr, std::abs(hFull - hProxy));
        }
    std::cout << "[ DIAG ] band-limit proxy max error = " << maxErr << " (full RMS " << fullRms << ")\n";
    // With the default 1 m small-wave suppression the dropped |k| band carries
    // almost no energy — the proxy should match to a few % of the wave RMS.
    EXPECT_LT(maxErr, fullRms * 0.1f) << "proxy diverges from the full surface — wrong bins extracted?";
}
