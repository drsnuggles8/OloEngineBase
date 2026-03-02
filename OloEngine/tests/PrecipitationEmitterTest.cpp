#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Precipitation/PrecipitationEmitter.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Particle/GPUParticleData.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// =============================================================================
// Emission Rate Calculation
// =============================================================================

TEST(PrecipitationEmitter, EmissionRateAtZeroIntensityIsZero)
{
    f32 rate = PrecipitationEmitter::CalculateEmissionRate(4000, 0.0f);
    EXPECT_FLOAT_EQ(rate, 0.0f);
}

TEST(PrecipitationEmitter, EmissionRateAtFullIntensityEqualsBaseRate)
{
    f32 rate = PrecipitationEmitter::CalculateEmissionRate(4000, 1.0f);
    EXPECT_FLOAT_EQ(rate, 4000.0f);
}

TEST(PrecipitationEmitter, EmissionRateQuadraticAtHalf)
{
    f32 rate = PrecipitationEmitter::CalculateEmissionRate(4000, 0.5f);
    EXPECT_FLOAT_EQ(rate, 1000.0f); // 4000 * 0.25
}

TEST(PrecipitationEmitter, EmissionRateNeverNegative)
{
    for (f32 i = -0.5f; i <= 1.5f; i += 0.1f)
    {
        f32 rate = PrecipitationEmitter::CalculateEmissionRate(4000, i);
        EXPECT_GE(rate, 0.0f);
    }
}

// =============================================================================
// Spawn Volume Computation
// =============================================================================

TEST(PrecipitationEmitter, SpawnVolumeContainsCamera)
{
    glm::vec3 cameraPos(10.0f, 20.0f, 30.0f);
    PrecipitationSettings settings;
    glm::vec3 windDir(1.0f, 0.0f, 0.0f);
    f32 windSpeed = 5.0f;

    auto [aabbMin, aabbMax] = PrecipitationEmitter::ComputeSpawnVolume(
        cameraPos, settings.NearFieldExtent, windDir, windSpeed);

    // Camera should be within the spawn volume (approximately)
    EXPECT_GE(cameraPos.x, aabbMin.x - 5.0f); // some margin for wind bias
    EXPECT_LE(cameraPos.x, aabbMax.x + 5.0f);
    EXPECT_GE(cameraPos.z, aabbMin.z - 5.0f);
    EXPECT_LE(cameraPos.z, aabbMax.z + 5.0f);
}

TEST(PrecipitationEmitter, FarFieldVolumeIsLargerThanNearField)
{
    glm::vec3 cameraPos(0.0f);
    PrecipitationSettings settings;
    glm::vec3 windDir(0.0f, 0.0f, 1.0f);

    auto [nearMin, nearMax] = PrecipitationEmitter::ComputeSpawnVolume(
        cameraPos, settings.NearFieldExtent, windDir, 0.0f);

    auto [farMin, farMax] = PrecipitationEmitter::ComputeSpawnVolume(
        cameraPos, settings.FarFieldExtent, windDir, 0.0f);

    glm::vec3 nearSize = nearMax - nearMin;
    glm::vec3 farSize = farMax - farMin;
    EXPECT_GT(farSize.x, nearSize.x);
    EXPECT_GT(farSize.z, nearSize.z);
}

TEST(PrecipitationEmitter, WindBiasShiftsVolumeUpwind)
{
    glm::vec3 cameraPos(0.0f);
    PrecipitationSettings settings;

    // Wind blowing in +X direction
    glm::vec3 windDir(1.0f, 0.0f, 0.0f);
    f32 windSpeed = 15.0f;

    auto [aabbMin, aabbMax] = PrecipitationEmitter::ComputeSpawnVolume(
        cameraPos, settings.NearFieldExtent, windDir, windSpeed);

    // Volume center should be shifted in -X (upwind) relative to camera
    glm::vec3 center = (aabbMin + aabbMax) * 0.5f;
    EXPECT_LT(center.x, cameraPos.x);
}

// =============================================================================
// Particle Generation Tests
// =============================================================================

TEST(PrecipitationEmitter, GeneratesZeroParticlesAtZeroIntensity)
{
    glm::vec3 cameraPos(0.0f);
    PrecipitationSettings settings;
    settings.Enabled = true;

    auto particles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, settings, 0.0f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 0.016f);

    EXPECT_TRUE(particles.empty());
}

