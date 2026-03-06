#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

#include <array>
#include <cstring>

namespace OloEngine
{
    // L2 spherical harmonics: 9 coefficients per RGB channel
    static constexpr u32 SH_COEFFICIENT_COUNT = 9;

    struct SHCoefficients
    {
        std::array<glm::vec3, SH_COEFFICIENT_COUNT> Coefficients{};

        void Zero()
        {
            Coefficients.fill(glm::vec3(0.0f));
        }

        void Accumulate(const SHCoefficients& other)
        {
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                Coefficients[i] += other.Coefficients[i];
            }
        }

        void Scale(f32 factor)
        {
            for (auto& c : Coefficients)
            {
                c *= factor;
            }
        }

        // Convert to flat vec4 array for GPU upload (std430-friendly).
        // The .w of the first element is set to the validity flag.
        void ToGPULayout(std::array<glm::vec4, SH_COEFFICIENT_COUNT>& out, f32 validity = 1.0f) const
        {
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                out[i] = glm::vec4(Coefficients[i], 0.0f);
            }
            out[0].w = validity;
        }

        // Read back from flat vec4 array (GPU layout)
        void FromGPULayout(const std::array<glm::vec4, SH_COEFFICIENT_COUNT>& in)
        {
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                Coefficients[i] = glm::vec3(in[i]);
            }
        }
    };

    static_assert(sizeof(SHCoefficients) == SH_COEFFICIENT_COUNT * sizeof(glm::vec3),
                  "SHCoefficients layout mismatch");

    // GPU-side size per probe: 9 vec4s
    static constexpr u32 SH_GPU_FLOATS_PER_PROBE = SH_COEFFICIENT_COUNT * 4;
    static constexpr u32 SH_GPU_BYTES_PER_PROBE = SH_GPU_FLOATS_PER_PROBE * sizeof(f32);

    // =============================================================================
    // L2 SH basis function constants
    // These must match the GLSL implementation in SphericalHarmonics.glsl
    // =============================================================================

    namespace SHBasis
    {
        // Y_0^0
        inline constexpr f32 Y00 = 0.282095f; // 1 / (2 * sqrt(pi))

        // Y_1^{-1}, Y_1^0, Y_1^1
        inline constexpr f32 Y1n1 = 0.488603f; // sqrt(3) / (2 * sqrt(pi))
        inline constexpr f32 Y10 = 0.488603f;
        inline constexpr f32 Y11 = 0.488603f;

        // Y_2^{-2}, Y_2^{-1}, Y_2^0, Y_2^1, Y_2^2
        inline constexpr f32 Y2n2 = 1.092548f; // sqrt(15) / (2 * sqrt(pi))
        inline constexpr f32 Y2n1 = 1.092548f;
        inline constexpr f32 Y20 = 0.315392f; // sqrt(5) / (4 * sqrt(pi))
        inline constexpr f32 Y21 = 1.092548f;
        inline constexpr f32 Y22 = 0.546274f; // sqrt(15) / (4 * sqrt(pi))

        // Evaluate all 9 L2 SH basis functions for a given direction.
        // Precondition: dir must be a unit vector (normalized).
        inline std::array<f32, SH_COEFFICIENT_COUNT> Evaluate(const glm::vec3& dir)
        {
            OLO_CORE_ASSERT(glm::abs(glm::length(dir) - 1.0f) < 0.01f, "SHBasis::Evaluate requires a normalized direction");
            return {
                Y00,
                Y1n1 * dir.y,
                Y10 * dir.z,
                Y11 * dir.x,
                Y2n2 * dir.x * dir.y,
                Y2n1 * dir.y * dir.z,
                Y20 * (3.0f * dir.z * dir.z - 1.0f),
                Y21 * dir.x * dir.z,
                Y22 * (dir.x * dir.x - dir.y * dir.y)
            };
        }

        // Evaluate SH irradiance for a given normal using stored coefficients
        inline glm::vec3 EvaluateIrradiance(const SHCoefficients& sh, const glm::vec3& normal)
        {
            auto basis = Evaluate(normal);
            glm::vec3 result(0.0f);
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                result += sh.Coefficients[i] * basis[i];
            }
            return glm::max(result, glm::vec3(0.0f));
        }
    } // namespace SHBasis
} // namespace OloEngine
