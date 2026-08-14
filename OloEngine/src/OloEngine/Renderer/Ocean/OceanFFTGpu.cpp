#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Ocean/OceanFFTGpu.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <bit>
#include <cmath>

// =============================================================================
// Host side of the GPU compute butterfly FFT ocean (see OceanFFTGpu.h).
// The CPU implementation (OceanSpectrum/OceanFFT) is the validation reference:
// every convention here — FFT-natural frequency ordering, the inverse
// e^{+i2πkn/N} kernel, deferred 1/N² normalisation, spectra derivation —
// deliberately mirrors it so OceanFFTGpuContractTest can compare the outputs.
// =============================================================================

namespace OloEngine::Ocean
{
    namespace
    {
        constexpr f32 kTwoPi = 6.28318530717958647692f;
        constexpr u32 kSpectraLayers = 4u;
        constexpr u32 kLocalSize = 8u; // matches local_size_x/y in the .comp files

        [[nodiscard]] u32 BitReverse(u32 v, u32 bits)
        {
            u32 r = 0u;
            for (u32 i = 0u; i < bits; ++i)
            {
                r = (r << 1u) | (v & 1u);
                v >>= 1u;
            }
            return r;
        }

        // Std140 twin of the OceanFFTParams block shared by the three
        // Ocean_*.comp passes. File-scope rather than a member (the header is
        // out of this slice's reach), shared across OceanFFTGpu instances —
        // sound because it is refilled before every dispatch on the render
        // thread. Lives until process exit: the class has no static shutdown
        // seam to release it from.
        Ref<UniformBuffer> s_FFTParamsUBO;

        // Former bare uniforms via ComputeShader::Set*, a deliberate no-op on
        // the Vulkan route — now one std140 refill per dispatch (issue #691
        // Phase 8, the HZB pattern: GL re-uploads the bound buffer; the Vulkan
        // arena mints a fresh address each SetData).
        void UploadFFTParams(const UBOStructures::OceanFFTUBO& params)
        {
            if (!s_FFTParamsUBO)
            {
                s_FFTParamsUBO = UniformBuffer::Create(UBOStructures::OceanFFTUBO::GetSize(),
                                                       ShaderBindingLayout::UBO_OCEAN_FFT);
            }
            s_FFTParamsUBO->SetData(&params, sizeof(params));
            s_FFTParamsUBO->Bind();
        }
    } // namespace

    bool OceanFFTGpu::IsAvailable()
    {
        return EnsureShaders();
    }

    bool OceanFFTGpu::EnsureShaders()
    {
        if (m_ShaderInitAttempted)
            return m_ShadersValid;
        m_ShaderInitAttempted = true;

        m_EvolveShader = ComputeShader::Create("assets/shaders/compute/Ocean_SpectrumEvolve.comp");
        m_ButterflyShader = ComputeShader::Create("assets/shaders/compute/Ocean_FFTButterfly.comp");
        m_AssembleShader = ComputeShader::Create("assets/shaders/compute/Ocean_Assemble.comp");

        m_ShadersValid = m_EvolveShader && m_EvolveShader->IsValid() && m_ButterflyShader &&
                         m_ButterflyShader->IsValid() && m_AssembleShader && m_AssembleShader->IsValid();
        if (!m_ShadersValid)
        {
            OLO_CORE_WARN("OceanFFTGpu: compute shaders unavailable — falling back to the CPU FFT path");
            m_EvolveShader = nullptr;
            m_ButterflyShader = nullptr;
            m_AssembleShader = nullptr;
        }
        return m_ShadersValid;
    }

