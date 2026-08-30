#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/WaterProbe.h"

#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::WaterProbe
{
    namespace
    {
        [[nodiscard("constructed water-surface params must be used")]] WaterSurface::Params MakeParams(const WaterComponent& wc, f32 planeHeight)
        {
            WaterSurface::Params p;
            p.m_WaveDir0 = wc.PackWaveDir0();
            p.m_WaveDir1 = wc.PackWaveDir1();
            p.m_WaveFrequency = wc.m_WaveFrequency;
            p.m_WaveAmplitude = wc.m_WaveAmplitude;
            p.m_WaveSpeed = wc.m_WaveSpeed;
            p.m_PlaneHeight = planeHeight;
            return p;
        }
    } // namespace

    std::vector<Volume> CollectEnabledVolumes(Scene* scene)
    {
        std::vector<Volume> waters;
        if (!scene)
            return waters;

        auto waterView = scene->GetAllEntitiesWith<TransformComponent, WaterComponent>();
        for (auto e : waterView)
        {
            Entity waterEntity{ e, scene };
            const auto& wc = waterEntity.GetComponent<WaterComponent>();
            if (!wc.m_Enabled)
                continue;

            const glm::mat4 model = waterEntity.GetComponent<TransformComponent>().GetTransform();
            // The water grid is a horizontal XZ plane at local y = 0; its world
            // height is the y of the tile origin (same assumption as the
            // underwater-fog footprint test).
            const glm::vec4 originWorld = model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            if (!std::isfinite(originWorld.y))
                continue;

            const f32 safeSizeX = std::isfinite(wc.m_WorldSizeX) ? wc.m_WorldSizeX : 0.1f;
            const f32 safeSizeZ = std::isfinite(wc.m_WorldSizeZ) ? wc.m_WorldSizeZ : 0.1f;

            Volume w;
            w.m_Params = MakeParams(wc, originWorld.y);
            w.m_InvModel = glm::inverse(model);
            w.m_HalfX = std::clamp(safeSizeX, 0.1f, 10000.0f) * 0.5f;
            w.m_HalfZ = std::clamp(safeSizeZ, 0.1f, 10000.0f) * 0.5f;

            // FFT ocean (WATER §5.1): when the entity renders the Tessendorf
            // spectral surface and its CPU proxy has been evaluated, a floating
            // body should track *that* surface, not the Gerstner approximation.
            // The proxy is produced by the renderer's water pass (Scene.cpp);
            // fall back to Gerstner if it hasn't been built yet (e.g. headless
            // physics with no render pass), so non-rendered scenes stay
            // backward-compatible. Clamp the height scale through the shared
            // helper the render path uses so physics and the shader agree
            // (single source of truth, can't drift).
            if (wc.m_UseFFT && wc.m_OceanField && wc.m_OceanField->GetField().IsValid())
            {
                w.m_OceanField = wc.m_OceanField;
                w.m_FFTHeightScale = WaterSurface::ClampFFTHeightScale(wc.m_FFTHeightScale);
            }

            // Boat / actor wake shape (issue #968). Clamped to the SAME bounds
            // the OLO_SERIALIZE annotations, SceneSerializer's sanitize pass and
            // WaterWakeSystem::SetSettings use, so the surface physics samples
            // cannot be shaped differently from the surface that is drawn.
            w.m_WakeAffectsPhysics = wc.m_WakeShapeEnabled && wc.m_WakeShapeAffectsPhysics;
            w.m_WakeHeightScale =
                wc.m_WakeShapeEnabled
                    ? (std::isfinite(wc.m_WakeShapeHeightScale)
                           ? std::clamp(wc.m_WakeShapeHeightScale, 0.0f, 4.0f)
                           : 1.0f)
                    : 0.0f;
            w.m_WakeFlattenStrength = std::isfinite(wc.m_WakeShapeFlattenStrength)
                                          ? std::clamp(wc.m_WakeShapeFlattenStrength, 0.0f, 1.0f)
                                          : 0.9f;

            waters.push_back(w);
        }
        return waters;
    }

    bool IsOverFootprint(const Volume& volume, const glm::vec3& worldPos)
    {
        const glm::vec4 local = volume.m_InvModel * glm::vec4(worldPos, 1.0f);
        if (!std::isfinite(local.x) || !std::isfinite(local.z))
            return false;
        return local.x >= -volume.m_HalfX && local.x <= volume.m_HalfX && local.z >= -volume.m_HalfZ && local.z <= volume.m_HalfZ;
    }

    const Volume* FindVolumeAt(const std::vector<Volume>& volumes, const glm::vec3& worldPos)
    {
        for (const auto& w : volumes)
        {
            if (IsOverFootprint(w, worldPos))
                return &w;
        }
        return nullptr;
    }

    f32 SampleSurfaceY(const Volume& volume, const glm::vec2& worldXZ, f32 rawTime)
    {
        const f32 oceanY =
            volume.m_OceanField
                ? WaterSurface::SampleHeightFFT(*volume.m_OceanField, worldXZ, volume.m_Params.m_PlaneHeight, volume.m_FFTHeightScale)
                : WaterSurface::SampleHeight(volume.m_Params, worldXZ, rawTime);

        // --- Boat / actor wake shape (issue #968) --------------------------
        // The SAME records and the SAME evaluator the water stages run through
        // WaterWakeCommon.glsl, so a hull floats on the surface that is drawn
        // rather than on the one it would have been drawn on without a boat in
        // it. The TILE's own switches gate it, not a global one: visual-only
        // mode, or a wake disabled on this water surface, collapses the whole
        // block to `oceanY`. See Volume's comment for why they live there.
        //
        // `vertexSpacing` is 0 — "no mesh, no band limit". That is not a parity
        // hole: the limit exists because a ridge narrower than the water mesh's
        // vertices aliases on screen, and physics has no vertices. Buoyancy
        // also samples at the hull, where the mesh is at its finest and the
        // factor is 1 anyway. See WaterWake.h section 5.
        if (!volume.m_WakeAffectsPhysics || !(volume.m_WakeHeightScale > 0.0f))
            return oceanY;

        const WaterWake::Sample wake = WaterWake::Evaluate(
            WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), volume.m_WakeHeightScale,
            volume.m_WakeFlattenStrength, worldXZ, 0.0f);

        // Mirrors the shader's order exactly (WaterVertexStage/WaterTessEval):
        // suppress the ocean displacement inside the hull footprint FIRST,
        // relative to the undisplaced plane, then add the wake height. Applying
        // them the other way round would flatten away part of the wake's own
        // bow bump under the hull, which is a difference the parity test would
        // not catch because it only exercises the shared evaluator.
        const f32 planeY = volume.m_Params.m_PlaneHeight;
        const f32 suppressed = planeY + (oceanY - planeY) * (1.0f - wake.m_Flatten);
        const f32 result = suppressed + wake.m_Height;
        return std::isfinite(result) ? result : oceanY; // physics must never see a NaN height
    }
} // namespace OloEngine::WaterProbe
