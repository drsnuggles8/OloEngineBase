#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Ocean/OceanFFT.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace OloEngine::Ocean
{
    // =========================================================================
    // GPU compute butterfly port of the Tessendorf FFT ocean *generation*
    // (docs/WATER_FUTURE_IMPROVEMENTS.md §1.2, the §6.4 transition path).
    //
    // Per Evaluate() it dispatches, entirely on the GPU:
    //   1. Ocean_SpectrumEvolve.comp — time-evolve h0(k) into 8 complex
    //      spectra (height, choppy x/z, slopes, displacement gradients),
    //      packed two-per-texel into a 4-layer RGBA32F image array.
    //   2. Ocean_FFTButterfly.comp ×(2·log2 N) — radix-2 Cooley-Tukey inverse
    //      FFT (horizontal then vertical), ping-ponging two image arrays with
    //      a precomputed twiddle/index LUT (stage 0 absorbs the bit-reversal).
    //   3. Ocean_Assemble.comp — normalise by 1/N², derive normal/Jacobian/
    //      foam, and image-store the SAME two RGBA32F textures the CPU path
    //      uploads (displacement rgb=(dx,h,dz) a=foam; derivatives rgb=normal
    //      a=jacobian) — the water shader is untouched.
    //
    // The base spectrum h0 stays CPU-generated (deterministic mt19937 — the
    // GPU cannot reproduce it) and is uploaded only when it changes. The CPU
    // implementation in OceanSpectrum/OceanFFT is the validation reference;
    // OceanFFTGpuContractTest compares the two within float tolerance.
    //
    // All methods require a live GL 4.6 context. IsAvailable() lazily compiles
    // the three compute shaders and reports false on failure so callers
    // (OceanFFTField) can fall back to the CPU path.
    // =========================================================================
    class OceanFFTGpu : public RefCounted
    {
      public:
        OceanFFTGpu() = default;
        ~OceanFFTGpu() override = default;

        OceanFFTGpu(const OceanFFTGpu&) = delete;
        OceanFFTGpu& operator=(const OceanFFTGpu&) = delete;

        /// Lazily compiles the compute shaders on first call (requires a live
        /// GL context). False ⇒ use the CPU path.
        [[nodiscard]] bool IsAvailable();

        /// Upload the (already amplitude-normalised) base spectrum h0 and
        /// (re)build the per-resolution butterfly LUT and ping-pong arrays.
        /// `h0` is the GenerateH0()-layout N×N grid.
        void SetH0(const std::vector<Complex>& h0, u32 resolution, f32 patchSize, f32 gravity);

        [[nodiscard]] bool HasH0() const noexcept
        {
            return m_Resolution != 0u;
        }

        /// Run the full evolve → IFFT → assemble chain at `time` seconds,
        /// image-storing into the two RGBA32F textures (must match the h0
        /// resolution). Issues the memory barrier the sampling shader needs.
        void Evaluate(f32 time, f32 choppiness, const Ref<Texture2D>& displacementTex,
                      const Ref<Texture2D>& derivativesTex);

        /// Test hook: inverse-2D-FFT an N×N complex grid through the exact GPU
        /// butterfly chain and read it back (normalised by 1/N² on the CPU,
        /// where the production path defers it to the assemble pass). Lets the
        /// riskiest math be pinned against the CPU FFT2D directly. Returns an
        /// empty vector when the GPU path is unavailable.
        [[nodiscard]] std::vector<Complex> DebugInverseFFT2D(const std::vector<Complex>& freq, u32 resolution);

      private:
        [[nodiscard]] bool EnsureShaders();
        void EnsureResources(u32 resolution);
        void UploadButterflyLut(u32 resolution);
        /// Dispatch the 2·log2(N) butterfly passes starting from ping-pong
        /// index `srcIndex`; returns the index holding the final result.
        [[nodiscard]] u32 RunButterflyPasses(u32 srcIndex);

        bool m_ShaderInitAttempted = false;
        bool m_ShadersValid = false;
        Ref<ComputeShader> m_EvolveShader;
        Ref<ComputeShader> m_ButterflyShader;
        Ref<ComputeShader> m_AssembleShader;

        u32 m_Resolution = 0u;
        f32 m_PatchSize = 0.0f;
        f32 m_Gravity = 9.81f;

        Ref<Texture2D> m_H0Tex;                        // rg = h0(k), ba = conj(h0(-k))
        Ref<Texture2D> m_ButterflyTex;                 // log2(N)×N: rg = ±twiddle, ba = gather indices
        std::array<Ref<Texture2DArray>, 2> m_PingPong; // 4-layer RGBA32F spectra arrays

        std::vector<glm::vec4> m_Scratch; // CPU staging for uploads
    };
} // namespace OloEngine::Ocean