    void OceanFFTGpu::EnsureResources(u32 resolution)
    {
        const bool needsCreate = !m_PingPong[0] || m_PingPong[0]->GetWidth() != resolution;
        if (!needsCreate)
            return;

        Texture2DArraySpecification arraySpec;
        arraySpec.Width = resolution;
        arraySpec.Height = resolution;
        arraySpec.Layers = kSpectraLayers;
        arraySpec.Format = Texture2DArrayFormat::RGBA32F;
        m_PingPong[0] = Texture2DArray::Create(arraySpec);
        m_PingPong[1] = Texture2DArray::Create(arraySpec);

        TextureSpecification h0Spec;
        h0Spec.Width = resolution;
        h0Spec.Height = resolution;
        h0Spec.Format = ImageFormat::RGBA32F;
        h0Spec.GenerateMips = false;
        h0Spec.MipLevels = 1;
        m_H0Tex = Texture2D::Create(h0Spec);

        UploadButterflyLut(resolution);
    }

    void OceanFFTGpu::UploadButterflyLut(u32 resolution)
    {
        // LUT layout (x = stage, y = output index): rg = twiddle (the bottom
        // butterfly wing bakes the minus sign into the factor, so the shader
        // kernel is uniformly out = in[a] + w·in[b]), ba = gather indices.
        // Stage 0's indices are bit-reversed, absorbing the reorder step of
        // the CPU FFT1D. Twiddles use the INVERSE kernel sign (e^{+i...}) —
        // this pipeline only ever inverse-transforms.
        const u32 stages = static_cast<u32>(std::countr_zero(resolution));

        TextureSpecification lutSpec;
        lutSpec.Width = stages;
        lutSpec.Height = resolution;
        lutSpec.Format = ImageFormat::RGBA32F;
        lutSpec.GenerateMips = false;
        lutSpec.MipLevels = 1;
        m_ButterflyTex = Texture2D::Create(lutSpec);

        m_Scratch.assign(static_cast<sizet>(stages) * resolution, glm::vec4(0.0f));
        for (u32 stage = 0u; stage < stages; ++stage)
        {
            const u32 half = 1u << stage;
            const u32 len = half << 1u;
            for (u32 x = 0u; x < resolution; ++x)
            {
                const u32 j = x % len;
                const bool topWing = j < half;
                const u32 k = topWing ? j : j - half;
                u32 a = topWing ? x : x - half;
                u32 b = a + half;
                if (stage == 0u)
                {
                    a = BitReverse(a, stages);
                    b = BitReverse(b, stages);
                }

                const f32 theta = kTwoPi * static_cast<f32>(k) / static_cast<f32>(len);
                glm::vec2 w(std::cos(theta), std::sin(theta));
                if (!topWing)
                    w = -w;

                // Row-major (y * stages + x) with width == stages.
                m_Scratch[static_cast<sizet>(x) * stages + stage] =
                    glm::vec4(w.x, w.y, static_cast<f32>(a), static_cast<f32>(b));
            }
        }
        m_ButterflyTex->SetData(m_Scratch.data(), static_cast<u32>(m_Scratch.size() * sizeof(glm::vec4)));
    }

    void OceanFFTGpu::SetH0(const std::vector<Complex>& h0, u32 resolution, f32 patchSize, f32 gravity)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(IsPowerOfTwo(resolution), "OceanFFTGpu::SetH0: resolution must be a power of two");
        OLO_CORE_ASSERT(h0.size() == static_cast<sizet>(resolution) * resolution, "OceanFFTGpu::SetH0: h0 size mismatch");
        if (!EnsureShaders())
            return;

        EnsureResources(resolution);
        m_Resolution = resolution;
        m_PatchSize = patchSize;
        m_Gravity = gravity;

