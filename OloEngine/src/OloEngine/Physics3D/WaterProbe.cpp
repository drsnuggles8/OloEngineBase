#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/WaterProbe.h"

#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
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
        return volume.m_OceanField
                   ? WaterSurface::SampleHeightFFT(*volume.m_OceanField, worldXZ, volume.m_Params.m_PlaneHeight, volume.m_FFTHeightScale)
                   : WaterSurface::SampleHeight(volume.m_Params, worldXZ, rawTime);
    }
} // namespace OloEngine::WaterProbe
