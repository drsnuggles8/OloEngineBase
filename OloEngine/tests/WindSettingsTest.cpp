#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Wind/WindSystem.h"
#include "OloEngine/Particle/ParticlePresets.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Particle/EmissionShape.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// WindSettings Defaults
// =============================================================================

TEST(WindSettings, DefaultsAreReasonable)
{
    WindSettings wind;

    EXPECT_FALSE(wind.Enabled);

    // Direction should be normalized (or at least non-zero)
    EXPECT_GT(glm::length(wind.Direction), 0.9f);
    EXPECT_LT(glm::length(wind.Direction), 1.1f);

    // Speed
    EXPECT_GT(wind.Speed, 0.0f);

    // Gust
    EXPECT_GE(wind.GustStrength, 0.0f);
    EXPECT_LE(wind.GustStrength, 1.0f);
    EXPECT_GT(wind.GustFrequency, 0.0f);

    // Turbulence
    EXPECT_GE(wind.TurbulenceIntensity, 0.0f);
    EXPECT_GT(wind.TurbulenceScale, 0.0f);

    // Grid
    EXPECT_GT(wind.GridWorldSize, 0.0f);
    EXPECT_GE(wind.GridResolution, 32u);
    EXPECT_LE(wind.GridResolution, 256u);
}

// =============================================================================
// WindUBOData Layout (std140 alignment)
// =============================================================================

TEST(WindUBOData, SizeIs64Bytes)
{
    // 4 vec4s = 4 * 16 = 64 bytes
    EXPECT_EQ(WindUBOData::GetSize(), 64u);
    EXPECT_EQ(sizeof(WindUBOData), 64u);
}

TEST(WindUBOData, FieldOffsets_Std140Compatible)
{
    // Each vec4 is 16 bytes, std140 aligned
    // Row 0: DirectionAndSpeed   (Direction.xyz, Speed)
    // Row 1: GustAndTurbulence   (GustStrength, GustFrequency, TurbulenceIntensity, TurbulenceScale)
    // Row 2: GridMinAndSize      (GridMin.xyz, GridWorldSize)
    // Row 3: TimeAndFlags        (Time, Enabled, GridResolution, pad)

    EXPECT_EQ(offsetof(WindUBOData, DirectionAndSpeed), 0u);
    EXPECT_EQ(offsetof(WindUBOData, GustAndTurbulence), 16u);
    EXPECT_EQ(offsetof(WindUBOData, GridMinAndSize), 32u);
    EXPECT_EQ(offsetof(WindUBOData, TimeAndFlags), 48u);
}

TEST(WindUBOData, DefaultsMatchSettings)
{
    WindSettings settings;
    WindUBOData gpu;

    // Direction + Speed
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.x, settings.Direction.x);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.y, settings.Direction.y);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.z, settings.Direction.z);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.w, settings.Speed);

    // Gust + Turbulence
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.x, settings.GustStrength);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.y, settings.GustFrequency);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.z, settings.TurbulenceIntensity);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.w, settings.TurbulenceScale);

    // Disabled by default
    EXPECT_FLOAT_EQ(gpu.TimeAndFlags.y, 0.0f);
}

// =============================================================================
// UBO Binding Indices
// =============================================================================

TEST(ShaderBindingLayout, WindBindingsExist)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_WIND, 15u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WIND_FIELD, 29u);

    // Must not collide with existing bindings
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_CAMERA);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SNOW);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SSS);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SHADOW);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_USER_0);
}

// =============================================================================
// WindSystem::GetWindAtPoint (CPU analytical evaluation)
// =============================================================================

TEST(WindSystem, GetWindAtPoint_DisabledReturnsZero)
{
    WindSettings settings;
    settings.Enabled = false;
    settings.Speed = 10.0f;

    glm::vec3 wind = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 0.0f);

    EXPECT_FLOAT_EQ(wind.x, 0.0f);
    EXPECT_FLOAT_EQ(wind.y, 0.0f);
    EXPECT_FLOAT_EQ(wind.z, 0.0f);
}

