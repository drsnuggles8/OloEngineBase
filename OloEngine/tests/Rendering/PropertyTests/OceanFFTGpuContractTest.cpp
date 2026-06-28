// =============================================================================
// OceanFFTGpuContractTest.cpp
//
// GPU-vs-CPU contract tests for the compute butterfly FFT ocean port
// (docs/design/WATER_FUTURE_IMPROVEMENTS.md §1.2). The CPU pipeline in
// OceanSpectrum/OceanFFT is the validated reference (OceanFFTSpectrumTest);
// these tests read the GPU results back and compare within float tolerance,
// from the riskiest math outward:
//
//   1. The raw butterfly chain — an analytic DC impulse (exact expectation)
//      and a random complex grid against the CPU inverse FFT2D. Any twiddle
//      sign, bit-reversal, ping-pong or normalisation bug dies here, where
//      it is diagnosable, instead of as "the ocean looks wrong".
//   2. The full evolve→IFFT→assemble chain against EvaluateField: height,
//      choppy displacement, foam, normals, Jacobian per-texel.
//   3. OceanFFTField end-to-end: the GPU-mode textures match the CPU-mode
//      textures (same h0, same RMS normalisation), and the band-limited
//      physics proxy keeps SampleHeight tracking the same surface.
//
// Requires a GL 4.6 context; SKIPs cleanly otherwise (RendererAttachedTest).
// Classification: integration (numeric GPU-vs-CPU contract via compute
// dispatch + texture readback — not a composed-frame golden/evidence test).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Ocean/OceanFFT.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Ocean/OceanFFTGpu.h"
#include "OloEngine/Renderer/Ocean/OceanSpectrum.h"
#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity
using OloEngine::Ocean::Complex;

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] std::vector<glm::vec4> ReadbackRgba32f(u32 textureID, u32 resolution)
        {
            std::vector<glm::vec4> out(static_cast<sizet>(resolution) * resolution);
            // Defaults, in case an earlier test left pack state dirty.
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glGetTextureImage(textureID, 0, GL_RGBA, GL_FLOAT,
                              static_cast<GLsizei>(out.size() * sizeof(glm::vec4)), out.data());
            return out;
        }

        [[nodiscard]] f32 RmsOf(const std::vector<f32>& v)
        {
            if (v.empty())
                return 0.0f;
            f64 acc = 0.0;
            for (f32 x : v)
                acc += static_cast<f64>(x) * x;
            return static_cast<f32>(std::sqrt(acc / static_cast<f64>(v.size())));
        }

        [[nodiscard]] Ref<Texture2D> MakeFieldTexture(u32 resolution)
        {
            TextureSpecification spec;
            spec.Width = resolution;
            spec.Height = resolution;
            spec.Format = ImageFormat::RGBA32F;
            spec.GenerateMips = false;
            spec.MipLevels = 1;
            return Texture2D::Create(spec);
        }
    } // namespace

    // Reuses RendererAttachedTest only for the one-time Renderer/GL bring-up
    // (and the OloEditor/ working directory the shader paths assume); no scene.
    class OceanFFTGpuContractTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    // -----------------------------------------------------------------------
    // 1. Raw butterfly chain
    // -----------------------------------------------------------------------

    TEST_F(OceanFFTGpuContractTest, InverseFFTOfDCImpulseIsConstant)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 N = 64u;
        std::vector<Complex> freq(static_cast<sizet>(N) * N, Complex(0.0f, 0.0f));
        freq[0] = Complex(1.0f, 0.0f); // DC only ⇒ inverse is 1/N² everywhere

        auto gpu = Ref<Ocean::OceanFFTGpu>::Create();
        ASSERT_TRUE(gpu->IsAvailable()) << "Ocean FFT compute shaders failed to compile";
        const auto spatial = gpu->DebugInverseFFT2D(freq, N);
        ASSERT_EQ(spatial.size(), freq.size());

        const f32 expected = 1.0f / (static_cast<f32>(N) * static_cast<f32>(N));
        for (sizet i = 0; i < spatial.size(); ++i)
        {
            EXPECT_NEAR(spatial[i].real(), expected, expected * 1e-3f) << "idx " << i;
            EXPECT_NEAR(spatial[i].imag(), 0.0f, expected * 1e-3f) << "idx " << i;
        }
    }

    TEST_F(OceanFFTGpuContractTest, InverseFFT2DMatchesCpuReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 N = 64u;
        std::vector<Complex> freq(static_cast<sizet>(N) * N);
        std::mt19937 rng(4242u);
        std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
        for (auto& c : freq)
            c = Complex(dist(rng), dist(rng));

        std::vector<Complex> cpu = freq;
        Ocean::FFT2D(cpu, N, N, /*inverse=*/true);

        auto gpu = Ref<Ocean::OceanFFTGpu>::Create();
        ASSERT_TRUE(gpu->IsAvailable()) << "Ocean FFT compute shaders failed to compile";
        const auto gpuOut = gpu->DebugInverseFFT2D(freq, N);
        ASSERT_EQ(gpuOut.size(), cpu.size());

        f32 maxAbs = 0.0f;
        for (const auto& c : cpu)
            maxAbs = std::max({ maxAbs, std::abs(c.real()), std::abs(c.imag()) });
        const f32 tol = std::max(maxAbs * 1e-3f, 1e-6f);

        f32 maxErr = 0.0f;
        for (sizet i = 0; i < cpu.size(); ++i)
        {
            maxErr = std::max({ maxErr, std::abs(gpuOut[i].real() - cpu[i].real()),
                                std::abs(gpuOut[i].imag() - cpu[i].imag()) });
        }
        std::cout << "[ DIAG ] GPU-vs-CPU IFFT max error = " << maxErr << " (max magnitude " << maxAbs << ")\n";
        EXPECT_LT(maxErr, tol) << "GPU butterfly chain diverges from the CPU inverse FFT";
    }

    // -----------------------------------------------------------------------
    // 2. Full evolve → IFFT → assemble chain vs EvaluateField
    // -----------------------------------------------------------------------

    TEST_F(OceanFFTGpuContractTest, EvaluatedFieldMatchesCpuReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ocean::SpectrumParams p{};
        p.m_Resolution = 128u;
        p.m_PatchSize = 64.0f;
        p.m_WindSpeed = 18.0f;
        p.m_WindDirection = glm::vec2(1.0f, 0.3f);
        p.m_Amplitude = 4.0f;
        p.m_Choppiness = 1.4f;
        const f32 time = 7.5f;
        const u32 N = p.m_Resolution;
        const sizet count = static_cast<sizet>(N) * N;

        const auto h0 = Ocean::GenerateH0(p);
        const auto cpuField = Ocean::EvaluateField(p, h0, time);
        ASSERT_TRUE(cpuField.IsValid());

        auto gpu = Ref<Ocean::OceanFFTGpu>::Create();
        ASSERT_TRUE(gpu->IsAvailable()) << "Ocean FFT compute shaders failed to compile";
        gpu->SetH0(h0, N, p.m_PatchSize, p.m_Gravity);

        auto dispTex = MakeFieldTexture(N);
        auto derivTex = MakeFieldTexture(N);
        gpu->Evaluate(time, p.m_Choppiness, dispTex, derivTex);

        const auto gpuDisp = ReadbackRgba32f(dispTex->GetRendererID(), N);
        const auto gpuDeriv = ReadbackRgba32f(derivTex->GetRendererID(), N);

        // Tolerances scale with the field's own magnitude (the raw Phillips
        // amplitude is unitless): both sides are f32 and run the identical
        // radix-2 stage order, so only sin/cos/fma precision differs.
        const f32 heightRms = RmsOf(cpuField.m_Height);
        ASSERT_GT(heightRms, 0.0f);
        const f32 dispTol = std::max(heightRms * 0.01f, 1e-5f);

        f32 maxHeightErr = 0.0f, maxDispErr = 0.0f, maxFoamErr = 0.0f, maxNormalErr = 0.0f, maxJacErr = 0.0f;
        for (sizet i = 0; i < count; ++i)
        {
            maxHeightErr = std::max(maxHeightErr, std::abs(gpuDisp[i].y - cpuField.m_Height[i]));
            maxDispErr = std::max({ maxDispErr, std::abs(gpuDisp[i].x - cpuField.m_HorizontalDisplacement[i].x),
                                    std::abs(gpuDisp[i].z - cpuField.m_HorizontalDisplacement[i].y) });
            const f32 cpuFoam = std::clamp(1.0f - cpuField.m_Jacobian[i], 0.0f, 1.0f);
            maxFoamErr = std::max(maxFoamErr, std::abs(gpuDisp[i].w - cpuFoam));

            const glm::vec3 gpuN(gpuDeriv[i].x, gpuDeriv[i].y, gpuDeriv[i].z);
            const glm::vec3 dN = gpuN - cpuField.m_Normal[i];
            maxNormalErr = std::max({ maxNormalErr, std::abs(dN.x), std::abs(dN.y), std::abs(dN.z) });
            maxJacErr = std::max(maxJacErr, std::abs(gpuDeriv[i].w - cpuField.m_Jacobian[i]));
        }
        std::cout << "[ DIAG ] field errors — height " << maxHeightErr << ", disp " << maxDispErr << ", foam "
                  << maxFoamErr << ", normal " << maxNormalErr << ", jacobian " << maxJacErr << " (height RMS "
                  << heightRms << ")\n";

        EXPECT_LT(maxHeightErr, dispTol) << "GPU height field diverges from the CPU reference";
        EXPECT_LT(maxDispErr, dispTol) << "GPU choppy displacement diverges from the CPU reference";
        EXPECT_LT(maxFoamErr, 0.02f) << "GPU foam factor diverges from the CPU reference";
        EXPECT_LT(maxNormalErr, 0.02f) << "GPU normals diverge from the CPU reference";
        EXPECT_LT(maxJacErr, 0.02f) << "GPU Jacobian diverges from the CPU reference";
    }

    // -----------------------------------------------------------------------
    // 3. OceanFFTField end-to-end: GPU mode vs CPU mode
    // -----------------------------------------------------------------------

    TEST_F(OceanFFTGpuContractTest, FieldGpuModeMatchesCpuModeTextures)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ocean::SpectrumParams p{};
        p.m_Resolution = 128u;
        p.m_PatchSize = 64.0f;
        p.m_WindSpeed = 18.0f;
        p.m_WindDirection = glm::vec2(1.0f, 0.3f);
        p.m_Amplitude = 3.0f;
        p.m_Choppiness = 1.2f;
        const f32 time = 12.0f;
        const u32 N = p.m_Resolution;

        auto gpuField = Ref<Ocean::OceanFFTField>::Create();
        auto cpuField = Ref<Ocean::OceanFFTField>::Create();

        // Warm both (h0 generation + RMS normalisation), then time the steady
        // per-tick Update — the cost the GPU port exists to remove from the CPU.
        gpuField->Update(p, 0.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/true);
        cpuField->Update(p, 0.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/false);

        const auto t0 = std::chrono::steady_clock::now();
        gpuField->Update(p, time, true, true);
        const auto t1 = std::chrono::steady_clock::now();
        cpuField->Update(p, time, true, false);
        const auto t2 = std::chrono::steady_clock::now();
        const f64 gpuMs = std::chrono::duration<f64, std::milli>(t1 - t0).count();
        const f64 cpuMs = std::chrono::duration<f64, std::milli>(t2 - t1).count();
        std::cout << "[ DIAG ] per-tick Update CPU-side cost @128² — GPU mode " << gpuMs << " ms (incl. 64² physics proxy), CPU mode "
                  << cpuMs << " ms\n";

        ASSERT_NE(gpuField->GetDisplacementTextureID(), 0u);
        ASSERT_NE(cpuField->GetDisplacementTextureID(), 0u);

        // GPU mode must actually have engaged (band-limited physics proxy, no
        // full-res CPU field) — otherwise this test compares CPU with CPU.
        ASSERT_EQ(gpuField->GetField().m_Resolution, 64u)
            << "GPU mode did not engage (physics proxy missing) — compute path silently fell back?";
        ASSERT_EQ(cpuField->GetField().m_Resolution, N);

        const auto gpuDisp = ReadbackRgba32f(gpuField->GetDisplacementTextureID(), N);
        const auto cpuDisp = ReadbackRgba32f(cpuField->GetDisplacementTextureID(), N);
        const auto gpuDeriv = ReadbackRgba32f(gpuField->GetDerivativesTextureID(), N);
        const auto cpuDeriv = ReadbackRgba32f(cpuField->GetDerivativesTextureID(), N);

        // The fields are RMS-normalised to metres (amplitude 3 ⇒ ~0.9 m RMS).
        f32 maxDispErr = 0.0f, maxDerivErr = 0.0f;
        for (sizet i = 0; i < gpuDisp.size(); ++i)
        {
            const glm::vec4 dd = gpuDisp[i] - cpuDisp[i];
            maxDispErr = std::max({ maxDispErr, std::abs(dd.x), std::abs(dd.y), std::abs(dd.z), std::abs(dd.w) });
            const glm::vec4 dv = gpuDeriv[i] - cpuDeriv[i];
            maxDerivErr = std::max({ maxDerivErr, std::abs(dv.x), std::abs(dv.y), std::abs(dv.z), std::abs(dv.w) });
        }
        std::cout << "[ DIAG ] end-to-end texture errors — displacement " << maxDispErr << " m, derivatives "
                  << maxDerivErr << "\n";
        EXPECT_LT(maxDispErr, 0.02f) << "GPU-mode displacement texture diverges from the CPU-mode one";
        EXPECT_LT(maxDerivErr, 0.05f) << "GPU-mode derivatives texture diverges from the CPU-mode one";

        // The physics proxy must keep SampleHeight tracking the same surface
        // (band-limited, so a small divergence is expected and bounded).
        f32 maxSampleErr = 0.0f;
        for (glm::vec2 xz : { glm::vec2(0.0f), glm::vec2(10.5f, 20.25f), glm::vec2(-7.3f, 41.0f),
                              glm::vec2(63.0f, 63.0f), glm::vec2(100.0f, -55.5f) })
        {
            maxSampleErr = std::max(maxSampleErr, std::abs(gpuField->SampleHeight(xz) - cpuField->SampleHeight(xz)));
        }
        std::cout << "[ DIAG ] physics-proxy SampleHeight max divergence = " << maxSampleErr << " m\n";
        EXPECT_LT(maxSampleErr, 0.1f)
            << "band-limited physics proxy no longer tracks the rendered surface";
    }
} // namespace OloEngine::Tests
