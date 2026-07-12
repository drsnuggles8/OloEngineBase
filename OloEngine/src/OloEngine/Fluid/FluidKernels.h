#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <numbers>

namespace OloEngine::FluidKernels
{
    // =========================================================================
    // SPH smoothing kernels for the Position-Based Fluids solver.
    //
    // CPU mirror of the GLSL implementations in
    // OloEditor/assets/shaders/include/FluidKernels.glsl — the two MUST stay
    // formula-identical (the GPU-vs-CPU parity test pins this). Formulas from
    // Macklin & Müller, "Position Based Fluids", ACM TOG 32(4), 2013, which
    // uses the poly6 kernel for density estimation and the spiky kernel's
    // gradient for the constraint gradient (poly6's gradient vanishes at r=0,
    // which would break the pressure push-apart).
    // =========================================================================

    /// poly6 normalization: 315 / (64 * pi * h^9). Müller et al. 2003, eq. (20).
    [[nodiscard]] constexpr f32 Poly6Scale(f32 smoothingRadius)
    {
        const f32 h3 = smoothingRadius * smoothingRadius * smoothingRadius;
        const f32 h9 = h3 * h3 * h3;
        return 315.0f / (64.0f * std::numbers::pi_v<f32> * h9);
    }

    /// spiky gradient normalization: -45 / (pi * h^6). Müller et al. 2003, eq. (21).
    [[nodiscard]] constexpr f32 SpikyGradScale(f32 smoothingRadius)
    {
        const f32 h2 = smoothingRadius * smoothingRadius;
        const f32 h6 = h2 * h2 * h2;
        return -45.0f / (std::numbers::pi_v<f32> * h6);
    }

    /// W_poly6(r, h) = Poly6Scale(h) * (h^2 - r^2)^3 for r < h, else 0.
    /// Precomputed-scale overload for inner loops.
    [[nodiscard]] constexpr f32 Poly6(f32 distance, f32 smoothingRadius, f32 poly6Scale)
    {
        if (distance >= smoothingRadius || distance < 0.0f)
        {
            return 0.0f;
        }
        const f32 diff = (smoothingRadius * smoothingRadius) - (distance * distance);
        return poly6Scale * diff * diff * diff;
    }

    [[nodiscard]] constexpr f32 Poly6(f32 distance, f32 smoothingRadius)
    {
        return Poly6(distance, smoothingRadius, Poly6Scale(smoothingRadius));
    }

    /// grad W_spiky(rVec, h) = SpikyGradScale(h) * (h - r)^2 * rVec / r for 0 < r < h.
    /// Returns the zero vector at r = 0 (the gradient is undefined there; every
    /// consumer treats a coincident pair as contributing no push direction) and
    /// outside the support radius.
    [[nodiscard]] inline glm::vec3 SpikyGrad(const glm::vec3& offset, f32 smoothingRadius, f32 spikyGradScale)
    {
        const f32 distance = glm::length(offset);
        if (distance <= 0.0f || distance >= smoothingRadius)
        {
            return glm::vec3(0.0f);
        }
        const f32 diff = smoothingRadius - distance;
        return offset * (spikyGradScale * diff * diff / distance);
    }

    [[nodiscard]] inline glm::vec3 SpikyGrad(const glm::vec3& offset, f32 smoothingRadius)
    {
        return SpikyGrad(offset, smoothingRadius, SpikyGradScale(smoothingRadius));
    }
} // namespace OloEngine::FluidKernels