TEST(WindSystem, GetWindAtPoint_BasicDirection)
{
    WindSettings settings;
    settings.Enabled = true;
    settings.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
    settings.Speed = 5.0f;
    settings.GustStrength = 0.0f; // No gusts for predictable output
    settings.TurbulenceIntensity = 0.0f;

    glm::vec3 wind = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 0.0f);

    // With no gusts: wind = direction * speed * (1 + 0) = (5, 0, 0)
    EXPECT_NEAR(wind.x, 5.0f, 0.01f);
    EXPECT_NEAR(wind.y, 0.0f, 0.01f);
    EXPECT_NEAR(wind.z, 0.0f, 0.01f);
}

TEST(WindSystem, GetWindAtPoint_DirectionNormalized)
{
    WindSettings settings;
    settings.Enabled = true;
    settings.Direction = glm::vec3(2.0f, 0.0f, 0.0f); // Unnormalized
    settings.Speed = 5.0f;
    settings.GustStrength = 0.0f;

    glm::vec3 wind = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 0.0f);

    // Direction should be internally normalized → wind = (1,0,0) * 5 = (5,0,0)
    EXPECT_NEAR(wind.x, 5.0f, 0.01f);
    EXPECT_NEAR(wind.y, 0.0f, 0.01f);
    EXPECT_NEAR(wind.z, 0.0f, 0.01f);
}

TEST(WindSystem, GetWindAtPoint_GustAffectsMagnitude)
{
    WindSettings settings;
    settings.Enabled = true;
    settings.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
    settings.Speed = 5.0f;
    settings.GustStrength = 0.5f;
    settings.GustFrequency = 1.0f;

    glm::vec3 windA = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 0.0f);
    glm::vec3 windB = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 0.5f);

    // Gusts modulate magnitude over time; the two samples should differ
    float magA = glm::length(windA);
    float magB = glm::length(windB);

    // Both should be around 5.0 ± 50% (gustStrength = 0.5)
    EXPECT_GT(magA, 2.0f);
    EXPECT_LT(magA, 8.0f);
    EXPECT_GT(magB, 2.0f);
    EXPECT_LT(magB, 8.0f);

    // They shouldn't be identical (different times with non-zero gust)
    EXPECT_NE(magA, magB);
}

TEST(WindSystem, GetWindAtPoint_SpatialVariation)
{
    WindSettings settings;
    settings.Enabled = true;
    settings.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
    settings.Speed = 5.0f;
    settings.GustStrength = 0.3f;
    settings.GustFrequency = 1.0f;

    // Same time, different positions → different gust phase from spatial offset
    glm::vec3 windA = WindSystem::GetWindAtPoint(settings, glm::vec3(0.0f), 1.0f);
    glm::vec3 windB = WindSystem::GetWindAtPoint(settings, glm::vec3(100.0f, 0.0f, 0.0f), 1.0f);

    // They should differ due to spatial offset in the gust function
    EXPECT_NE(glm::length(windA), glm::length(windB));
}

// =============================================================================
// ParticlePresets — Snowfall
// =============================================================================

TEST(ParticlePresets, ApplySnowfall_ConfiguresGPUMode)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_TRUE(sys.UseGPU);
    EXPECT_TRUE(sys.Playing);
    EXPECT_TRUE(sys.Looping);
    EXPECT_EQ(sys.SimulationSpace, ParticleSpace::World);
}

TEST(ParticlePresets, ApplySnowfall_ReasonableParticleCounts)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_GE(sys.GetMaxParticles(), 10000u);
    EXPECT_LE(sys.GetMaxParticles(), 200000u);
    EXPECT_GT(sys.Emitter.RateOverTime, 100.0f);
}

TEST(ParticlePresets, ApplySnowfall_HasDownwardGravity)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_TRUE(sys.GravityModule.Enabled);
    EXPECT_LT(sys.GravityModule.Gravity.y, 0.0f); // Downward
    EXPECT_GT(sys.GravityModule.Gravity.y, -5.0f); // Not freefall (snowflakes float)
}

