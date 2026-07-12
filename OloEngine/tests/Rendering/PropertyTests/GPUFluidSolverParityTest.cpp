// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUFluidSolverParityTest.cpp
//
// GPU-vs-CPU parity contracts for the Position-Based Fluids compute chain
// (issue #630). CPUFluidSolver is the deterministic, contract-tested ground
// truth (CPUFluidSolverContractTest); these tests drive the REAL Fluid_*.comp
// chain through GPUFluidSolver on hardware and compare aggregate behaviour:
//
//   (a) an identical seeded lattice stays mass-conserving, finite, in-bounds,
//       and statistically co-located (centre of mass / fill height) after 30
//       steps on both backends;
//   (b) a kill box removes exactly the same particle count (the kill decision
//       is a deterministic point-in-AABB test on identical pre-step data);
//   (c) Emit appends exactly the same particle count;
//   (d) a submerged static sphere proxy receives an upward (buoyant) reaction
//       impulse from both backends.
//
// Setup mirrors GPUOcclusionCullGPUTest: RenderPropertyTest raw fixture (no
// Renderer::Init — GPUFluidSolver only needs ComputeShader / StorageBuffer /
// UniformBuffer / RenderCommand, and the fixture chdirs into OloEditor/ so
// "assets/shaders/compute/Fluid_*.comp" resolves). SKIPs cleanly headless.
// Every test scopes its solvers (their dtors free the GL buffers) and then
// unbinds SSBO slots 21-32 + UBO 47 + the program so no slot is left
// referencing a deleted object (issue #485 GL-error-state listener).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Fluid/CPUFluidSolver.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kDt = 1.0f / 60.0f;

        /// Same values as CPUFluidSolverContractTest::MakeDefaultParams — the
        /// configuration the CPU contracts were tuned against.
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

        /// Cell-centred lattice of nx*ny*nz emit entries at `spacing`, lower
        /// corner at `origin`, zero initial velocity (mirrors the CPU contract
        /// test's FillLattice).
        std::vector<GPUFluidEmitEntry> MakeLattice(const glm::vec3& origin, f32 spacing,
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
            return entries;
        }

        [[nodiscard]] glm::vec3 CentreOfMass(const std::vector<glm::vec4>& positions)
        {
            glm::vec3 sum(0.0f);
            for (const glm::vec4& p : positions)
            {
                sum += glm::vec3(p);
            }
            return positions.empty() ? sum : sum / static_cast<f32>(positions.size());
        }

        [[nodiscard]] glm::vec3 CentreOfMass(std::span<const glm::vec3> positions)
        {
            glm::vec3 sum(0.0f);
            for (const glm::vec3& p : positions)
            {
                sum += p;
            }
            return positions.empty() ? sum : sum / static_cast<f32>(positions.size());
        }

        /// Unbind everything the fluid solver's buffers/programs occupied.
        /// Called AFTER the solver objects went out of scope (their dtors free
        /// the GL buffers), so no slot keeps referencing a deleted object into
        /// the next test's shared GL context (issue #485).
        void UnbindFluidGLState()
        {
            for (u32 slot = ShaderBindingLayout::SSBO_FLUID_POSITIONS;
                 slot <= ShaderBindingLayout::SSBO_FLUID_VELOCITIES_ALT; ++slot)
            {
                ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, 0);
            }
            ::glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_FLUID, 0);
            ::glUseProgram(0);
        }
    } // namespace

    // -------------------------------------------------------------------------
    // (a) Settling-dam parity: identical 6x6x6 lattice, 30 steps at 1/60 on
    // both backends. The formulas are identical; only float summation order
    // differs (the GPU's grid linked lists are built by nondeterministic
    // atomic prepend, so neighbour sums permute). The contracts are therefore
    // aggregate:
    //   * count exact (no kills/emits — mass conservation is order-independent);
    //   * finite + inside the domain (the CPU contract's own invariant);
    //   * |COM_gpu - COM_cpu| < 1.5 * spacing — per-step reordering noise is
    //     ~1e-5 m and cannot accumulate past a fraction of a spacing in 30
    //     steps of a settling (energy-losing) dam, while a wrong formula
    //     (kernel scale, missing m/rho0, wrong lambda denominator) collapses
    //     or explodes the block and moves the COM by several spacings;
    //   * |meanY_gpu - meanY_cpu| < 1 * spacing — pins the settled fill height,
    //     the most sensitive scalar to a density/lambda error.
    // -------------------------------------------------------------------------
    TEST(GPUFluidSolverParityTest, LatticeParityWithCpuAfterThirtySteps)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const FluidSolverParams params = MakeDefaultParams();
        const auto entries = MakeLattice({ -0.3f, 0.0f, -0.3f }, params.Spacing(), 6, 6, 6);
        ASSERT_EQ(entries.size(), 216u);

        CPUFluidSolver cpu(4096);
        cpu.Emit(entries);
        for (i32 step = 0; step < 30; ++step)
        {
            cpu.Step(params, kDt);
        }
        ASSERT_EQ(cpu.GetCount(), 216u);

        bool gpuValid = false;
        std::vector<glm::vec4> gpuPositions;
        std::vector<glm::vec4> gpuVelocities;
        u32 gpuCount = 0;
        {
            GPUFluidSolver gpu(4096);
            gpuValid = gpu.IsValid();
            if (gpuValid)
            {
                gpu.SeedParticles(entries);
                for (i32 step = 0; step < 30; ++step)
                {
                    gpu.Step(params, kDt, {}, {});
                }
                gpu.ReadbackParticles(gpuPositions, gpuVelocities, gpuCount);
            }
        }
        UnbindFluidGLState();
        ASSERT_TRUE(gpuValid) << "GPUFluidSolver failed to initialise (Fluid_*.comp compile error?)";

        // Mass conservation: nothing was killed or emitted.
        ASSERT_EQ(gpuCount, cpu.GetCount());
        ASSERT_EQ(gpuPositions.size(), static_cast<sizet>(gpuCount));

        // Finite + in-bounds, same epsilon as the CPU contract test.
        for (const glm::vec4& p : gpuPositions)
        {
            ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
            EXPECT_GE(p.x, params.BoundsMin.x - 1.0e-4f);
            EXPECT_LE(p.x, params.BoundsMax.x + 1.0e-4f);
            EXPECT_GE(p.y, params.BoundsMin.y - 1.0e-4f);
            EXPECT_LE(p.y, params.BoundsMax.y + 1.0e-4f);
            EXPECT_GE(p.z, params.BoundsMin.z - 1.0e-4f);
            EXPECT_LE(p.z, params.BoundsMax.z + 1.0e-4f);
        }
        for (const glm::vec4& v : gpuVelocities)
        {
            ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
        }

        const glm::vec3 cpuCom = CentreOfMass(cpu.GetPositions());
        const glm::vec3 gpuCom = CentreOfMass(gpuPositions);
        const f32 comDistance = glm::length(gpuCom - cpuCom);
        std::cout << "[ DIAG ] COM cpu (" << cpuCom.x << ", " << cpuCom.y << ", " << cpuCom.z
                  << ") gpu (" << gpuCom.x << ", " << gpuCom.y << ", " << gpuCom.z
                  << ") distance " << comDistance << " (spacing " << params.Spacing() << ")\n";
        EXPECT_LT(comDistance, 1.5f * params.Spacing())
            << "GPU centre of mass diverged from the CPU reference — formula drift?";

        EXPECT_LT(std::abs(gpuCom.y - cpuCom.y), params.Spacing())
            << "GPU settled fill height diverged from the CPU reference — density/lambda drift?";
    }

    // -------------------------------------------------------------------------
    // (b) Kill-box parity: the kill decision is a deterministic inclusive
    // point-in-AABB test evaluated on the IDENTICAL pre-step particle
    // positions (both backends kill before integration on step 1), so the
    // removed count must match exactly (tolerance 0) even though the
    // surviving order differs (GPU compaction is an atomic append).
    // -------------------------------------------------------------------------
    TEST(GPUFluidSolverParityTest, KillBoxRemovesSameCountAsCpu)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const FluidSolverParams params = MakeDefaultParams();
        const auto entries = MakeLattice({ -0.3f, 0.0f, -0.3f }, params.Spacing(), 6, 4, 6);
        const u32 before = static_cast<u32>(entries.size());

        // Kill everything below y = 0.2 (the lower two lattice layers).
        const FluidKillBox box{ { -1.0f, -1.0f, -1.0f }, { 1.0f, 0.2f, 1.0f } };

        CPUFluidSolver cpu(4096);
        cpu.Emit(entries);
        cpu.Step(params, kDt, {}, {}, std::span(&box, 1));
        const u32 cpuAfter = cpu.GetCount();
        ASSERT_LT(cpuAfter, before);
        ASSERT_GT(cpuAfter, 0u);

        bool gpuValid = false;
        u32 gpuAfter = 0;
        std::vector<glm::vec4> gpuPositions;
        std::vector<glm::vec4> gpuVelocities;
        {
            GPUFluidSolver gpu(4096);
            gpuValid = gpu.IsValid();
            if (gpuValid)
            {
                gpu.SeedParticles(entries);
                gpu.Step(params, kDt, {}, std::span(&box, 1));
                gpu.ReadbackParticles(gpuPositions, gpuVelocities, gpuAfter);
            }
        }
        UnbindFluidGLState();
        ASSERT_TRUE(gpuValid) << "GPUFluidSolver failed to initialise";

        EXPECT_EQ(gpuAfter, cpuAfter)
            << "kill-box removal count must match the CPU exactly (deterministic AABB test)";

        // Survivors started above the kill plane; one step of gravity moves
        // them ~g*dt^2, far less than a spacing (CPU contract's own bound).
        for (const glm::vec4& p : gpuPositions)
        {
            EXPECT_GT(p.y, 0.2f - params.Spacing());
        }
    }

    // -------------------------------------------------------------------------
    // (c) Emit parity: staging + the Fluid_Emit atomic slot claim must append
    // exactly as many particles as the CPU's serial append (tolerance 0 —
    // counts are integers and no capacity limit is hit here).
    // -------------------------------------------------------------------------
    TEST(GPUFluidSolverParityTest, EmitAppendsSameCountAsCpu)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const FluidSolverParams params = MakeDefaultParams();
        const auto lattice = MakeLattice({ -0.2f, 0.0f, -0.2f }, params.Spacing(), 4, 4, 4);
        // Second, smaller block dropped from above the first one.
        const auto extra = MakeLattice({ -0.15f, 0.8f, -0.15f }, params.Spacing(), 3, 4, 3);
        const u32 expected = static_cast<u32>(lattice.size() + extra.size());

        CPUFluidSolver cpu(4096);
        cpu.Emit(lattice);
        cpu.Emit(extra);
        cpu.Step(params, kDt);
        ASSERT_EQ(cpu.GetCount(), expected);

        bool gpuValid = false;
        u32 gpuCount = 0;
        std::vector<glm::vec4> gpuPositions;
        std::vector<glm::vec4> gpuVelocities;
        {
            GPUFluidSolver gpu(4096);
            gpuValid = gpu.IsValid();
            if (gpuValid)
            {
                gpu.SeedParticles(lattice);
                gpu.Emit(extra); // staged; applied by the next Step
                gpu.Step(params, kDt, {}, {});
                gpu.ReadbackParticles(gpuPositions, gpuVelocities, gpuCount);
            }
        }
        UnbindFluidGLState();
        ASSERT_TRUE(gpuValid) << "GPUFluidSolver failed to initialise";

        EXPECT_EQ(gpuCount, expected)
            << "GPU emit must append exactly the staged entry count";
        for (const glm::vec4& p : gpuPositions)
        {
            ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        }
    }

    // -------------------------------------------------------------------------
    // (d) Coupling parity: a sphere pushed DOWN through a settled pool's
    // surface must receive an upward (opposing) reaction impulse from BOTH
    // backends. v1 coupling is contact-based, so the moving-intruder form is
    // the robust-signed contract (a static submerged body mostly feels the
    // particles resting on it — see the CPU contract test's rationale).
    // Direction is the contract — magnitudes differ with summation order and
    // the GPU's i32 fixed-point accumulation (1/4096 quantisation), so we
    // accumulate over 30 steps and only pin the sign plus finiteness. A sign
    // error here means the SDF normal, the correction direction, or the
    // J = -correction*(m/dt)*k formula flipped — exactly the bugs this guards.
    // -------------------------------------------------------------------------
    TEST(GPUFluidSolverParityTest, DescendingSphereProxyGetsOpposingImpulseOnBothBackends)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const FluidSolverParams params = MakeDefaultParams();
        // Pool: full-domain footprint, ~0.8 m deep (CPU contract test setup).
        const auto pool = MakeLattice({ -0.5f, 0.0f, -0.5f }, params.Spacing(), 10, 8, 10);

        constexpr f32 kSphereRadius = 0.12f;
        constexpr f32 kDescentSpeed = 1.0f;
        const auto makeProxy = [](f32 sphereY)
        {
            FluidBodyProxy proxy{};
            proxy.Position = glm::vec4(0.0f, sphereY, 0.0f, static_cast<f32>(FluidBodyProxyShape::Sphere));
            proxy.Rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            proxy.HalfExtents = glm::vec4(kSphereRadius, 0.0f, 0.0f, 0.0f);
            proxy.LinearVelocity = glm::vec4(0.0f, -kDescentSpeed, 0.0f, 0.0f);
            proxy.AngularVelocity = glm::vec4(0.0f);
            return proxy;
        };

        // --- CPU reference: settle, then descend + accumulate feedback. ------
        CPUFluidSolver cpu(8192);
        cpu.Emit(pool);
        for (i32 step = 0; step < 45; ++step)
        {
            cpu.Step(params, kDt);
        }
        glm::vec3 cpuTotalImpulse(0.0f);
        {
            f32 sphereY = 0.95f;
            FluidBodyFeedback feedback{};
            for (i32 step = 0; step < 30; ++step)
            {
                sphereY -= kDescentSpeed * kDt;
                const FluidBodyProxy proxy = makeProxy(sphereY);
                cpu.Step(params, kDt, std::span(&proxy, 1), std::span(&feedback, 1));
                cpuTotalImpulse += feedback.Impulse;
            }
        }

        // --- GPU: same schedule; harvest after every step (the impulse SSBO
        // holds the just-completed step's accumulation until the next Step
        // clears it). ----------------------------------------------------------
        bool gpuValid = false;
        glm::vec3 gpuTotalImpulse(0.0f);
        bool gpuAllFinite = true;
        {
            GPUFluidSolver gpu(8192);
            gpuValid = gpu.IsValid();
            if (gpuValid)
            {
                gpu.SeedParticles(pool);
                for (i32 step = 0; step < 45; ++step)
                {
                    gpu.Step(params, kDt, {}, {});
                }
                f32 sphereY = 0.95f;
                FluidBodyFeedback feedback{};
                for (i32 step = 0; step < 30; ++step)
                {
                    sphereY -= kDescentSpeed * kDt;
                    const FluidBodyProxy proxy = makeProxy(sphereY);
                    gpu.Step(params, kDt, std::span(&proxy, 1), {});
                    gpu.HarvestFeedback(std::span(&feedback, 1));
                    gpuAllFinite = gpuAllFinite &&
                                   std::isfinite(feedback.Impulse.x) &&
                                   std::isfinite(feedback.Impulse.y) &&
                                   std::isfinite(feedback.Impulse.z);
                    gpuTotalImpulse += feedback.Impulse;
                }
            }
        }
        UnbindFluidGLState();
        ASSERT_TRUE(gpuValid) << "GPUFluidSolver failed to initialise";
        ASSERT_TRUE(gpuAllFinite) << "GPU feedback impulse went non-finite";

        std::cout << "[ DIAG ] 30-step impulse.y — cpu " << cpuTotalImpulse.y
                  << " gpu " << gpuTotalImpulse.y << " (kg*m/s)\n";

        EXPECT_GT(cpuTotalImpulse.y, 0.0f) << "CPU reaction impulse must oppose the descending sphere";
        EXPECT_GT(gpuTotalImpulse.y, 0.0f) << "GPU reaction impulse must oppose the descending sphere";
    }
} // namespace OloEngine::Tests
