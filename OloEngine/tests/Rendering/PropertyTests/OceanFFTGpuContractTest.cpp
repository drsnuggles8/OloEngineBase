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
#include "OloEngine/Renderer/Ocean/OceanCascades.h"
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
#include <limits>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity
using OloEngine::Ocean::Complex;

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] f32 RmsOf(const std::vector<f32>& v)
        {
            if (v.empty())
                return 0.0f;
            f64 acc = 0.0;
            for (f32 x : v)
                acc += static_cast<f64>(x) * x;
            return static_cast<f32>(std::sqrt(acc / static_cast<f64>(v.size())));
        }

        // The cascade outputs are 2D ARRAYS since issue #969 — one layer per
        // band — so a readback returns every layer back to back.
        [[nodiscard]] std::vector<glm::vec4> ReadbackRgba32fArray(u32 textureID, u32 resolution, u32 layers)
        {
            std::vector<glm::vec4> out(static_cast<sizet>(resolution) * resolution * layers);
            // Defaults, in case an earlier test left pack state dirty.
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glGetTextureImage(textureID, 0, GL_RGBA, GL_FLOAT,
                              static_cast<GLsizei>(out.size() * sizeof(glm::vec4)), out.data());
            return out;
        }

        [[nodiscard]] Ref<Texture2DArray> MakeFieldTexture(u32 resolution, u32 layers = 1u)
        {
            Texture2DArraySpecification spec;
            spec.Width = resolution;
            spec.Height = resolution;
            spec.Layers = layers;
            spec.Format = Texture2DArrayFormat::RGBA32F;
            spec.GenerateMipmaps = false;
            return Texture2DArray::Create(spec);
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
        gpu->Evaluate(time, p.m_Choppiness, dispTex, derivTex, /*layer=*/0u);

        const auto gpuDisp = ReadbackRgba32fArray(dispTex->GetRendererID(), N, 1u);
        const auto gpuDeriv = ReadbackRgba32fArray(derivTex->GetRendererID(), N, 1u);

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

        // One cascade ⇒ one array layer; the readback is the whole array.
        ASSERT_EQ(gpuField->GetCascadeCount(), 1u);
        const auto gpuDisp = ReadbackRgba32fArray(gpuField->GetDisplacementTextureID(), N, 1u);
        const auto cpuDisp = ReadbackRgba32fArray(cpuField->GetDisplacementTextureID(), N, 1u);
        const auto gpuDeriv = ReadbackRgba32fArray(gpuField->GetDerivativesTextureID(), N, 1u);
        const auto cpuDeriv = ReadbackRgba32fArray(cpuField->GetDerivativesTextureID(), N, 1u);

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

    // -----------------------------------------------------------------------
    // 4. The band-limited three-cascade preset (issue #969)
    // -----------------------------------------------------------------------

    // The texture-space sum, written straight off the contract in
    // Ocean/OceanCascades.h and evaluated on READ-BACK TEXELS — i.e. on the
    // exact bytes the water shader samples. This is a third evaluation of one
    // function, and that is the point: OceanFFTField::SampleCascades and
    // include/OceanCascadeCommon.glsl are the two production halves, and a test
    // that re-read the CPU field would only be checking the CPU half against
    // itself.
    namespace
    {
        struct TexelCascadeSum
        {
            f32 Height = 0.0f;
            glm::vec2 Horizontal{ 0.0f };
            f32 Foam = 0.0f;
        };

        [[nodiscard]] glm::vec4 BilinearLayer(const std::vector<glm::vec4>& texels, u32 resolution, u32 layer,
                                              glm::vec2 uv)
        {
            const f32 fN = static_cast<f32>(resolution);
            f32 gx = uv.x * fN;
            f32 gz = uv.y * fN;
            gx -= std::floor(gx / fN) * fN;
            gz -= std::floor(gz / fN) * fN;
            const u32 x0 = static_cast<u32>(gx) % resolution;
            const u32 z0 = static_cast<u32>(gz) % resolution;
            const u32 x1 = (x0 + 1u) % resolution;
            const u32 z1 = (z0 + 1u) % resolution;
            const f32 tx = gx - std::floor(gx);
            const f32 tz = gz - std::floor(gz);
            const sizet base = static_cast<sizet>(layer) * resolution * resolution;
            const glm::vec4 a = texels[base + static_cast<sizet>(z0) * resolution + x0];
            const glm::vec4 b = texels[base + static_cast<sizet>(z0) * resolution + x1];
            const glm::vec4 c = texels[base + static_cast<sizet>(z1) * resolution + x0];
            const glm::vec4 d = texels[base + static_cast<sizet>(z1) * resolution + x1];
            return glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), tz);
        }

        [[nodiscard]] TexelCascadeSum SumFromTexels(const Ocean::CascadePreset& preset,
                                                    const std::vector<glm::vec4>& disp, glm::vec2 worldXZ)
        {
            TexelCascadeSum out;
            for (u32 i = 0u; i < preset.m_Count; ++i)
            {
                const f32 c = std::cos(preset.m_Bands[i].m_DomainRotation);
                const f32 sn = std::sin(preset.m_Bands[i].m_DomainRotation);
                const glm::vec2 uv = Ocean::RotateVec2(worldXZ, c, sn) / preset.m_Bands[i].m_PatchSize;
                const glm::vec4 d = BilinearLayer(disp, preset.m_ArrayResolution, i, uv);
                out.Height += d.y;
                out.Horizontal += Ocean::RotateVec2(glm::vec2(d.x, d.z), c, -sn);
                out.Foam += d.w;
            }
            out.Foam = std::clamp(out.Foam, 0.0f, 1.0f);
            return out;
        }
    } // namespace

    TEST_F(OceanFFTGpuContractTest, ThreeCascadeFieldProducesEveryLayerOnTheGpu)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ocean::SpectrumParams p{};
        p.m_Resolution = 128u;
        p.m_PatchSize = 64.0f;
        p.m_WindSpeed = 16.0f;
        p.m_WindDirection = glm::vec2(1.0f, 0.25f);
        p.m_Amplitude = 3.0f;
        p.m_Choppiness = 1.1f;
        p.m_CascadeCount = 3u;

        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 5.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/true);

        const Ocean::CascadePreset& preset = field->GetPreset();
        ASSERT_EQ(preset.m_Count, 3u);
        ASSERT_NE(field->GetDisplacementTextureID(), 0u);
        ASSERT_NE(field->GetDerivativesTextureID(), 0u);
        // GPU mode must actually have engaged, or this compares CPU with CPU.
        // Asserted as the PROPERTY (the retained CPU field is a reduced proxy,
        // not the full grid) rather than against a fixed size: the proxy grid is
        // derived per band from that band's occupied bins, so a literal here
        // pins a number that legitimately moves.
        ASSERT_LT(field->GetCascadeField(0).m_Resolution, preset.m_ArrayResolution)
            << "GPU mode did not engage (physics proxy missing) — compute path silently fell back?";
        for (u32 i = 0u; i < preset.m_Count; ++i)
            EXPECT_EQ(field->GetCascadeField(i).m_Resolution, preset.m_Bands[i].m_PhysicsResolution)
                << "band " << i << " did not use its derived physics-proxy grid";

        const auto disp = ReadbackRgba32fArray(field->GetDisplacementTextureID(), preset.m_ArrayResolution, 3u);
        const auto deriv = ReadbackRgba32fArray(field->GetDerivativesTextureID(), preset.m_ArrayResolution, 3u);

        // EVERY layer must carry a real field. A dispatch that wrote the wrong
        // layer — or wrote layer 0 three times — leaves the others at their
        // allocation contents, which is the failure this catches and which
        // renders as a perfectly plausible one-cascade sea.
        const sizet perLayer = static_cast<sizet>(preset.m_ArrayResolution) * preset.m_ArrayResolution;
        for (u32 layer = 0u; layer < 3u; ++layer)
        {
            f64 acc = 0.0;
            for (sizet i = 0; i < perLayer; ++i)
            {
                const f32 h = disp[static_cast<sizet>(layer) * perLayer + i].y;
                ASSERT_TRUE(std::isfinite(h)) << "layer " << layer << " texel " << i << " is not finite";
                acc += static_cast<f64>(h) * h;
                // The derivatives layer must hold an upward normal.
                const glm::vec4 n = deriv[static_cast<sizet>(layer) * perLayer + i];
                ASSERT_GT(n.y, 0.0f) << "layer " << layer << " normal points down";
            }
            const f64 rms = std::sqrt(acc / static_cast<f64>(perLayer));
            std::cout << "[ DIAG ] cascade layer " << layer << " height RMS = " << rms << " m\n";
            EXPECT_GT(rms, 1e-4) << "cascade layer " << layer << " is empty";
        }

        // The bands are disjoint, so no two layers may hold the same field —
        // which is what "the same chain ran three times" would look like.
        for (u32 a = 0u; a < 3u; ++a)
        {
            for (u32 b = a + 1u; b < 3u; ++b)
            {
                f64 maxDiff = 0.0;
                for (sizet i = 0; i < perLayer; ++i)
                    maxDiff = std::max(maxDiff,
                                       static_cast<f64>(std::abs(disp[static_cast<sizet>(a) * perLayer + i].y -
                                                                 disp[static_cast<sizet>(b) * perLayer + i].y)));
                EXPECT_GT(maxDiff, 1e-4) << "cascade layers " << a << " and " << b << " hold the same field";
            }
        }
    }

    TEST_F(OceanFFTGpuContractTest, SummedHeightMatchesBetweenTheCpuSamplerAndTheCascadeTextures)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The issue's "CPU/GPU summed height parity ... for all enabled
        // cascades" criterion. Run in CPU-generation mode so the retained CPU
        // fields are full resolution and the ONLY difference between the two
        // sides is the summation itself — a band-limited physics proxy would
        // put a real, expected divergence between them and leave the assertion
        // unable to tell that apart from a wrong rotation or a swapped layer.
        Ocean::SpectrumParams p{};
        p.m_Resolution = 64u;
        p.m_PatchSize = 90.0f;
        p.m_WindSpeed = 14.0f;
        p.m_WindDirection = glm::vec2(1.0f, 0.4f);
        p.m_Amplitude = 3.0f;
        p.m_Choppiness = 1.2f;
        p.m_CascadeCount = 3u;

        auto field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(p, 9.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/false);
        const Ocean::CascadePreset& preset = field->GetPreset();
        ASSERT_EQ(preset.m_Count, 3u);
        ASSERT_NE(field->GetDisplacementTextureID(), 0u);

        const auto disp = ReadbackRgba32fArray(field->GetDisplacementTextureID(), preset.m_ArrayResolution, 3u);

        f32 maxHeightErr = 0.0f;
        f32 maxHorizErr = 0.0f;
        f32 maxFoamErr = 0.0f;
        f32 peakHeight = 0.0f;
        for (int zi = 0; zi < 16; ++zi)
        {
            for (int xi = 0; xi < 16; ++xi)
            {
                const glm::vec2 xz(static_cast<f32>(xi) * 13.7f - 90.0f, static_cast<f32>(zi) * 17.3f - 110.0f);
                const auto cpu = field->SampleCascades(xz);
                const auto tex = SumFromTexels(preset, disp, xz);
                maxHeightErr = std::max(maxHeightErr, std::abs(cpu.Height - tex.Height));
                maxHorizErr = std::max({ maxHorizErr, std::abs(cpu.Horizontal.x - tex.Horizontal.x),
                                         std::abs(cpu.Horizontal.y - tex.Horizontal.y) });
                maxFoamErr = std::max(maxFoamErr, std::abs(cpu.Foam - tex.Foam));
                peakHeight = std::max(peakHeight, std::abs(cpu.Height));
            }
        }
        std::cout << "[ DIAG ] summed parity — height " << maxHeightErr << " m, horizontal " << maxHorizErr
                  << " m, foam " << maxFoamErr << " (peak height " << peakHeight << " m)\n";
        ASSERT_GT(peakHeight, 0.1f) << "the sea is flat — every assertion below would pass on nothing";
        EXPECT_LT(maxHeightErr, 1e-3f)
            << "the CPU sampler and the cascade textures disagree about the summed height — a layer order, "
               "a tile scale or a rotation direction differs between the two halves";
        EXPECT_LT(maxHorizErr, 1e-3f) << "the summed choppy displacement disagrees between the two halves";
        EXPECT_LT(maxFoamErr, 1e-3f) << "the summed foam disagrees between the two halves";
    }

    TEST_F(OceanFFTGpuContractTest, CascadeArraysTileRatherThanClamp)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Every cascade layer is periodic over its own patch size, and the water
        // shader addresses it with an unbounded worldXZ * (1/L). The single-
        // cascade field got REPEAT for free from the GL texture default; the
        // Texture2DArray it moved onto defaults to CLAMP_TO_EDGE, which renders
        // one tile of sea and smears its border across the rest of the frame.
        // That is an invariant a container swap drops silently and no math test
        // can see, so it is asserted on the texture object itself.
        Ocean::SpectrumParams p{};
        p.m_Resolution = 64u;
        p.m_PatchSize = 80.0f;
        p.m_Amplitude = 2.0f;

        for (u32 cascades : { 1u, 3u })
        {
            p.m_CascadeCount = cascades;
            auto field = Ref<Ocean::OceanFFTField>::Create();
            field->Update(p, 1.0f, true, true);
            for (u32 id : { field->GetDisplacementTextureID(), field->GetDerivativesTextureID() })
            {
                ASSERT_NE(id, 0u);
                GLint wrapS = 0;
                GLint wrapT = 0;
                glGetTextureParameteriv(id, GL_TEXTURE_WRAP_S, &wrapS);
                glGetTextureParameteriv(id, GL_TEXTURE_WRAP_T, &wrapT);
                EXPECT_EQ(wrapS, GL_REPEAT) << "cascades=" << cascades << ": the field does not tile in S";
                EXPECT_EQ(wrapT, GL_REPEAT) << "cascades=" << cascades << ": the field does not tile in T";
            }
        }
    }

    TEST_F(OceanFFTGpuContractTest, SwitchingCascadeCountRebuildsTheTextureArray)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Resource lifetime across the opt-in: the arrays are allocated for a
        // layer count, and toggling the preset has to rebuild them rather than
        // leave a one-layer array that a three-band draw would sample layer 2 of.
        Ocean::SpectrumParams p{};
        p.m_Resolution = 64u;
        p.m_PatchSize = 80.0f;
        p.m_Amplitude = 2.0f;

        auto field = Ref<Ocean::OceanFFTField>::Create();

        p.m_CascadeCount = 1u;
        field->Update(p, 1.0f, true, true);
        ASSERT_EQ(field->GetCascadeCount(), 1u);
        ASSERT_TRUE(field->GetDisplacementTextureHandle().IsValid());

        p.m_CascadeCount = 3u;
        field->Update(p, 1.0f, true, true);
        ASSERT_EQ(field->GetCascadeCount(), 3u);
        ASSERT_TRUE(field->GetDisplacementTextureHandle().IsValid());
        const u32 arrayRes = field->GetPreset().m_ArrayResolution;
        const auto disp = ReadbackRgba32fArray(field->GetDisplacementTextureID(), arrayRes, 3u);
        ASSERT_EQ(disp.size(), static_cast<sizet>(arrayRes) * arrayRes * 3u);
        for (const glm::vec4& t : disp)
            ASSERT_TRUE(std::isfinite(t.y)) << "the widened array holds uninitialised texels";

        // ...and back, which is the direction that would otherwise leave a
        // three-layer array in place with two stale bands still in it.
        p.m_CascadeCount = 1u;
        field->Update(p, 1.0f, true, true);
        EXPECT_EQ(field->GetCascadeCount(), 1u);
        EXPECT_TRUE(field->GetDisplacementTextureHandle().IsValid());
    }
    // TEMPORARY (#1015): the AMD flat-ocean trigger is the AMPLITUDE, not the
    // patch size or the camera.
    //
    // DiagPatchAndPoseSweep showed every visual-evidence case landing at a
    // normal luma spread of 22-26 on the box EXCEPT amplitude 4, which collapses
    // to 3.7 while amplitude 3 at the identical patch, pose and spectrum is
    // fine. The CPU side of the amplitude is a single linear multiply into h0,
    // so a 4/3 increase producing a flat sea means something downstream is not
    // linear. This reads the displacement texture straight off the field and
    // reports what is actually in it -- NaN, zero, or clamped are three
    // different bugs and the rendered frame cannot tell them apart.
    TEST_F(OceanFFTGpuContractTest, DiagAmplitudeSweepDisplacementStats)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ocean::SpectrumParams p{};
        p.m_Resolution = 128u;
        p.m_PatchSize = 200.0f;
        p.m_WindSpeed = 18.0f;
        p.m_WindDirection = glm::vec2(1.0f, 0.3f);
        p.m_Choppiness = 1.4f;
        const f32 time = 12.0f;
        const u32 N = p.m_Resolution;

        const auto report = [&](const char* mode, f32 amplitude, const std::vector<glm::vec4>& texels)
        {
            f64 sumSq = 0.0;
            f32 lo = std::numeric_limits<f32>::infinity();
            f32 hi = -std::numeric_limits<f32>::infinity();
            sizet nans = 0, infs = 0;
            for (const glm::vec4& t : texels)
            {
                for (int ch = 0; ch < 3; ++ch)
                {
                    const f32 v = t[ch];
                    if (std::isnan(v))
                    {
                        ++nans;
                        continue;
                    }
                    if (std::isinf(v))
                    {
                        ++infs;
                        continue;
                    }
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    sumSq += static_cast<f64>(v) * v;
                }
            }
            const f64 rms = texels.empty() ? 0.0 : std::sqrt(sumSq / (static_cast<f64>(texels.size()) * 3.0));
            GTEST_LOG_(INFO) << "DIAGAMP " << mode << " amp=" << amplitude << " texels=" << texels.size()
                             << " rms=" << rms << " min=" << lo << " max=" << hi << " nan=" << nans
                             << " inf=" << infs;
        };

        // NON-MONOTONIC ON PURPOSE. In the sweep that found this, amplitude 4
        // was also the LAST case run, so "amplitude 4 is bad" and "the sixth
        // capture is bad" fit the data equally well. Putting 4 first and
        // repeating it last separates them: if both 4s collapse it is the
        // amplitude, if only the late one does it is accumulated GPU state.
        for (const f32 amplitude : { 4.0f, 1.0f, 6.0f, 3.0f, 3.5f, 4.0f })
        {
            p.m_Amplitude = amplitude;

            auto gpuField = Ref<Ocean::OceanFFTField>::Create();
            gpuField->Update(p, 0.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/true);
            gpuField->Update(p, time, true, true);
            report("gpu", amplitude, ReadbackRgba32fArray(gpuField->GetDisplacementTextureID(), N, 1u));

            auto cpuField = Ref<Ocean::OceanFFTField>::Create();
            cpuField->Update(p, 0.0f, /*uploadToGpu=*/true, /*useGpuCompute=*/false);
            cpuField->Update(p, time, true, false);
            report("cpu", amplitude, ReadbackRgba32fArray(cpuField->GetDisplacementTextureID(), N, 1u));
        }
    }

} // namespace OloEngine::Tests