TEST(ParticlePresets, ApplySnowfall_WindEnabled)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_GT(sys.WindInfluence, 0.0f);
}

TEST(ParticlePresets, ApplySnowfall_GroundCollisionEnabled)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_TRUE(sys.GPUGroundCollision);
    EXPECT_FLOAT_EQ(sys.GPUCollisionBounce, 0.0f); // Snow doesn't bounce
}

TEST(ParticlePresets, ApplySnowfall_EmissionShapeIsBox)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    EXPECT_TRUE(std::holds_alternative<EmitBox>(sys.Emitter.Shape));

    const auto& box = std::get<EmitBox>(sys.Emitter.Shape);
    // Should be a wide, thin slab for overhead coverage
    EXPECT_GT(box.HalfExtents.x, 10.0f);
    EXPECT_LT(box.HalfExtents.y, 5.0f); // Thin in Y
    EXPECT_GT(box.HalfExtents.z, 10.0f);
}

TEST(ParticlePresets, ApplySnowfall_SmallWhiteParticles)
{
    ParticleSystem sys;
    ParticlePresets::ApplySnowfall(sys);

    // Small snowflakes
    EXPECT_LT(sys.Emitter.InitialSize, 0.2f);
    EXPECT_GT(sys.Emitter.InitialSize, 0.001f);

    // White-ish color
    EXPECT_GT(sys.Emitter.InitialColor.r, 0.8f);
    EXPECT_GT(sys.Emitter.InitialColor.g, 0.8f);
    EXPECT_GT(sys.Emitter.InitialColor.b, 0.8f);
}

// =============================================================================
// ParticlePresets — Blizzard (inherits from Snowfall with overrides)
// =============================================================================

TEST(ParticlePresets, ApplyBlizzard_MoreParticlesThanSnowfall)
{
    ParticleSystem snowSys;
    ParticlePresets::ApplySnowfall(snowSys);

    ParticleSystem blizzSys;
    ParticlePresets::ApplyBlizzard(blizzSys);

    EXPECT_GT(blizzSys.GetMaxParticles(), snowSys.GetMaxParticles());
    EXPECT_GT(blizzSys.Emitter.RateOverTime, snowSys.Emitter.RateOverTime);
}

TEST(ParticlePresets, ApplyBlizzard_StrongerEffects)
{
    ParticleSystem snowSys;
    ParticlePresets::ApplySnowfall(snowSys);

    ParticleSystem blizzSys;
    ParticlePresets::ApplyBlizzard(blizzSys);

    // Blizzard should have more intense wind and noise
    EXPECT_GE(blizzSys.WindInfluence, snowSys.WindInfluence);
    EXPECT_GE(blizzSys.GPUNoiseStrength, snowSys.GPUNoiseStrength);
}

TEST(ParticlePresets, ApplyBlizzard_StillValidConfig)
{
    ParticleSystem sys;
    ParticlePresets::ApplyBlizzard(sys);

    EXPECT_TRUE(sys.UseGPU);
    EXPECT_TRUE(sys.Playing);
    EXPECT_TRUE(sys.Looping);
    EXPECT_TRUE(sys.GPUGroundCollision);
    EXPECT_TRUE(sys.GravityModule.Enabled);
    EXPECT_TRUE(std::holds_alternative<EmitBox>(sys.Emitter.Shape));
}

// =============================================================================
// SnowSettings — WindDriftFactor
// =============================================================================

TEST(SnowSettings, WindDriftFactorDefaultIsZero)
{
    SnowSettings snow;
    EXPECT_FLOAT_EQ(snow.WindDriftFactor, 0.0f);
}

TEST(SnowSettings, WindDriftFactorInUBOFlags)
{
    // WindDriftFactor should be packed into Flags.y of SnowUBOData
    SnowUBOData gpu;
    // Default: Flags = vec4(0) → Flags.y = 0 (no drift)
    EXPECT_FLOAT_EQ(gpu.Flags.y, 0.0f);
}
