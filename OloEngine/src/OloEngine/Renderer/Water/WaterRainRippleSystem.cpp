#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    WaterRainRippleSystem::WaterRainData WaterRainRippleSystem::s_Data;

    void WaterRainRippleSystem::SetSettings(const WaterRain::WaterRainSettings& settings)
    {
        s_Data.m_Settings = settings;
    }

    const WaterRain::WaterRainSettings& WaterRainRippleSystem::GetSettings()
    {
        return s_Data.m_Settings;
    }

    void WaterRainRippleSystem::SetPrecipitation(bool rippleCapable, f32 intensity)
    {
        s_Data.m_RippleCapable = rippleCapable;
        const f32 clamped = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 0.0f;
        // Snap the tail to zero. PrecipitationSystem's smoothed intensity decays
        // exponentially and never reaches zero, so without this the shader's
        // `params.x <= 0` early-out would stop firing after the first shower and
        // "no cost when rain is off" would quietly become "no cost until it has
        // rained once" — see WaterRain::kMinIntensity.
        s_Data.m_Intensity = (clamped >= WaterRain::kMinIntensity) ? clamped : 0.0f;
    }

    f32 WaterRainRippleSystem::GetEffectiveStrength()
    {
        if (!s_Data.m_Settings.m_Enabled || !s_Data.m_RippleCapable)
            return 0.0f;

        const f32 gain = std::isfinite(s_Data.m_Settings.m_Strength)
                             ? std::clamp(s_Data.m_Settings.m_Strength, 0.0f, 4.0f)
                             : 1.0f;
        // The intensity multiplies the SLOPE as well as gating the density, so
        // drizzle is both sparser and shallower than a downpour. Gating density
        // alone would make a single drizzle ring as deep as a storm's.
        return gain * s_Data.m_Intensity;
    }

    glm::vec4 WaterRainRippleSystem::GetShaderParams()
    {
        const f32 strength = GetEffectiveStrength();
        if (!(strength > 0.0f))
            return glm::vec4(0.0f); // x <= 0 IS the disabled state

        return { strength, WaterRain::DensityForIntensity(s_Data.m_Intensity),
                 WaterRain::kCellSizeMetres, 0.0f };
    }

    glm::vec4 WaterRainRippleSystem::GetShaderParams2()
    {
        f32 fadeStart = std::isfinite(s_Data.m_Settings.m_FadeStartMetres)
                            ? std::clamp(s_Data.m_Settings.m_FadeStartMetres, 0.0f, 2000.0f)
                            : 18.0f;
        f32 fadeEnd = std::isfinite(s_Data.m_Settings.m_FadeEndMetres)
                          ? std::clamp(s_Data.m_Settings.m_FadeEndMetres, 0.0f, 4000.0f)
                          : 45.0f;
        // smoothstep(edge0, edge1, x) is undefined for edge0 >= edge1; a scene
        // that authored these inverted would get an implementation-defined fade
        // rather than a clamped one. Same guard as
        // WaterDisturbanceSystem::GetShaderParams2.
        fadeEnd = std::max(fadeEnd, fadeStart + 1.0f);
        return { fadeStart, fadeEnd, 0.0f, 0.0f };
    }

    void WaterRainRippleSystem::Reset()
    {
        s_Data.m_Settings = WaterRain::WaterRainSettings{};
        s_Data.m_RippleCapable = false;
        s_Data.m_Intensity = 0.0f;
    }
} // namespace OloEngine
