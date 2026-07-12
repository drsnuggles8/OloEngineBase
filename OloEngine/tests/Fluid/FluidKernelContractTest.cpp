// OLO_TEST_LAYER: L1
// =============================================================================
// FluidKernelContractTest.cpp
//
// L1 contracts for the SPH smoothing kernels behind the Position-Based Fluids
// solver (issue #630) plus layout pins for the CPU structs that mirror the
// Fluid_*.comp std430 buffers. The kernels are the CPU mirror of
// assets/shaders/include/FluidKernels.glsl — these tests pin the math both
// backends must agree on.
//
// References cited in the expectations:
//  * Müller, Charypar, Gross, "Particle-Based Fluid Simulation for Interactive
//    Applications", SCA 2003 — poly6 (eq. 20) and spiky (eq. 21) kernels.
//  * Macklin & Müller, "Position Based Fluids", ACM TOG 32(4), 2013.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Fluid/FluidKernels.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace OloEngine::Tests
{
    using namespace OloEngine::FluidKernels;

    // -------------------------------------------------------------------------
    // poly6
    // -------------------------------------------------------------------------

    TEST(FluidKernels, Poly6IntegratesToOneOverSupport)
    {
        // A density kernel must integrate to 1 over its support so that
        // sum(m * W) reproduces the rest density for uniformly distributed
        // mass (Müller 2003 §3.5). Radial integral: 4*pi * int_0^h W(r) r^2 dr,
        // evaluated with Simpson's rule.
        for (const f32 h : { 0.1f, 0.2f, 0.5f, 1.0f })
        {
            constexpr i32 kSteps = 4096; // even
            const f32 dr = h / static_cast<f32>(kSteps);
            f64 integral = 0.0;
            for (i32 s = 0; s <= kSteps; ++s)
            {
                const f32 r = static_cast<f32>(s) * dr;
                const f64 weight = (s == 0 || s == kSteps) ? 1.0 : ((s % 2 == 1) ? 4.0 : 2.0);
                integral += weight * static_cast<f64>(Poly6(r, h)) * r * r;
            }
            integral *= (dr / 3.0) * 4.0 * std::numbers::pi;
            EXPECT_NEAR(integral, 1.0, 1.0e-3) << "h = " << h;
        }
    }

    TEST(FluidKernels, Poly6CompactSupportAndPeak)
    {
        const f32 h = 0.2f;
        // W(0) = 315 / (64 * pi * h^9) * h^6 = 315 / (64 * pi * h^3).
        const f32 expectedPeak = 315.0f / (64.0f * std::numbers::pi_v<f32> * h * h * h);
        EXPECT_NEAR(Poly6(0.0f, h), expectedPeak, expectedPeak * 1.0e-5f);

        EXPECT_EQ(Poly6(h, h), 0.0f);
        EXPECT_EQ(Poly6(h * 1.5f, h), 0.0f);
        EXPECT_EQ(Poly6(-0.01f, h), 0.0f); // negative distance guard
    }

    TEST(FluidKernels, Poly6MonotonicallyDecreasing)
    {
        const f32 h = 0.35f;
        f32 previous = Poly6(0.0f, h);
        for (i32 s = 1; s <= 100; ++s)
        {
            const f32 r = h * static_cast<f32>(s) / 100.0f;
            const f32 w = Poly6(r, h);
            EXPECT_LE(w, previous) << "r = " << r;
            previous = w;
        }
    }

    // -------------------------------------------------------------------------
    // spiky gradient
    // -------------------------------------------------------------------------

    TEST(FluidKernels, SpikyGradPointsTowardNeighbourWithKnownMagnitude)
    {
        // grad W_spiky = -45/(pi h^6) (h-r)^2 rHat (Müller 2003 eq. 21): the
        // kernel decreases with distance, so the gradient w.r.t. the particle's
        // own position points TOWARD the neighbour (negative rHat direction).
        const f32 h = 0.2f;
        const glm::vec3 offset(0.1f, 0.0f, 0.0f); // p_i - p_j
        const glm::vec3 grad = SpikyGrad(offset, h);

        EXPECT_LT(grad.x, 0.0f);
        EXPECT_EQ(grad.y, 0.0f);
        EXPECT_EQ(grad.z, 0.0f);

        const f32 expectedMagnitude = 45.0f / (std::numbers::pi_v<f32> * std::pow(h, 6.0f)) *
                                      (h - 0.1f) * (h - 0.1f);
        EXPECT_NEAR(glm::length(grad), expectedMagnitude, expectedMagnitude * 1.0e-4f);
    }

    TEST(FluidKernels, SpikyGradVanishesAtZeroAndBeyondSupport)
    {
        const f32 h = 0.2f;
        EXPECT_EQ(SpikyGrad(glm::vec3(0.0f), h), glm::vec3(0.0f));
        EXPECT_EQ(SpikyGrad(glm::vec3(h, 0.0f, 0.0f), h), glm::vec3(0.0f));
        EXPECT_EQ(SpikyGrad(glm::vec3(0.0f, 2.0f * h, 0.0f), h), glm::vec3(0.0f));
    }

    TEST(FluidKernels, KernelsScaleWithSmoothingRadiusCubed)
    {
        // Dimensional analysis: W has units 1/volume, so W(k*r, k*h) must equal
        // W(r, h) / k^3 — guards against a mistyped exponent in the scales.
        const f32 h = 0.25f;
        const f32 r = 0.1f;
        const f32 k = 2.0f;
        EXPECT_NEAR(Poly6(k * r, k * h), Poly6(r, h) / (k * k * k), Poly6(r, h) * 1.0e-4f);
    }

    // -------------------------------------------------------------------------
    // Parameter derivations (FluidSolverParams)
    // -------------------------------------------------------------------------

    TEST(FluidSolverParams, DerivedQuantities)
    {
        FluidSolverParams params;
        params.ParticleRadius = 0.05f;
        params.SmoothingRadiusScale = 2.0f;
        params.RestDensity = 1000.0f;

        EXPECT_FLOAT_EQ(params.Spacing(), 0.1f);
        EXPECT_FLOAT_EQ(params.SmoothingRadius(), 0.2f);
        // m = rho0 * d^3: each particle owns one spacing-cube of fluid, so a
        // cubic lattice at spacing d reproduces rho0 (within kernel error).
        EXPECT_FLOAT_EQ(params.ParticleMass(), 1.0f);
    }

    TEST(FluidSolverParams, RestLatticeDensityNearRestDensity)
    {
        // The parameterisation contract behind m = rho0*d^3 with h = 2d: summing
        // m*W over a cubic lattice at spacing d yields ~rho0 (the +-5% window
        // is the kernel discretisation error at this h/d ratio).
        FluidSolverParams params;
        params.ParticleRadius = 0.05f;
        params.SmoothingRadiusScale = 2.0f;
        params.RestDensity = 1000.0f;

        const f32 d = params.Spacing();
        const f32 h = params.SmoothingRadius();
        const f32 m = params.ParticleMass();

        f32 density = 0.0f;
        for (i32 x = -3; x <= 3; ++x)
        {
            for (i32 y = -3; y <= 3; ++y)
            {
                for (i32 z = -3; z <= 3; ++z)
                {
                    const glm::vec3 p(static_cast<f32>(x) * d, static_cast<f32>(y) * d, static_cast<f32>(z) * d);
                    density += m * Poly6(glm::length(p), h);
                }
            }
        }
        EXPECT_NEAR(density, params.RestDensity, params.RestDensity * 0.05f);
    }

    // -------------------------------------------------------------------------
    // GPU mirror layout pins (must match Fluid_*.comp std430 declarations —
    // same discipline as PrecipitationEmitter.GPUParticleSizeIs96Bytes)
    // -------------------------------------------------------------------------

    TEST(FluidGpuLayout, StructSizesMatchStd430)
    {
        EXPECT_EQ(sizeof(FluidBodyProxy), 80u);
        EXPECT_EQ(sizeof(GPUFluidCounters), 16u);
        EXPECT_EQ(sizeof(GPUFluidEmitEntry), 32u);
        EXPECT_EQ(sizeof(GPUFluidBodyImpulse), 32u);

        static_assert(std::is_trivially_copyable_v<FluidBodyProxy>);
        static_assert(std::is_trivially_copyable_v<GPUFluidCounters>);
        static_assert(std::is_trivially_copyable_v<GPUFluidEmitEntry>);
        static_assert(std::is_trivially_copyable_v<GPUFluidBodyImpulse>);
    }

    TEST(FluidGpuLayout, FieldOffsets)
    {
        EXPECT_EQ(offsetof(FluidBodyProxy, Position), 0u);
        EXPECT_EQ(offsetof(FluidBodyProxy, Rotation), 16u);
        EXPECT_EQ(offsetof(FluidBodyProxy, HalfExtents), 32u);
        EXPECT_EQ(offsetof(FluidBodyProxy, LinearVelocity), 48u);
        EXPECT_EQ(offsetof(FluidBodyProxy, AngularVelocity), 64u);

        EXPECT_EQ(offsetof(GPUFluidCounters, Count), 0u);
        EXPECT_EQ(offsetof(GPUFluidCounters, EmitCount), 4u);
        EXPECT_EQ(offsetof(GPUFluidCounters, KillCount), 8u);
    }
} // namespace OloEngine::Tests
