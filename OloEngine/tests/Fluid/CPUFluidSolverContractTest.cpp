// OLO_TEST_LAYER: unit
// =============================================================================
// CPUFluidSolverContractTest.cpp
//
// Behavioural contracts for the deterministic CPU Position-Based Fluids
// reference solver (issue #630): a resting dam settles toward the rest
// density, incompressibility improves under constraint iterations, mass is
// conserved, the solver is bit-deterministic, kill boxes drain, emitters
// append, and rigid-body proxies exchange momentum in the right direction.
// The GPU compute chain is parity-tested against THIS solver, so these
// contracts transitively pin the GPU path too.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Fluid/CPUFluidSolver.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kDt = 1.0f / 60.0f;

        FluidSolverParams MakeDefaultParams()
        {
            FluidSolverParams params;
            params.BoundsMin = { -0.5f, 0.0f, -0.5f };
            params.BoundsMax = { 0.5f, 1.5f, 0.5f };
            params.ParticleRadius = 0.05f; // d = 0.1, h = 0.2, m = 1 kg
            params.SmoothingRadiusScale = 2.0f;
            params.RestDensity = 1000.0f;
            params.SolverIterations = 3;
            return params;
        }

        /// Fill a lattice of `nx*ny*nz` particles at `spacing`, lower corner at
        /// `origin` (cell-centred), zero initial velocity.
        void FillLattice(CPUFluidSolver& solver, const glm::vec3& origin, f32 spacing,
                         u32 nx, u32 ny, u32 nz)
        {
            std::vector<GPUFluidEmitEntry> entries;
            entries.reserve(static_cast<sizet>(nx) * ny * nz);
            for (u32 x = 0; x < nx; ++x)
            {
                for (u32 y = 0; y < ny; ++y)
                {
                    for (u32 z = 0; z < nz; ++z)
                    {
                        GPUFluidEmitEntry entry{};
                        entry.Position = glm::vec4(
                            origin.x + (static_cast<f32>(x) + 0.5f) * spacing,
                            origin.y + (static_cast<f32>(y) + 0.5f) * spacing,
                            origin.z + (static_cast<f32>(z) + 0.5f) * spacing,
                            0.0f);
                        entry.Velocity = glm::vec4(0.0f);
                        entries.push_back(entry);
                    }
                }
            }
            solver.Emit(entries);
        }

        f32 MeanSpeed(const CPUFluidSolver& solver)
        {
            f32 sum = 0.0f;
            for (const glm::vec3& v : solver.GetVelocities())
            {
                sum += glm::length(v);
            }
            const u32 count = solver.GetCount();
            return count > 0 ? sum / static_cast<f32>(count) : 0.0f;
        }
    } // namespace

    TEST(CPUFluidSolver, DamSettlesIncompressibleAndInBounds)
    {
        FluidSolverParams params = MakeDefaultParams();
        CPUFluidSolver solver(4096);

        // 6x6x6 block resting on the floor (spacing = rest spacing).
        FillLattice(solver, { -0.3f, 0.0f, -0.3f }, params.Spacing(), 6, 6, 6);
        ASSERT_EQ(solver.GetCount(), 216u);

        for (i32 step = 0; step < 90; ++step)
        {
            solver.Step(params, kDt);
        }

        // Mass conserved, everything finite and inside the domain.
        ASSERT_EQ(solver.GetCount(), 216u);
        for (const glm::vec3& p : solver.GetPositions())
        {
            ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
            EXPECT_GE(p.x, params.BoundsMin.x - 1.0e-4f);
            EXPECT_LE(p.x, params.BoundsMax.x + 1.0e-4f);
            EXPECT_GE(p.y, params.BoundsMin.y - 1.0e-4f);
            EXPECT_LE(p.y, params.BoundsMax.y + 1.0e-4f);
            EXPECT_GE(p.z, params.BoundsMin.z - 1.0e-4f);
            EXPECT_LE(p.z, params.BoundsMax.z + 1.0e-4f);
        }

        // Settled: the block started at rest on the floor, so after 1.5 s the
        // residual sloshing should be small.
        EXPECT_LT(MeanSpeed(solver), 0.25f);

        // Incompressibility: peak compression after the final constraint solve
        // stays under 15%. The worst case is the bottom CORNER particles —
        // wall/floor-deficient neighbourhoods carrying the full column load —
        // and ~14% is the converged residual for one substep of 4 under-relaxed
        // (kFluidJacobiRelaxation) Jacobi iterations at the default epsilon;
        // Macklin & Müller's low single-digit errors use more iterations /
        // substeps. Tightening this bound means paying SolverIterations.
        EXPECT_LT(solver.GetLastMaxDensityError(), 0.15f);

        // Average density includes free-surface particles (legitimately
        // under-dense), so pin a broad band around rho0 rather than equality.
        EXPECT_GT(solver.GetLastAverageDensity(), 0.55f * params.RestDensity);
        EXPECT_LT(solver.GetLastAverageDensity(), 1.10f * params.RestDensity);
    }

    TEST(CPUFluidSolver, ConstraintIterationsRelaxCompression)
    {
        // Over-compressed lattice (80% spacing => ~1.95x rest density): a few
        // steps of the constraint solver must reduce peak compression a lot.
        FluidSolverParams params = MakeDefaultParams();
        params.SolverIterations = 4;

        CPUFluidSolver solver(4096);
        FillLattice(solver, { -0.25f, 0.0f, -0.25f }, params.Spacing() * 0.8f, 6, 6, 6);

        solver.Step(params, kDt);
        const f32 initialError = solver.GetLastMaxDensityError();

        for (i32 step = 0; step < 5; ++step)
        {
            solver.Step(params, kDt);
        }
        EXPECT_LT(solver.GetLastMaxDensityError(), initialError * 0.5f)
            << "initial max compression " << initialError;
    }

    TEST(CPUFluidSolver, StepIsBitDeterministic)
    {
        const auto run = [](std::vector<glm::vec3>& outPositions)
        {
            FluidSolverParams params = MakeDefaultParams();
            CPUFluidSolver solver(1024);
            FillLattice(solver, { -0.2f, 0.0f, -0.2f }, params.Spacing(), 4, 5, 4);
            for (i32 step = 0; step < 30; ++step)
            {
                solver.Step(params, kDt);
            }
            outPositions.assign(solver.GetPositions().begin(), solver.GetPositions().end());
        };

        std::vector<glm::vec3> first;
        std::vector<glm::vec3> second;
        run(first);
        run(second);

        ASSERT_EQ(first.size(), second.size());
        EXPECT_EQ(0, std::memcmp(first.data(), second.data(), first.size() * sizeof(glm::vec3)));
    }

    TEST(CPUFluidSolver, EmitRespectsCapacity)
    {
        CPUFluidSolver solver(10);
        std::vector<GPUFluidEmitEntry> entries(25);
        for (sizet i = 0; i < entries.size(); ++i)
        {
            entries[i].Position = glm::vec4(static_cast<f32>(i) * 0.1f, 0.5f, 0.0f, 0.0f);
            entries[i].Velocity = glm::vec4(0.0f);
        }
        solver.Emit(entries);
        EXPECT_EQ(solver.GetCount(), 10u);
    }

    TEST(CPUFluidSolver, KillBoxDrainsParticles)
    {
        FluidSolverParams params = MakeDefaultParams();
        CPUFluidSolver solver(4096);
        FillLattice(solver, { -0.3f, 0.0f, -0.3f }, params.Spacing(), 6, 4, 6);
        const u32 before = solver.GetCount();

        // Kill everything below y = 0.2 (the lower two layers).
        const FluidKillBox box{ { -1.0f, -1.0f, -1.0f }, { 1.0f, 0.2f, 1.0f } };
        solver.Step(params, kDt, {}, {}, std::span(&box, 1));

        EXPECT_LT(solver.GetCount(), before);
        EXPECT_GT(solver.GetCount(), 0u);
        // Survivors were above the kill plane when the step began; gravity only
        // moved them ~g*dt^2 downward, well under one spacing.
        for (const glm::vec3& p : solver.GetPositions())
        {
            EXPECT_GT(p.y, 0.2f - params.Spacing());
        }
    }

    TEST(CPUFluidSolver, BodyProxyExcludesParticles)
    {
        FluidSolverParams params = MakeDefaultParams();
        CPUFluidSolver solver(4096);
        FillLattice(solver, { -0.3f, 0.0f, -0.3f }, params.Spacing(), 6, 6, 6);

        // Static sphere in the middle of the block.
        FluidBodyProxy proxy{};
        proxy.Position = glm::vec4(0.0f, 0.3f, 0.0f, static_cast<f32>(FluidBodyProxyShape::Sphere));
        proxy.Rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        proxy.HalfExtents = glm::vec4(0.15f, 0.0f, 0.0f, 0.0f);
        proxy.LinearVelocity = glm::vec4(0.0f);
        proxy.AngularVelocity = glm::vec4(0.0f);

        for (i32 step = 0; step < 60; ++step)
        {
            solver.Step(params, kDt, std::span(&proxy, 1));
        }

        // No particle centre may remain meaningfully inside the sphere.
        const glm::vec3 center(proxy.Position);
        const f32 minDist = proxy.HalfExtents.x + params.ParticleRadius * 0.5f;
        for (const glm::vec3& p : solver.GetPositions())
        {
            EXPECT_GT(glm::length(p - center), minDist);
        }
    }

    TEST(CPUFluidSolver, DescendingProxyReceivesOpposingImpulse)
    {
        // v1 coupling is CONTACT-based: bodies exchange momentum with fluid on
        // penetration (push-out reaction). A static body in an already-settled
        // pool therefore feels mostly the particles resting on it — hydrostatic
        // pressure does not transmit without contact — so the honest, robust
        // contract is: a body MOVING INTO the fluid receives an impulse
        // opposing the intrusion. (Floating behaviour of dynamic bodies is the
        // Functional test's job: a light box reaches contact equilibrium, a
        // dense box sinks — FluidCouplingTest.LightBoxFloatsDenseBoxSinks.)
        FluidSolverParams params = MakeDefaultParams();
        CPUFluidSolver solver(8192);
        // Pool: full-domain footprint, ~0.8 m deep.
        FillLattice(solver, { -0.5f, 0.0f, -0.5f }, params.Spacing(), 10, 8, 10);

        // Let the pool settle without the body first.
        for (i32 step = 0; step < 45; ++step)
        {
            solver.Step(params, kDt);
        }

        // Push a sphere downward through the pool surface at 1 m/s
        // (kinematic: reposition each step, velocity advertised on the proxy).
        constexpr f32 kSphereRadius = 0.12f;
        constexpr f32 kDescentSpeed = 1.0f;
        FluidBodyProxy proxy{};
        proxy.Rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        proxy.HalfExtents = glm::vec4(kSphereRadius, 0.0f, 0.0f, 0.0f);
        proxy.LinearVelocity = glm::vec4(0.0f, -kDescentSpeed, 0.0f, 0.0f);
        proxy.AngularVelocity = glm::vec4(0.0f);

        glm::vec3 totalImpulse(0.0f);
        f32 sphereY = 0.95f; // just above the ~0.8 m surface
        FluidBodyFeedback feedback{};
        for (i32 step = 0; step < 30; ++step)
        {
            sphereY -= kDescentSpeed * kDt;
            proxy.Position = glm::vec4(0.0f, sphereY, 0.0f, static_cast<f32>(FluidBodyProxyShape::Sphere));
            solver.Step(params, kDt, std::span(&proxy, 1), std::span(&feedback, 1));
            totalImpulse += feedback.Impulse;
            ASSERT_TRUE(std::isfinite(feedback.Impulse.x));
            ASSERT_TRUE(std::isfinite(feedback.Impulse.y));
            ASSERT_TRUE(std::isfinite(feedback.Impulse.z));
        }

        // Direction contract: the intruding sphere displaces fluid downward /
        // outward, so the reaction on the sphere must point UP. Magnitude
        // sanity: it plausibly relates to the momentum of the displaced fluid
        // (rho0 * V_sphere * descent speed ~ 7.2 kg.m/s); pin two orders of
        // magnitude around it to catch unit errors without tuning flake.
        const f32 displacedMomentum = params.RestDensity *
                                      (4.0f / 3.0f) * std::numbers::pi_v<f32> *
                                      kSphereRadius * kSphereRadius * kSphereRadius *
                                      kDescentSpeed;
        EXPECT_GT(totalImpulse.y, 0.05f * displacedMomentum);
        EXPECT_LT(totalImpulse.y, 100.0f * displacedMomentum);
    }

    TEST(CPUFluidSolver, LightDynamicBoxProxyFloats)
    {
        // Closed-loop floating: integrate a light dynamic box against the
        // solver's contact feedback (the same loop FluidSystem runs through
        // Jolt). A 0.4 m box massing 15 kg displaces ~64 kg of water — the
        // contact reaction must hold it in the upper half of the pool instead
        // of letting it reach the floor. This isolates "can the coupling carry
        // a floater" from the Scene/Jolt plumbing.
        FluidSolverParams params;
        params.BoundsMin = { -1.0f, 0.0f, -1.0f };
        params.BoundsMax = { 1.0f, 1.5f, 1.0f };
        params.ParticleRadius = 0.1f; // engine-default spacing 0.2 (the Functional test's resolution)
        params.RestDensity = 1000.0f;

        CPUFluidSolver solver(8192);
        // ~320 particles: 9x4x9 lattice at spacing 0.2 fills the lower ~0.9 m.
        FillLattice(solver, { -0.9f, 0.0f, -0.9f }, params.Spacing(), 9, 4, 9);
        for (i32 step = 0; step < 45; ++step)
        {
            solver.Step(params, kDt);
        }

        constexpr f32 kHalf = 0.2f;
        constexpr f32 kMass = 15.0f;
        f32 boxY = 0.7f;
        f32 boxVelY = 0.0f;

        FluidBodyProxy proxy{};
        proxy.Rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        proxy.HalfExtents = glm::vec4(kHalf, kHalf, kHalf, 0.0f);
        proxy.AngularVelocity = glm::vec4(0.0f);

        f32 minY = boxY;
        FluidBodyFeedback feedback{};
        for (i32 step = 0; step < 180; ++step) // 3 s
        {
            boxVelY += -9.81f * kDt;
            proxy.Position = glm::vec4(0.0f, boxY, 0.0f, static_cast<f32>(FluidBodyProxyShape::Box));
            proxy.LinearVelocity = glm::vec4(0.0f, boxVelY, 0.0f, 0.0f);
            solver.Step(params, kDt, std::span(&proxy, 1), std::span(&feedback, 1));
            boxVelY += feedback.Impulse.y / kMass;
            boxY += boxVelY * kDt;
            minY = std::min(minY, boxY);
            ASSERT_TRUE(std::isfinite(boxY));
        }

        std::cout << "[ DIAG ] closed-loop float: final boxY " << boxY
                  << " minY " << minY << " velY " << boxVelY << std::endl;

        // Floor rest would be boxY == kHalf (0.2). Floating means the box never
        // gets near the floor AND is never catapulted out of the pool (the
        // over-impulse failure mode this contract originally caught: summing
        // contact impulses across solver iterations launched the box to y=30).
        EXPECT_GT(minY, 0.3f) << "box reached the floor region - coupling cannot carry a floater";
        EXPECT_GT(boxY, 0.4f);
        EXPECT_LT(boxY, 1.6f) << "box was launched out of the pool - coupling over-impulses";
        EXPECT_LT(std::abs(boxVelY), 3.0f) << "box still moving violently - no floating equilibrium";
    }

    TEST(CPUFluidSolver, VelocityClampHolds)
    {
        FluidSolverParams params = MakeDefaultParams();
        params.Gravity = { 0.0f, -5000.0f, 0.0f };
        params.MaxSpeed = 10.0f;

        CPUFluidSolver solver(512);
        FillLattice(solver, { -0.2f, 0.6f, -0.2f }, params.Spacing(), 4, 4, 4);

        for (i32 step = 0; step < 10; ++step)
        {
            solver.Step(params, kDt);
        }
        for (const glm::vec3& v : solver.GetVelocities())
        {
            EXPECT_LE(glm::length(v), params.MaxSpeed * 1.01f);
        }
    }
} // namespace OloEngine::Tests
