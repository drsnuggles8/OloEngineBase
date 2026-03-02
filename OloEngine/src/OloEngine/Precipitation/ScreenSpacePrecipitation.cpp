#include "OloEnginePCH.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Core/FastRandom.h"

#include <cmath>
#include <numbers>

namespace OloEngine
{
    ScreenSpacePrecipitation::ScreenSpacePrecipitationData ScreenSpacePrecipitation::s_Data;

    void ScreenSpacePrecipitation::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("ScreenSpacePrecipitation::Init called when already initialized");
            return;
        }

        s_Data = ScreenSpacePrecipitationData{};
        s_Data.m_Initialized = true;

        OLO_CORE_INFO("ScreenSpacePrecipitation initialized (max {} lens impacts)", MAX_LENS_IMPACTS);
    }

    void ScreenSpacePrecipitation::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data = ScreenSpacePrecipitationData{};
        OLO_CORE_INFO("ScreenSpacePrecipitation shut down");
    }

    bool ScreenSpacePrecipitation::IsInitialized()
    {
        OLO_PROFILE_FUNCTION();

        return s_Data.m_Initialized;
    }

    void ScreenSpacePrecipitation::Update(const PrecipitationSettings& settings,
                                          f32 intensity,
                                          const glm::vec2& windDirScreen,
                                          f32 windSpeed,
                                          f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        s_Data.m_AccumulatedTime += dt;

        // --- Update streak parameters ---
        if (settings.ScreenStreaksEnabled)
        {
            f32 windLen = glm::length(windDirScreen);
            if (windLen > 0.001f)
            {
                s_Data.m_StreakDirection = windDirScreen / windLen;
            }
            else
            {
                // Default: downward streaks
                s_Data.m_StreakDirection = glm::vec2(0.0f, -1.0f);
            }

            // Streak intensity scales with both precipitation intensity and wind speed
            f32 windFactor = std::clamp(windSpeed / 20.0f, 0.0f, 1.0f); // Normalize to ~20 m/s
            s_Data.m_StreakIntensity = settings.ScreenStreakIntensity * intensity * windFactor;
            s_Data.m_StreakLength = settings.ScreenStreakLength * (0.5f + 0.5f * windFactor);
        }
        else
        {
            s_Data.m_StreakIntensity = 0.0f;
            s_Data.m_StreakLength = 0.0f;
        }

        // --- Update lens impacts ---
        auto& rng = s_Data.m_Rng;

        // Age existing impacts and deactivate expired ones
        for (auto& impact : s_Data.m_LensImpacts)
        {
            if (!impact.Active)
            {
                continue;
            }

            f32 age = s_Data.m_AccumulatedTime - impact.BirthTime;
            if (age >= impact.Lifetime)
            {
                impact.Active = false;
            }
        }

        // Spawn new impacts
        if (settings.LensImpactsEnabled && intensity > 0.01f)
        {
            // Rate scales quadratically with intensity for perceptual linearity
            f32 spawnRate = settings.LensImpactRate * intensity * intensity;
            s_Data.m_ImpactSpawnAccumulator += spawnRate * dt;

            while (s_Data.m_ImpactSpawnAccumulator >= 1.0f)
            {
                s_Data.m_ImpactSpawnAccumulator -= 1.0f;

                // Find next slot (ring buffer overwrites oldest)
                LensImpact& impact = s_Data.m_LensImpacts[s_Data.m_NextImpactSlot];
                s_Data.m_NextImpactSlot = (s_Data.m_NextImpactSlot + 1) % MAX_LENS_IMPACTS;

                // Spawn at random screen position, biased slightly toward center
                // (camera lens catches more in the middle)
                f32 u = rng.GetFloat32InRange(0.1f, 0.9f);
                f32 v = rng.GetFloat32InRange(0.1f, 0.9f);

                // Wind bias: impacts tend to come from the windward side
                u += windDirScreen.x * 0.1f;
                v += windDirScreen.y * 0.1f;
                u = std::clamp(u, 0.0f, 1.0f);
                v = std::clamp(v, 0.0f, 1.0f);

                impact.ScreenUV = glm::vec2(u, v);
                impact.BirthTime = s_Data.m_AccumulatedTime;
                impact.Lifetime = settings.LensImpactLifetime * rng.GetFloat32InRange(0.7f, 1.3f);
                impact.Size = settings.LensImpactSize * rng.GetFloat32InRange(0.6f, 1.4f);
                impact.Rotation = rng.GetFloat32InRange(0.0f, 2.0f * std::numbers::pi_v<f32>);
                impact.Active = true;
            }
        }
    }

    std::array<LensImpactGPUData, ScreenSpacePrecipitation::MAX_LENS_IMPACTS>
    ScreenSpacePrecipitation::GetLensImpactGPUData()
    {
        OLO_PROFILE_FUNCTION();

        std::array<LensImpactGPUData, MAX_LENS_IMPACTS> gpuData{};

        for (u32 i = 0; i < MAX_LENS_IMPACTS; ++i)
        {
            const auto& impact = s_Data.m_LensImpacts[i];
            if (impact.Active)
            {
                f32 age = s_Data.m_AccumulatedTime - impact.BirthTime;
                f32 normalizedAge = (impact.Lifetime > 1e-6f)
                                        ? std::clamp(age / impact.Lifetime, 0.0f, 1.0f)
                                        : 1.0f; // Expired if lifetime is zero/negative

                gpuData[i].PositionAndSize = glm::vec4(
                    impact.ScreenUV.x, impact.ScreenUV.y,
                    impact.Size, impact.Rotation);
                gpuData[i].TimeParams = glm::vec4(
                    normalizedAge,
                    1.0f - normalizedAge, // fade factor
                    0.0f,                 // reserved
                    1.0f);                // active flag
            }
            else
            {
                gpuData[i].PositionAndSize = glm::vec4(0.0f);
                gpuData[i].TimeParams = glm::vec4(0.0f); // w = 0 means inactive
            }
        }

        return gpuData;
    }

    glm::vec4 ScreenSpacePrecipitation::GetStreakParams()
    {
        OLO_PROFILE_FUNCTION();

        return glm::vec4(
            s_Data.m_StreakDirection.x,
            s_Data.m_StreakDirection.y,
            s_Data.m_StreakIntensity,
            s_Data.m_StreakLength);
    }

    u32 ScreenSpacePrecipitation::GetActiveLensImpactCount()
    {
        OLO_PROFILE_FUNCTION();

        u32 count = 0;
        for (const auto& impact : s_Data.m_LensImpacts)
        {
            if (impact.Active)
            {
                ++count;
            }
        }
        return count;
    }

    void ScreenSpacePrecipitation::Reset()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            for (auto& impact : s_Data.m_LensImpacts)
            {
                impact.Active = false;
            }
            s_Data.m_NextImpactSlot = 0;
            s_Data.m_AccumulatedTime = 0.0f;
            s_Data.m_ImpactSpawnAccumulator = 0.0f;
            s_Data.m_StreakIntensity = 0.0f;
            s_Data.m_StreakLength = 0.0f;
        }
    }
} // namespace OloEngine
