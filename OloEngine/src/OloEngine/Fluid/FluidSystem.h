#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // FluidSystem — per-tick driver for every FluidComponent domain
    // (Position-Based Fluids, issue #630).
    //
    // Bridges Scene (components), the Fluid solvers (CPU reference / GPU
    // compute) and Physics3D (two-way Jolt coupling), so it lives behind a
    // small static entry point mirroring BuoyancySystem / NavigationSystem.
    //
    // Call sites — the same pre-step seam as BuoyancySystem, and they must not
    // drift: the "Fluid" gameplay-scheduler node (runtime path, ordered
    // Before("PhysicsKick") with a kBodyForces edge) and Scene::StepPhysics
    // (editor Simulate path), both BEFORE the Jolt step so coupling impulses
    // are integrated this frame. Runs on the game thread only: the GPU solver
    // issues GL compute dispatches.
    //
    // Solver-backend selection per FluidComponent (m_SolverMode):
    //   Auto — GPU when Renderer3D::IsInitialized(), else CPU (headless /
    //          OloServer / Functional tests).
    //   GPU/CPU — forced. The OLO_FLUID_SEQUENTIAL=1 environment variable
    //   overrides everything to the deterministic CPU reference solver.
    // =========================================================================
    class FluidSystem
    {
      public:
        /// Advance every enabled fluid domain by `deltaTime` (the fixed physics
        /// step at both call sites) and queue coupling impulses on overlapped
        /// dynamic bodies. Safe to call with no physics scene (fluid still
        /// simulates against its domain walls).
        static void OnUpdate(Scene* scene, f32 deltaTime);

        /// True when OLO_FLUID_SEQUENTIAL=1 forces the CPU reference solver
        /// process-wide (read once, cached).
        [[nodiscard]] static bool IsSequentialForced();
    };
} // namespace OloEngine