TEST(PrecipitationEmitter, GeneratesParticlesAtFullIntensity)
{
    glm::vec3 cameraPos(0.0f);
    PrecipitationSettings settings;
    settings.Enabled = true;
    settings.BaseEmissionRate = 4000;

    auto particles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, settings, 1.0f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 5.0f, 0.016f);

    // At 4000/s * 40% near-field * 0.016s ~= 25.6 particles, at least a few
    EXPECT_GT(particles.size(), 0u);
}

TEST(PrecipitationEmitter, ParticlesHaveFiniteValues)
{
    glm::vec3 cameraPos(100.0f, 50.0f, 200.0f);
    PrecipitationSettings settings;
    settings.Enabled = true;

    auto particles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, settings, 0.8f,
        PrecipitationLayer::NearField, glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, 0.016f);

    for (const auto& p : particles)
    {
        EXPECT_TRUE(std::isfinite(p.PositionLifetime.x));
        EXPECT_TRUE(std::isfinite(p.PositionLifetime.y));
        EXPECT_TRUE(std::isfinite(p.PositionLifetime.z));
        EXPECT_TRUE(std::isfinite(p.InitialVelocitySize.w)); // size
        EXPECT_GT(p.InitialVelocitySize.w, 0.0f);            // size positive
        EXPECT_TRUE(std::isfinite(p.VelocityMaxLifetime.w)); // max lifetime
        EXPECT_GT(p.VelocityMaxLifetime.w, 0.0f);            // max lifetime positive
    }
}

TEST(PrecipitationEmitter, ParticlesHaveDownwardVelocity)
{
    glm::vec3 cameraPos(0.0f);
    PrecipitationSettings settings;
    settings.Enabled = true;

    auto particles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, settings, 0.8f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 5.0f, 0.016f);

    for (const auto& p : particles)
    {
        // Snow should fall downward (negative Y velocity)
        EXPECT_LT(p.VelocityMaxLifetime.y, 0.0f);
    }
}

// =============================================================================
// Camera Motion Compensation
// =============================================================================

TEST(PrecipitationEmitter, ParticlesSpawnDespiteCameraMotion)
{
    glm::vec3 prevCameraPos(0.0f, 10.0f, 0.0f);
    glm::vec3 currCameraPos(5.0f, 10.0f, 0.0f); // Moving 5m/frame in X
    PrecipitationSettings settings;
    settings.Enabled = true;
    settings.BaseEmissionRate = 4000;

    auto particles = PrecipitationEmitter::GenerateParticles(
        currCameraPos, prevCameraPos, settings, 1.0f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 5.0f, 0.016f);

    // Should still generate particles even with camera motion
    EXPECT_GT(particles.size(), 0u);
}

// =============================================================================
// GPUParticle Size
// =============================================================================

TEST(PrecipitationEmitter, GPUParticleSizeIs96Bytes)
{
    // 6 vec4s * 16 bytes = 96 bytes (std430 layout)
    EXPECT_EQ(sizeof(GPUParticle), 96u);
}

// =============================================================================
// Type-Specific Default Settings
// =============================================================================

TEST(PrecipitationDefaults, SnowDefaultsAreReasonable)
{
    auto s = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Snow);
    EXPECT_EQ(s.Type, PrecipitationType::Snow);
    EXPECT_LT(s.GravityScale, 1.5f);    // Snow falls gently
    EXPECT_GT(s.DragCoefficient, 0.1f); // Significant drag
    EXPECT_GT(s.TurbulenceStrength, 0.2f);
    EXPECT_FLOAT_EQ(s.CollisionBounce, 0.0f); // Snow sticks
    EXPECT_TRUE(s.FeedAccumulation);
}

TEST(PrecipitationDefaults, RainDefaultsAreReasonable)
{
    auto r = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Rain);
    EXPECT_EQ(r.Type, PrecipitationType::Rain);
    EXPECT_GT(r.GravityScale, 1.5f);          // Rain falls fast
    EXPECT_LT(r.DragCoefficient, 0.1f);       // Low drag
    EXPECT_LT(r.TurbulenceStrength, 0.2f);    // Minimal turbulence
    EXPECT_FLOAT_EQ(r.CollisionBounce, 0.0f); // Rain splashes
    EXPECT_FALSE(r.FeedAccumulation);         // No accumulation
    EXPECT_GT(r.NearFieldSpeedMin, 3.0f);     // Faster than snow
}