        // Pack rg = h0(k), ba = conj(h0(-k)) so the evolve pass needs one fetch.
        const u32 N = resolution;
        m_Scratch.resize(static_cast<sizet>(N) * N);
        for (u32 m = 0u; m < N; ++m)
        {
            const u32 mm = (N - m) % N;
            for (u32 n = 0u; n < N; ++n)
            {
                const u32 mn = (N - n) % N;
                const Complex k = h0[static_cast<sizet>(m) * N + n];
                const Complex mk = h0[static_cast<sizet>(mm) * N + mn];
                m_Scratch[static_cast<sizet>(m) * N + n] = glm::vec4(k.real(), k.imag(), mk.real(), -mk.imag());
            }
        }
        m_H0Tex->SetData(m_Scratch.data(), static_cast<u32>(m_Scratch.size() * sizeof(glm::vec4)));
    }

    u32 OceanFFTGpu::RunButterflyPasses(u32 srcIndex)
    {
        const u32 N = m_Resolution;
        const u32 stages = static_cast<u32>(std::countr_zero(N));
        const u32 groups = (N + kLocalSize - 1u) / kLocalSize;

        m_ButterflyShader->Bind();
        // Persistent: the twiddle table is pass-owned and never pooled. Staged
        // once here rather than per stage — the offset scratch persists across
        // flushes, and the loop below already flushes per iteration for the
        // ping-pong images (issue #691 Phase 3).
        HeapBinding::BindTextureOrOffset(0, m_ButterflyTex->GetRHIHandle(),
                                         RHI::HeapSlotLifetime::Persistent);

        u32 src = srcIndex;
        for (int vertical = 0; vertical <= 1; ++vertical)
        {
            for (u32 stage = 0u; stage < stages; ++stage)
            {
                // Stage/Vertical change EVERY dispatch, so the shared block is
                // refilled per iteration; value-initialisation zeroes the
                // evolve/assemble tail this pass never reads.
                UBOStructures::OceanFFTUBO fftParams{};
                fftParams.Resolution = static_cast<i32>(N);
                fftParams.Stage = static_cast<i32>(stage);
                fftParams.Vertical = vertical;
                UploadFFTParams(fftParams);
                // Both ping-pong halves are read AND written across the stage loop with
                // their roles swapping, so each texture ends up with a read view and a
                // write view. Those fold to one GL image handle, which is why the backend
                // widens its residency to READ_WRITE rather than transitioning twice.
                HeapBinding::BindImageOrOffset(0, m_PingPong[src]->GetRHIHandle(), 0, true, 0,
                                               RHI::Access::StorageRead, RHI::Format::RGBA32Float,
                                               RHI::HeapSlotLifetime::Persistent);
                HeapBinding::BindImageOrOffset(1, m_PingPong[1u - src]->GetRHIHandle(), 0, true, 0,
                                               RHI::Access::StorageWrite, RHI::Format::RGBA32Float,
                                               RHI::HeapSlotLifetime::Persistent);
                HeapBinding::FlushOffsets();
                RenderCommand::DispatchCompute(groups, groups, kSpectraLayers);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
                src = 1u - src;
            }
        }
        return src;
    }

    void OceanFFTGpu::Evaluate(f32 time, f32 choppiness, const Ref<Texture2D>& displacementTex,
                               const Ref<Texture2D>& derivativesTex)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_ShadersValid || m_Resolution == 0u)
            return;
        if (!displacementTex || !derivativesTex || displacementTex->GetWidth() != m_Resolution ||
            derivativesTex->GetWidth() != m_Resolution)
        {
            OLO_CORE_WARN("OceanFFTGpu::Evaluate: output texture resolution mismatch; skipping");
            return;
        }

        const u32 N = m_Resolution;
        const u32 groups = (N + kLocalSize - 1u) / kLocalSize;

        // 1. Time-evolve the spectra into ping-pong[0].
        {
            OLO_PROFILE_SCOPE("OceanFFTGpu::SpectrumEvolve");
            m_EvolveShader->Bind();
            UBOStructures::OceanFFTUBO fftParams{};
            fftParams.Resolution = static_cast<i32>(N);
            fftParams.PatchSize = m_PatchSize;
            fftParams.Gravity = m_Gravity;
            fftParams.Time = time;
            UploadFFTParams(fftParams);
            // Persistent: the h0 spectrum is pass-owned, generated once per
            // parameter change rather than acquired from the transient pool.
            HeapBinding::BindTextureOrOffset(0, m_H0Tex->GetRHIHandle(),
                                             RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(0, m_PingPong[0]->GetRHIHandle(), 0, true, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(groups, groups, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
        }

        // 2. Inverse FFT: 2·log2(N) butterfly passes.
        u32 finalIndex;
        {
            OLO_PROFILE_SCOPE("OceanFFTGpu::ButterflyPasses");
            finalIndex = RunButterflyPasses(0u);
        }

        // 3. Assemble the displacement/derivatives textures.
        {
            OLO_PROFILE_SCOPE("OceanFFTGpu::Assemble");
            m_AssembleShader->Bind();
            UBOStructures::OceanFFTUBO fftParams{};
            fftParams.Resolution = static_cast<i32>(N);
            fftParams.Choppiness = choppiness;
            UploadFFTParams(fftParams);
            HeapBinding::BindImageOrOffset(0, m_PingPong[finalIndex]->GetRHIHandle(), 0, true, 0,
                                           RHI::Access::StorageRead, RHI::Format::RGBA32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(1, displacementTex->GetRHIHandle(), 0, false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(2, derivativesTex->GetRHIHandle(), 0, false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(groups, groups, 1);
            // The water shader samples these textures next; image stores must
            // be visible to texture fetches (and any later image loads).
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        }
    }

    std::vector<Complex> OceanFFTGpu::DebugInverseFFT2D(const std::vector<Complex>& freq, u32 resolution)
    {
        OLO_CORE_ASSERT(IsPowerOfTwo(resolution), "OceanFFTGpu::DebugInverseFFT2D: resolution must be a power of two");
        OLO_CORE_ASSERT(freq.size() == static_cast<sizet>(resolution) * resolution,
                        "OceanFFTGpu::DebugInverseFFT2D: grid size mismatch");
        if (!EnsureShaders())
            return {};

        EnsureResources(resolution);
        m_Resolution = resolution;

        const u32 N = resolution;
        const sizet count = static_cast<sizet>(N) * N;

        // Clear both arrays (the butterfly chain transforms all 4 layers; the
        // unused ones must not feed NaN/garbage through imageLoad).
        const glm::vec4 zero(0.0f);
        RenderCommand::ClearTextureFloat(m_PingPong[0]->GetRHIHandle(), 0, zero);
        RenderCommand::ClearTextureFloat(m_PingPong[1]->GetRHIHandle(), 0, zero);

        // Upload the input into layer 0 (rg = complex, ba unused).
        m_Scratch.assign(count, glm::vec4(0.0f));
        for (sizet i = 0; i < count; ++i)
            m_Scratch[i] = glm::vec4(freq[i].real(), freq[i].imag(), 0.0f, 0.0f);
        RenderCommand::UploadTextureSubImage3D(m_PingPong[0]->GetRHIHandle(), 0, 0, 0, N, N, 1,
                                               RHI::Format::RGBA32Float, m_Scratch.data());
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate);

        const u32 finalIndex = RunButterflyPasses(0u);

        // Read back layer 0 and apply the 1/N² normalisation the production
        // path defers to the assemble pass.
        std::vector<glm::vec4> readback(count);
        if (!RenderCommand::ReadTextureSubImage(m_PingPong[finalIndex]->GetRHIHandle(), 0, 0, 0, 0,
                                                N, N, 1, RHI::Format::RGBA32Float,
                                                count * sizeof(glm::vec4), readback.data()))
        {
            // Return empty rather than normalizing an unspecified buffer — this
            // is a verification utility, so silently handing back plausible-looking
            // garbage would corrupt the very comparison it exists to make.
            OLO_CORE_WARN("OceanFFTGpu: butterfly readback failed; returning no result");
            return {};
        }

        const f32 invN2 = 1.0f / (static_cast<f32>(N) * static_cast<f32>(N));
        std::vector<Complex> result(count);
        for (sizet i = 0; i < count; ++i)
            result[i] = Complex(readback[i].x * invN2, readback[i].y * invN2);
        return result;
    }
} // namespace OloEngine::Ocean
