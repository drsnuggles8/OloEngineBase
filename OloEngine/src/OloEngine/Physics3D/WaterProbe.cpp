#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/WaterProbe.h"

#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
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
            // Shore wave deformation (issue #1033). Buoyancy has to follow the
            // shoaled surface or a boat in the surf floats on a deep-water sea
            // the screen is not showing — the wave slows and steepens under it
            // and the hull does not react.
            p.m_ShoreEnabled = wc.m_ShoreWavesEnabled;
            p.m_ShoreBreakerIndex = std::isfinite(wc.m_ShoreBreakerIndex)
                                        ? std::clamp(wc.m_ShoreBreakerIndex, 0.02f, 2.0f)
                                        : WaterShore::kBreakerIndex;
            return p;
        }
    } // namespace

    std::optional<Volume> MakeVolume(const glm::mat4& model, const WaterComponent& wc)
    {
        Volume w;
        {
            const auto frame = WaterSurface::SurfaceFrame::FromModel(model);
            if (!frame)
                return std::nullopt;

            w.m_Frame = *frame;
            w.m_Params = MakeParams(wc, frame->Origin.y);
            w.m_HalfX = WaterSurface::SanitizeTileDimension(wc.m_WorldSizeX) * 0.5f;
            w.m_HalfZ = WaterSurface::SanitizeTileDimension(wc.m_WorldSizeZ) * 0.5f;

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
        }
        return w;
    }

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

            if (const std::optional<Volume> w = MakeVolume(scene->ComputeWorldTransform(e), wc))
                waters.push_back(*w);
        }
        return waters;
    }

    bool IsOverFootprint(const Volume& volume, const glm::vec3& worldPos)
    {
        const glm::vec2 local = volume.m_Frame.LocalXZ({ worldPos.x, worldPos.z });
        return std::abs(local.x) <= volume.m_HalfX && std::abs(local.y) <= volume.m_HalfZ;
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
        const f32 restY = volume.m_Frame.RestHeight(worldXZ);
        if (!std::isfinite(restY))
            return volume.m_Frame.Origin.y;
        // Invert the rendered column, including the horizontal displacement
        // suppressed by a wake. Both wave paths add world-axis displacement
        // to the rest height at the *base* vertex (WaterTessEvalStage.glsl).
        const auto displacement = [&](glm::vec2 base)
        {
            glm::vec3 delta;
            if (volume.m_OceanField)
            {
                const auto sample = volume.m_OceanField->SampleCascades(base);
                delta = { sample.Horizontal.x, sample.Height * volume.m_FFTHeightScale, sample.Horizontal.y };
                if (volume.m_Params.m_ShoreEnabled)
                    delta = WaterSurface::LimitFFTAtShore(delta, WaterShoreDepthSystem::SampleWorld(base), volume.m_Params.m_ShoreBreakerIndex);
            }
            else
            {
                delta = WaterSurface::SampleDisplacement(volume.m_Params, base, rawTime);
            }
            return WaterSurface::ApplyWakeAtBase(delta, base, volume.m_WakeAffectsPhysics ? volume.m_WakeHeightScale : 0.0f,
                                                 volume.m_WakeFlattenStrength);
        };
        glm::vec2 base = worldXZ;
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 delta = displacement(base);
            base = worldXZ - glm::vec2(delta.x, delta.z);
            if (!std::isfinite(base.x) || !std::isfinite(base.y))
                return restY;
        }
        const f32 height = volume.m_Frame.RestHeight(base) + displacement(base).y;
        return std::isfinite(height) ? height : restY;
    }
} // namespace OloEngine::WaterProbe