TEST(PrecipitationDefaults, HailDefaultsAreReasonable)
{
    auto h = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Hail);
    EXPECT_EQ(h.Type, PrecipitationType::Hail);
    EXPECT_GT(h.GravityScale, 2.0f);    // Hail is heavy
    EXPECT_GT(h.CollisionBounce, 0.1f); // Hail bounces
    EXPECT_FALSE(h.FeedAccumulation);
    EXPECT_GT(h.NearFieldSpeedMin, 5.0f); // Very fast
}

TEST(PrecipitationDefaults, SleetDefaultsAreReasonable)
{
    auto sl = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Sleet);
    EXPECT_EQ(sl.Type, PrecipitationType::Sleet);
    EXPECT_TRUE(sl.FeedAccumulation); // Sleet accumulates (partially)
    // Sleet is between snow and rain
    auto snow = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Snow);
    auto rain = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Rain);
    EXPECT_GT(sl.GravityScale, snow.GravityScale);
    EXPECT_LT(sl.GravityScale, rain.GravityScale);
}

TEST(PrecipitationDefaults, RainFallsFasterThanSnow)
{
    auto snow = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Snow);
    auto rain = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Rain);
    EXPECT_GT(rain.NearFieldSpeedMin, snow.NearFieldSpeedMin);
    EXPECT_GT(rain.NearFieldSpeedMax, snow.NearFieldSpeedMax);
    EXPECT_GT(rain.GravityScale, snow.GravityScale);
}

// =============================================================================
// Type-Specific Particle Generation
// =============================================================================

TEST(PrecipitationEmitter, RainParticlesFallFasterThanSnow)
{
    glm::vec3 cameraPos(0.0f, 50.0f, 0.0f);
    PrecipitationSettings snowSettings = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Snow);
    snowSettings.Enabled = true;
    PrecipitationSettings rainSettings = PrecipitationSettings::GetDefaultsForType(PrecipitationType::Rain);
    rainSettings.Enabled = true;

    auto snowParticles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, snowSettings, 1.0f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 0.1f);
    auto rainParticles = PrecipitationEmitter::GenerateParticles(
        cameraPos, cameraPos, rainSettings, 1.0f,
        PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 0.1f);

    ASSERT_FALSE(snowParticles.empty()) << "Snow particles should be generated";
    ASSERT_FALSE(rainParticles.empty()) << "Rain particles should be generated";

    // Average fall speed (more negative = faster)
    f32 snowAvgY = 0.0f;
    for (const auto& p : snowParticles)
    {
        snowAvgY += p.VelocityMaxLifetime.y;
    }
    snowAvgY /= static_cast<f32>(snowParticles.size());

    f32 rainAvgY = 0.0f;
    for (const auto& p : rainParticles)
    {
        rainAvgY += p.VelocityMaxLifetime.y;
    }
    rainAvgY /= static_cast<f32>(rainParticles.size());

    // Rain should fall faster (more negative)
    EXPECT_LT(rainAvgY, snowAvgY);
}

TEST(PrecipitationEmitter, AllTypesGenerateValidParticles)
{
    glm::vec3 cameraPos(0.0f, 50.0f, 0.0f);
    const PrecipitationType types[] = {
        PrecipitationType::Snow,
        PrecipitationType::Rain,
        PrecipitationType::Hail,
        PrecipitationType::Sleet
    };

    for (auto type : types)
    {
        auto settings = PrecipitationSettings::GetDefaultsForType(type);
        settings.Enabled = true;

        auto particles = PrecipitationEmitter::GenerateParticles(
            cameraPos, cameraPos, settings, 1.0f,
            PrecipitationLayer::NearField, glm::vec3(0.0f, 0.0f, 1.0f), 5.0f, 0.1f);

        EXPECT_GT(particles.size(), 0u) << "Type " << static_cast<int>(type) << " produced no particles";

        for (const auto& p : particles)
        {
            EXPECT_TRUE(std::isfinite(p.PositionLifetime.x)) << "Type " << static_cast<int>(type);
            EXPECT_TRUE(std::isfinite(p.PositionLifetime.y)) << "Type " << static_cast<int>(type);
            EXPECT_TRUE(std::isfinite(p.PositionLifetime.z)) << "Type " << static_cast<int>(type);
            EXPECT_GT(p.InitialVelocitySize.w, 0.0f) << "Type " << static_cast<int>(type);
            EXPECT_LT(p.VelocityMaxLifetime.y, 0.0f) << "Type " << static_cast<int>(type); // Falls down
        }
    }
}
