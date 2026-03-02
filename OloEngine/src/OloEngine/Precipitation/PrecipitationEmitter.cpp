#include "OloEnginePCH.h"
#include "OloEngine/Precipitation/PrecipitationEmitter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OloEngine
{
    FastRandom<PCG32Algorithm> PrecipitationEmitter::s_Rng;

    f32 PrecipitationEmitter::CalculateEmissionRate(u32 baseRate, f32 intensity)
    {
        // Quadratic scaling for perceptual linearity
        f32 clampedIntensity = std::clamp(intensity, 0.0f, 1.0f);
        return static_cast<f32>(baseRate) * clampedIntensity * clampedIntensity;
    }

    std::pair<glm::vec3, glm::vec3> PrecipitationEmitter::ComputeSpawnVolume(
        const glm::vec3& cameraPos,
        const glm::vec3& extent,
        const glm::vec3& windDir,
        f32 windSpeed)
    {
        // Shift spawn volume upwind so particles drift into view
        // Bias magnitude scales with wind speed (capped to half-extent)
        f32 biasMagnitude = std::min(windSpeed * 0.5f, extent.x * 0.5f);
        glm::vec3 windBias = -windDir * biasMagnitude;

        // Center the volume around camera with upwind offset
        // Offset Y upward so snow falls from above
        glm::vec3 center = cameraPos + windBias;
        center.y += extent.y * 0.5f; // Top of volume above camera

        glm::vec3 aabbMin = center - extent;
        glm::vec3 aabbMax = center + extent;

        return { aabbMin, aabbMax };
    }

    std::vector<GPUParticle> PrecipitationEmitter::GenerateSnowParticles(
        const glm::vec3& cameraPos,
        const glm::vec3& lastCameraPos,
        const PrecipitationSettings& settings,
        f32 intensity,
        PrecipitationLayer layer,
        const glm::vec3& windDir,
        f32 windSpeed,
        f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (intensity <= 0.001f || dt <= 0.0f)
        {
            return {};
        }

        // Select layer-specific parameters
        const glm::vec3& extent = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldExtent
            : settings.FarFieldExtent;
        f32 particleSize = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldParticleSize
            : settings.FarFieldParticleSize;
        f32 sizeVariance = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldSizeVariance
            : 0.01f; // Far-field: less variance
        f32 speedMin = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldSpeedMin
            : settings.FarFieldSpeedMin;
        f32 speedMax = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldSpeedMax
            : settings.FarFieldSpeedMax;
        f32 lifetime = (layer == PrecipitationLayer::NearField)
            ? settings.NearFieldLifetime
            : settings.FarFieldLifetime;
        f32 alphaMultiplier = (layer == PrecipitationLayer::NearField)
            ? 1.0f
            : settings.FarFieldAlphaMultiplier;

        // Calculate emission count for this frame
        f32 layerRateFraction = (layer == PrecipitationLayer::NearField) ? 0.4f : 0.6f;
        f32 effectiveRate = CalculateEmissionRate(settings.BaseEmissionRate, intensity) * layerRateFraction;
        f32 particlesToEmitF = effectiveRate * dt;

        // Fractional accumulation: use probabilistic rounding
        u32 particlesToEmit = static_cast<u32>(particlesToEmitF);
        f32 fractional = particlesToEmitF - static_cast<f32>(particlesToEmit);
        if (s_Rng.GetFloat32() < fractional)
        {
            ++particlesToEmit;
        }

        if (particlesToEmit == 0)
        {
            return {};
        }

        // Cap per-frame emission to avoid overflow
        u32 maxPerFrame = (layer == PrecipitationLayer::NearField)
            ? std::min(particlesToEmit, 4096u)
            : std::min(particlesToEmit, 4096u);
        particlesToEmit = maxPerFrame;

        // Compute spawn volume
        auto [aabbMin, aabbMax] = ComputeSpawnVolume(cameraPos, extent, windDir, windSpeed);

        // Velocity-compensated spawning: offset to counteract camera motion
        glm::vec3 cameraDelta = cameraPos - lastCameraPos;

        std::vector<GPUParticle> particles(particlesToEmit);

        for (u32 i = 0; i < particlesToEmit; ++i)
        {
            // Random position within AABB
            f32 px = s_Rng.GetFloat32InRange(aabbMin.x, aabbMax.x);
            f32 py = s_Rng.GetFloat32InRange(aabbMin.y, aabbMax.y);
            f32 pz = s_Rng.GetFloat32InRange(aabbMin.z, aabbMax.z);

            // Height-stratified density: bias toward mid-height using triangle distribution
            // Remap py to have more particles at 40-60% height
            f32 heightT = (py - aabbMin.y) / (aabbMax.y - aabbMin.y);
            f32 stratified = 1.0f - std::abs(heightT * 2.0f - 1.0f);
            stratified = stratified * stratified; // Cubic falloff at extremes
            if (s_Rng.GetFloat32() > (0.3f + 0.7f * stratified))
            {
                // Re-roll position toward center height
                py = s_Rng.GetFloat32InRange(
                    aabbMin.y + (aabbMax.y - aabbMin.y) * 0.3f,
                    aabbMin.y + (aabbMax.y - aabbMin.y) * 0.7f);
            }

            // Compensate for camera motion — spread particles along motion vector
            f32 motionT = s_Rng.GetFloat32();
            glm::vec3 pos = glm::vec3(px, py, pz) - cameraDelta * motionT;

            // Downward velocity with jitter + wind influence
            f32 fallSpeed = s_Rng.GetFloat32InRange(speedMin, speedMax) * settings.GravityScale;
            glm::vec3 vel(
                s_Rng.GetFloat32InRange(-0.2f, 0.2f), // Horizontal jitter
                -fallSpeed,
                s_Rng.GetFloat32InRange(-0.2f, 0.2f)  // Horizontal jitter
            );

            // Lifetime with slight variance
            f32 lt = lifetime * s_Rng.GetFloat32InRange(0.8f, 1.2f);

            // Size with variance
            f32 size = particleSize + s_Rng.GetFloat32InRange(-sizeVariance, sizeVariance);
            size = std::max(size, 0.005f);

            // Color with alpha variance
            glm::vec4 color = settings.ParticleColor;
            color.a *= alphaMultiplier;
            color.a *= s_Rng.GetFloat32InRange(1.0f - settings.ColorVariance, 1.0f);

            // Rotation
            f32 rotation = s_Rng.GetFloat32InRange(0.0f, 2.0f * std::numbers::pi_v<f32>);

            auto& p = particles[i];
            p.PositionLifetime = glm::vec4(pos, lt);
            p.VelocityMaxLifetime = glm::vec4(vel, lt);
            p.Color = color;
            p.InitialColor = color;
            p.InitialVelocitySize = glm::vec4(vel, size);
            p.Misc = glm::vec4(size, rotation, 1.0f, -1.0f); // alive=1, entityID=-1 (world particle)
        }

        return particles;
    }
} // namespace OloEngine
