#include "OloEnginePCH.h"
#include "OloEngine/Snow/SnowEjectaSystem.h"
#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Core/FastRandom.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace OloEngine
{
    SnowEjectaSystem::SnowEjectaData SnowEjectaSystem::s_Data;

    void SnowEjectaSystem::Init(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("SnowEjectaSystem::Init called when already initialized");
            return;
        }

        s_Data.m_GPUSystem = CreateScope<GPUParticleSystem>(maxParticles);
        if (!s_Data.m_GPUSystem->IsInitialized())
        {
            OLO_CORE_ERROR("SnowEjectaSystem: GPUParticleSystem failed to initialize");
            s_Data.m_GPUSystem.reset();
            return;
        }

        // Generate a procedural soft radial-gradient puff texture (32x32)
        constexpr u32 texSize = 32;
        std::vector<u32> pixels(texSize * texSize);
        constexpr f32 center = static_cast<f32>(texSize) * 0.5f;
        constexpr f32 invRadius = 1.0f / center;

        for (u32 y = 0; y < texSize; ++y)
        {
            for (u32 x = 0; x < texSize; ++x)
            {
                f32 dx = (static_cast<f32>(x) + 0.5f - center) * invRadius;
                f32 dy = (static_cast<f32>(y) + 0.5f - center) * invRadius;
                f32 dist = std::sqrt(dx * dx + dy * dy);
                // Smooth radial falloff: 1 at center, 0 at edge
                f32 alpha = std::clamp(1.0f - dist, 0.0f, 1.0f);
                alpha *= alpha; // Quadratic falloff for soft edge
                auto a = static_cast<u8>(alpha * 255.0f);
                // White RGB, varying alpha
                pixels[y * texSize + x] = (static_cast<u32>(a) << 24u) | 0x00FFFFFFu;
            }
        }

        TextureSpecification spec;
        spec.Width = texSize;
        spec.Height = texSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        s_Data.m_EjectaTexture = Texture2D::Create(spec);
        s_Data.m_EjectaTexture->SetData(pixels.data(), texSize * texSize * sizeof(u32));

        s_Data.m_Initialized = true;
        OLO_CORE_INFO("SnowEjectaSystem initialized ({} max particles, 32x32 puff texture)", maxParticles);
    }

    void SnowEjectaSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_GPUSystem.reset();
        s_Data.m_EjectaTexture = nullptr;
        s_Data.m_Initialized = false;

        OLO_CORE_INFO("SnowEjectaSystem shut down");
    }

    bool SnowEjectaSystem::IsInitialized()
    {
        return s_Data.m_Initialized;
    }

    void SnowEjectaSystem::EmitAt(const glm::vec3& position,
                                  const glm::vec3& deformerVelocity,
                                  f32 deformRadius,
                                  f32 deformDepth,
                                  const SnowEjectaSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !settings.Enabled)
        {
            return;
        }

        // Only emit if the deformer is moving fast enough
        f32 speed = glm::length(deformerVelocity);
        if (speed < settings.VelocityThreshold)
        {
            return;
        }

        // Scale particle count by deform depth (deeper stamp = more ejecta)
        f32 depthScale = std::clamp(deformDepth / 0.1f, 0.5f, 3.0f);
        u32 count = static_cast<u32>(static_cast<f32>(settings.ParticlesPerDeform) * depthScale);
        count = std::min(count, 64u); // Cap per-stamp burst

        // Compute movement direction for wake-biased emission
        glm::vec3 moveDir = speed > 0.001f ? deformerVelocity / speed : glm::vec3(0.0f, 0.0f, 1.0f);

        FastRandom rng;

        std::vector<GPUParticle> particles(count);
        for (u32 i = 0; i < count; ++i)
        {
            // Random angle around the vertical axis
            f32 angle = rng.GetFloat32InRange(0.0f, 2.0f * std::numbers::pi_v<f32>);
            f32 cosA = std::cos(angle);
            f32 sinA = std::sin(angle);

            // Random outward + upward velocity
            f32 speedMult = settings.EjectaSpeed *
                            rng.GetFloat32InRange(1.0f - settings.SpeedVariance, 1.0f + settings.SpeedVariance);

            // Radial outward direction in XZ plane, biased away from movement direction
            glm::vec3 outward(cosA, 0.0f, sinA);
            outward = glm::normalize(outward + moveDir * 0.4f);

            // Bias velocity: fraction upward, rest outward
            glm::vec3 vel = outward * speedMult * (1.0f - settings.UpwardBias) + glm::vec3(0.0f, speedMult * settings.UpwardBias, 0.0f);

            // Add a fraction of the deformer's velocity for momentum transfer
            vel += deformerVelocity * 0.3f;

            // Random offset within the deform radius
            f32 offsetR = rng.GetFloat32InRange(0.0f, deformRadius * 0.8f);
            glm::vec3 offset(cosA * offsetR, 0.0f, sinA * offsetR);

            f32 lifetime = rng.GetFloat32InRange(settings.LifetimeMin, settings.LifetimeMax);
            f32 size = settings.InitialSize + rng.GetFloat32InRange(-settings.SizeVariance, settings.SizeVariance);
            size = std::max(size, 0.005f);

            auto& p = particles[i];
            p.PositionLifetime = glm::vec4(position + offset, lifetime);
            p.VelocityMaxLifetime = glm::vec4(vel, lifetime);
            p.Color = settings.Color;
            p.InitialColor = settings.Color;
            p.InitialVelocitySize = glm::vec4(vel, size);
            p.Misc = glm::vec4(size, rng.GetFloat32InRange(0.0f, 2.0f * std::numbers::pi_v<f32>), 1.0f, -1.0f);
        }

        s_Data.m_GPUSystem->EmitParticles(std::span<const GPUParticle>(particles));
    }

    void SnowEjectaSystem::Update(const SnowEjectaSettings& settings, Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !settings.Enabled)
        {
            return;
        }

        // Fill simulation parameters
        GPUSimParams simParams;
        simParams.DeltaTime = static_cast<f32>(dt);
        simParams.Gravity = glm::vec3(0.0f, -9.81f * settings.GravityScale, 0.0f);
        simParams.DragCoefficient = settings.DragCoefficient;
        simParams.MaxParticles = s_Data.m_GPUSystem->GetMaxParticles();
        simParams.EnableGravity = 1;
        simParams.EnableDrag = 1;
        simParams.EnableWind = 1;       // Let wind affect snow puffs
        simParams.WindInfluence = 0.5f; // Moderate wind sensitivity
        simParams.EnableNoise = 1;      // Turbulence for organic feel
        simParams.NoiseStrength = 0.3f;
        simParams.NoiseFrequency = 2.0f;
        simParams.EnableGroundCollision = 1;
        simParams.GroundY = 0.0f;
        simParams.CollisionBounce = 0.0f;   // Snow doesn't bounce
        simParams.CollisionFriction = 1.0f; // Full friction on landing

        // Run the GPU compute pipeline
        s_Data.m_GPUSystem->Simulate(simParams);
        s_Data.m_GPUSystem->Compact();
        s_Data.m_GPUSystem->PrepareIndirectDraw();
    }

    void SnowEjectaSystem::Render()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        ParticleBatchRenderer::Flush();

        // Render with additive blending for snow puff glow effect
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_GPUSystem, s_Data.m_EjectaTexture);
    }

    void SnowEjectaSystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized && s_Data.m_GPUSystem)
        {
            // Re-initialize the particle system to clear all particles
            u32 maxParticles = s_Data.m_GPUSystem->GetMaxParticles();
            s_Data.m_GPUSystem->Shutdown();
            s_Data.m_GPUSystem->Init(maxParticles);
        }
    }
} // namespace OloEngine
