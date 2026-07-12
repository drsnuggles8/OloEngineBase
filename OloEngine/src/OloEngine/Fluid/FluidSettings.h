#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Tuning asset for a Position-Based Fluids domain (`.olofluid`).
     *
     * Referenced by FluidComponent::m_Settings; a handle of 0 means "engine
     * defaults" (this struct's initialisers). Pure value data serialized by
     * FluidSettingsSerializer; every float is finite-validated and clamped to
     * the documented range on load.
     *
     * The solver block feeds FluidSolverParams (see ToSolverParams); the
     * appearance block is consumed by the screen-space fluid rendering passes.
     */
    class FluidSettings : public Asset
    {
      public:
        FluidSettings() = default;

        static AssetType GetStaticType()
        {
            return AssetType::FluidSettings;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // ---- Solver ---------------------------------------------------------
        f32 m_RestDensity = 1000.0f;       ///< kg/m^3 [50, 20000]
        f32 m_ParticleRadius = 0.1f;       ///< metres [0.01, 1]; rest spacing = 2r
        f32 m_SmoothingRadiusScale = 2.0f; ///< h = scale * spacing [1.2, 4]
        u32 m_SolverIterations = 4;        ///< constraint Jacobi iterations [1, 16]
        f32 m_CfmEpsilon = 50.0f;          ///< lambda relaxation [1e-4, 1e6]
        f32 m_SCorrK = 0.01f;              ///< anti-clump strength [0, 1]
        f32 m_SCorrN = 4.0f;               ///< anti-clump exponent [1, 8]
        f32 m_SCorrDeltaQ = 0.2f;          ///< anti-clump reference distance, fraction of h [0, 0.99]
        f32 m_XsphViscosity = 0.1f;        ///< XSPH smoothing factor [0, 1]
        f32 m_VorticityEpsilon = 0.05f;    ///< vorticity-confinement strength [0, 5]
        f32 m_MaxSpeed = 40.0f;            ///< velocity clamp, m/s [1, 1000]
        f32 m_GravityScale = 1.0f;         ///< multiplier on scene gravity [-10, 10]
        f32 m_CouplingStiffness = 1.0f;    ///< reaction-impulse scale onto rigid bodies [0, 10]

        // ---- Appearance (screen-space rendering) ----------------------------
        glm::vec3 m_Tint = { 0.35f, 0.65f, 0.85f };            ///< shallow-water surface tint
        glm::vec3 m_AbsorptionColor = { 0.45f, 0.06f, 0.01f }; ///< Beer-Lambert per-channel absorption
        f32 m_AbsorptionScale = 1.0f;                          ///< absorption strength [0, 100]
        f32 m_FoamVorticityThreshold = 3.0f;                   ///< |omega| above which foam appears [0, 100]

        /// Build validated solver parameters for a domain AABB centred at
        /// `domainCenter`. `gravity` is the scene gravity (scaled here).
        [[nodiscard]] FluidSolverParams ToSolverParams(const glm::vec3& domainCenter,
                                                       const glm::vec3& domainHalfExtents,
                                                       const glm::vec3& gravity) const
        {
            FluidSolverParams params;
            params.BoundsMin = domainCenter - domainHalfExtents;
            params.BoundsMax = domainCenter + domainHalfExtents;
            params.Gravity = gravity * m_GravityScale;
            params.ParticleRadius = m_ParticleRadius;
            params.SmoothingRadiusScale = m_SmoothingRadiusScale;
            params.RestDensity = m_RestDensity;
            params.SolverIterations = m_SolverIterations;
            params.CfmEpsilon = m_CfmEpsilon;
            params.SCorrK = m_SCorrK;
            params.SCorrN = m_SCorrN;
            params.SCorrDeltaQ = m_SCorrDeltaQ;
            params.XsphViscosity = m_XsphViscosity;
            params.VorticityEpsilon = m_VorticityEpsilon;
            params.MaxSpeed = m_MaxSpeed;
            params.CouplingStiffness = m_CouplingStiffness;
            return params;
        }
    };
} // namespace OloEngine
