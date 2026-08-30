#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/WaterSurface.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Scene;

    namespace Ocean
    {
        class OceanFFTField;
    }
} // namespace OloEngine

namespace OloEngine::WaterProbe
{
    // =========================================================================
    // WaterProbe — resolve the scene's enabled WaterComponent tiles once per
    // tick and sample the rendered surface height above an arbitrary world XZ.
    //
    // Extracted from BuoyancySystem (issue #438) so the boat controller can ask
    // "is my propeller in the water, and how deep?" against the SAME surface
    // buoyancy floats the hull on. Duplicating the tile resolution would let the
    // two drift — a boat thrusting against a surface its hull isn't floating on
    // is exactly the kind of bug that only shows up as "the boat sometimes
    // doesn't accelerate".
    //
    // Both consumers still own their own force models: buoyancy owns Archimedes
    // + the righting torque, the boat owns propulsion / rudder / hull drag.
    // =========================================================================

    /// One resolved water tile: the wave parameters plus the flat-plane XZ
    /// footprint test (mirroring Scene::GetWaterCameraFootprintGap).
    struct Volume
    {
        WaterSurface::Params m_Params;
        glm::mat4 m_InvModel{ 1.0f };
        f32 m_HalfX = 0.0f;
        f32 m_HalfZ = 0.0f;
        /// When the tile renders the Tessendorf FFT ocean and its CPU proxy has
        /// been evaluated, sample that surface (the one actually rendered)
        /// instead of the analytic Gerstner field. Null ⇒ Gerstner.
        Ref<Ocean::OceanFFTField> m_OceanField;
        f32 m_FFTHeightScale = 1.0f; ///< artist multiplier (WaterComponent::m_FFTHeightScale, u_FFTParams.z)

        // --- Boat / actor wake shape (issue #968) ---------------------------
        //
        // Carried PER TILE, read straight off the WaterComponent the body is
        // over, rather than from WaterWakeSystem's published settings. Two
        // reasons, and the second is the load-bearing one:
        //
        //   * the render settings are published from
        //     Scene::ProcessScene3DSharedLogic, which is on the RENDER path — a
        //     headless scene tick never runs it, so physics would silently never
        //     see the wake and every functional test of it would pass by
        //     agreeing that nothing happened;
        //   * the tile a body floats on is the one whose settings should govern
        //     it, which is already how m_FFTHeightScale works above.
        //
        // The hull RECORDS still come from WaterWakeSystem: those are produced
        // by BoatWakeSystem on the physics path and are one global set, not a
        // per-tile property.

        /// The visual-only switch. False means this tile's wake shapes what you
        /// see and nothing that floats.
        bool m_WakeAffectsPhysics = false;
        f32 m_WakeHeightScale = 0.0f;     ///< 0 when the wake is disabled on this tile
        f32 m_WakeFlattenStrength = 0.9f; ///< WaterComponent::m_WakeShapeFlattenStrength
    };

    /// Every enabled WaterComponent tile in the scene, resolved to world space.
    /// Typically 0 or 1 entries; an empty result means "no water here".
    [[nodiscard("collected water volumes must be used")]] std::vector<Volume> CollectEnabledVolumes(Scene* scene);

    /// True when \p worldPos sits over this tile's XZ footprint.
    [[nodiscard("footprint test result must be used")]] bool IsOverFootprint(const Volume& volume, const glm::vec3& worldPos);

    /// The first tile whose footprint \p worldPos sits over, or null.
    [[nodiscard("volume lookup result must be used")]] const Volume* FindVolumeAt(const std::vector<Volume>& volumes, const glm::vec3& worldPos);

    /// World-space Y of the water surface above \p worldXZ on this tile — the
    /// FFT proxy when the tile has one, otherwise the analytic Gerstner field.
    /// `rawTime` is the wave clock (Scene::m_SimulationTime on the physics path).
    [[nodiscard("sampled surface height must be used")]] f32 SampleSurfaceY(const Volume& volume, const glm::vec2& worldXZ, f32 rawTime);
} // namespace OloEngine::WaterProbe
