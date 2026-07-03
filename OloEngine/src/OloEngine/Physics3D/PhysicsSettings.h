#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{

    enum class PhysicsDebugType
    {
        DebugToFile = 0,
        LiveDebug
    };

    struct PhysicsSettings
    {
        // Simulation settings
        f32 m_FixedTimestep = 1.0f / 60.0f;
        glm::vec3 m_Gravity = { 0.0f, -9.81f, 0.0f };

        // Solver settings
        u32 m_PositionSolverIterations = 2;
        u32 m_VelocitySolverIterations = 10;

        // System limits
        u32 m_MaxBodies = 65536;
        u32 m_MaxBodyPairs = 65536;
        // A contact constraint is generated per touching body pair, so this can never
        // exceed m_MaxBodyPairs — match it, rather than the old 10240 (issue #523: far too
        // small for the advertised 65536 max bodies, letting Jolt's contact-constraint
        // buffer overflow and bodies tunnel through static geometry in dense piles well
        // under that body count). NOTE: raising this requires JoltScene's temp allocator to
        // scale with it (see JoltScene.h's s_BaselineTempAllocatorSize) — Jolt's per-step
        // scratch usage scales with the constraint capacity, and handing Jolt a bigger
        // capacity without a proportionally bigger fixed-size scratch buffer corrupts
        // memory. Do not raise this default without checking that scaling is still wired up.
        u32 m_MaxContactConstraints = 65536;

        // Debug and capture settings (capture is off by default for production)
        bool m_CaptureOnPlay = false;
        PhysicsDebugType m_CaptureMethod = PhysicsDebugType::DebugToFile;

        // Advanced Jolt-specific settings
        f32 m_Baumgarte = 0.2f;
        f32 m_SpeculativeContactDistance = 0.02f;
        f32 m_PenetrationSlop = 0.05f;
        f32 m_LinearCastThreshold = 0.75f;
        f32 m_MinVelocityForRestitution = 1.0f;
        f32 m_TimeBeforeSleep = 0.5f;
        f32 m_PointVelocitySleepThreshold = 0.03f;

        // Boolean physics options
        bool m_DeterministicSimulation = true;
        bool m_ConstraintWarmStart = true;
        bool m_UseBodyPairContactCache = true;
        bool m_UseManifoldReduction = true;
        bool m_UseLargeIslandSplitter = true;
        bool m_AllowSleeping = true;

        // Default values method for easy reset
        [[nodiscard]] static PhysicsSettings GetDefaults()
        {
            return PhysicsSettings{};
        }
    };

} // namespace OloEngine
