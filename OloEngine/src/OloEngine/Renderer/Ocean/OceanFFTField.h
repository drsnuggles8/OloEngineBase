#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Ocean/OceanSpectrum.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine::Ocean
{
    // =========================================================================
    // Runtime owner of one Tessendorf FFT ocean cascade
    // (docs/WATER_FUTURE_IMPROVEMENTS.md §1).
    //
    // Each tick it evaluates the spectral ocean field on the CPU (the validated
    // OceanSpectrum reference) and uploads it to two GPU textures the water
    // shader samples instead of summing Gerstner waves:
    //   * Displacement (RGBA32F): rgb = (dx, height, dz) choppy displacement,
    //     a = foam factor (saturate(1 - Jacobian)).
    //   * Derivatives  (RGBA32F): rgb = surface normal, a = raw Jacobian.
    // Both tile seamlessly (REPEAT) over PatchSize world metres.
    //
    // The base spectrum h0(k) is regenerated only when the wind/shape params
    // change; the per-tick cost is the time-evolution + inverse FFTs.
    //
    // A CPU copy of the field is retained so physics/buoyancy can sample the
    // exact rendered surface with no GPU readback. The class is deliberately
    // structured so a future GPU-compute butterfly port (§1.2) can replace the
    // *generation* method behind the same texture interface, exactly the
    // transition path §6.4 describes (same output texture, different producer).
    // =========================================================================
    class OceanFFTField : public RefCounted
    {
      public:
        OceanFFTField() = default;
        ~OceanFFTField() override = default;

        OceanFFTField(const OceanFFTField&) = delete;
        OceanFFTField& operator=(const OceanFFTField&) = delete;

        /// Evaluate the field at `time` seconds. Regenerates the base spectrum
        /// only when the shape params changed. When `uploadToGpu` is true the
        /// result is uploaded to the GPU textures (requires a live GL context);
        /// pass false for headless/CPU-only use (tests, physics warm-up).
        void Update(const SpectrumParams& params, f32 time, bool uploadToGpu = true);

        [[nodiscard]] u32 GetDisplacementTextureID() const;
        [[nodiscard]] u32 GetDerivativesTextureID() const;
        [[nodiscard]] f32 GetPatchSize() const noexcept
        {
            return m_Params.m_PatchSize;
        }
        [[nodiscard]] f32 GetChoppiness() const noexcept
        {
            return m_Params.m_Choppiness;
        }

        /// CPU height of the surface column above world `worldXZ` (metres),
        /// inverting the choppy horizontal shift with a short fixed-point
        /// iteration. Returns 0 before the first Update().
        [[nodiscard]] f32 SampleHeight(glm::vec2 worldXZ) const;

        [[nodiscard]] const DisplacementField& GetField() const noexcept
        {
            return m_Field;
        }

      private:
        // The subset of SpectrumParams that determines h0(k); a change here
        // forces a base-spectrum regeneration. Choppiness/time are excluded
        // (they only affect the per-tick evaluation, not h0).
        struct H0Key
        {
            u32 Resolution = 0u;
            f32 PatchSize = 0.0f;
            f32 WindSpeed = 0.0f;
            glm::vec2 WindDirection{ 0.0f };
            f32 Amplitude = 0.0f;
            f32 Gravity = 0.0f;
            f32 SmallWaveSuppression = 0.0f;
            f32 DirectionalExponent = 0.0f;
            u32 Seed = 0u;

            // Bit-exact change detection (a "slider moved" check, not a math
            // equality test) — Math::BitwiseEqual is the sanctioned form, so we
            // don't trip the no-raw-float-== rule. H0Key is a trivially-copyable
            // POD with no padding, so this is well-defined.
            [[nodiscard]] bool operator==(const H0Key& o) const noexcept
            {
                return Math::BitwiseEqual(*this, o);
            }
            [[nodiscard]] bool operator!=(const H0Key& o) const noexcept
            {
                return !(*this == o);
            }
        };
        [[nodiscard]] static H0Key MakeH0Key(const SpectrumParams& p);

        void Upload();
        void EnsureTextures(u32 resolution);
        [[nodiscard]] glm::vec2 SampleHorizontalBilinear(glm::vec2 worldXZ) const;

        SpectrumParams m_Params;
        H0Key m_H0Key;
        bool m_HasH0 = false;
        std::vector<Complex> m_H0;
        DisplacementField m_Field;

        Ref<Texture2D> m_DisplacementTex; // rgb = (dx, h, dz), a = foam
        Ref<Texture2D> m_DerivativesTex;  // rgb = normal,      a = jacobian
        std::vector<glm::vec4> m_DisplacementScratch;
        std::vector<glm::vec4> m_DerivativesScratch;
    };
} // namespace OloEngine::Ocean
