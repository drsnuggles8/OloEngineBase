#include "OloEnginePCH.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/PrecipitationEmitter.h"
#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace OloEngine
{
    PrecipitationSystem::PrecipitationData PrecipitationSystem::s_Data;

    void PrecipitationSystem::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("PrecipitationSystem::Init called when already initialized");
            return;
        }

        // Create near-field GPU particle system (100k particles)
        s_Data.m_NearFieldSystem = CreateScope<GPUParticleSystem>(100000u);
        if (!s_Data.m_NearFieldSystem->IsInitialized())
        {
            OLO_CORE_ERROR("PrecipitationSystem: Near-field GPUParticleSystem failed to initialize");
            s_Data.m_NearFieldSystem.reset();
            return;
        }

        // Create far-field GPU particle system (200k particles)
        s_Data.m_FarFieldSystem = CreateScope<GPUParticleSystem>(200000u);
        if (!s_Data.m_FarFieldSystem->IsInitialized())
        {
            OLO_CORE_ERROR("PrecipitationSystem: Far-field GPUParticleSystem failed to initialize");
            s_Data.m_NearFieldSystem.reset();
            s_Data.m_FarFieldSystem.reset();
            return;
        }

        // Generate a procedural soft snowflake texture (64x64)
        constexpr u32 texSize = 64;
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

                // Smooth radial falloff with a slight crystalline shape
                f32 angle = std::atan2(dy, dx);
                f32 crystalMod = 0.9f + 0.1f * std::cos(angle * 6.0f); // 6-fold symmetry
                f32 alpha = std::clamp((1.0f - dist / crystalMod), 0.0f, 1.0f);
                alpha *= alpha; // Quadratic falloff
                alpha = std::min(alpha, 1.0f);
                auto a = static_cast<u8>(alpha * 255.0f);
                pixels[y * texSize + x] = (static_cast<u32>(a) << 24u) | 0x00FFFFFFu;
            }
        }

        TextureSpecification spec;
        spec.Width = texSize;
        spec.Height = texSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        s_Data.m_SnowflakeTexture = Texture2D::Create(spec);
        if (!s_Data.m_SnowflakeTexture)
        {
            OLO_CORE_ERROR("PrecipitationSystem: failed to create snowflake texture");
            s_Data.m_NearFieldSystem.reset();
            s_Data.m_FarFieldSystem.reset();
            return;
        }
        s_Data.m_SnowflakeTexture->SetData(pixels.data(), texSize * texSize * sizeof(u32));

        // Create precipitation UBO at binding 18
        s_Data.m_PrecipitationUBO = UniformBuffer::Create(
            PrecipitationUBOData::GetSize(),
            ShaderBindingLayout::UBO_PRECIPITATION);

        // Load accumulation feed compute shader (optional — may not exist yet)
        s_Data.m_FeedShader = ComputeShader::Create("assets/shaders/compute/Precipitation_Feed.comp");
        if (!s_Data.m_FeedShader)
        {
            OLO_CORE_WARN("PrecipitationSystem: Precipitation_Feed.comp not found — accumulation bridge disabled");
        }

        // Create GPU timer queries for performance monitoring
        glGenQueries(2, s_Data.m_TimerQueries);

        s_Data.m_CurrentIntensity = 0.0f;
        s_Data.m_TargetIntensity = 0.0f;
        s_Data.m_AccumulatedTime = 0.0f;
        s_Data.m_EmissionReductionFactor = 1.0f;
        s_Data.m_Initialized = true;

        OLO_CORE_INFO("PrecipitationSystem initialized (100k near + 200k far particles)");
    }

    void PrecipitationSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_TimerQueries[0] != 0)
        {
            glDeleteQueries(2, s_Data.m_TimerQueries);
            s_Data.m_TimerQueries[0] = 0;
            s_Data.m_TimerQueries[1] = 0;
        }

        s_Data.m_NearFieldSystem.reset();
        s_Data.m_FarFieldSystem.reset();
        s_Data.m_SnowflakeTexture = nullptr;
        s_Data.m_PrecipitationUBO = nullptr;
        s_Data.m_FeedShader = nullptr;
        s_Data.m_Initialized = false;

        OLO_CORE_INFO("PrecipitationSystem shut down");
    }

    bool PrecipitationSystem::IsInitialized()
    {
        return s_Data.m_Initialized;
    }

    void PrecipitationSystem::Update(const PrecipitationSettings& settings,
                                     const glm::vec3& cameraPos,
                                     const glm::vec3& windDir,
                                     f32 windSpeed,
                                     Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        f32 deltaTime = static_cast<f32>(dt);
        s_Data.m_AccumulatedTime += deltaTime;

        if (!settings.Enabled)
        {
            // Still simulate existing particles to let them die naturally
            if (s_Data.m_CurrentIntensity > 0.001f)
            {
                s_Data.m_TargetIntensity = 0.0f;
                s_Data.m_CurrentIntensity = std::lerp(s_Data.m_CurrentIntensity, 0.0f,
                    std::clamp(settings.TransitionSpeed * deltaTime, 0.0f, 1.0f));

                GPUSimParams simParams = {};
                simParams.DeltaTime = deltaTime;
                simParams.Gravity = glm::vec3(0.0f, -9.81f * settings.GravityScale, 0.0f);
                simParams.DragCoefficient = settings.DragCoefficient;
                simParams.EnableGravity = 1;
                simParams.EnableDrag = 1;
                simParams.EnableWind = 1;
                simParams.WindInfluence = settings.WindInfluence;
                simParams.EnableNoise = 1;
                simParams.NoiseStrength = settings.TurbulenceStrength;
                simParams.NoiseFrequency = settings.TurbulenceFrequency;
                simParams.EnableGroundCollision = settings.GroundCollisionEnabled ? 1 : 0;
                simParams.GroundY = settings.GroundY;
                simParams.CollisionBounce = 0.0f;
                simParams.CollisionFriction = 1.0f;

                simParams.MaxParticles = s_Data.m_NearFieldSystem->GetMaxParticles();
                s_Data.m_NearFieldSystem->Simulate(simParams);
                s_Data.m_NearFieldSystem->Compact();
                s_Data.m_NearFieldSystem->PrepareIndirectDraw();

                simParams.MaxParticles = s_Data.m_FarFieldSystem->GetMaxParticles();
                s_Data.m_FarFieldSystem->Simulate(simParams);
                s_Data.m_FarFieldSystem->Compact();
                s_Data.m_FarFieldSystem->PrepareIndirectDraw();
            }

            s_Data.m_LastCameraPos = cameraPos;
            return;
        }

        // --- Begin GPU timer ---
        u32 queryIdx = s_Data.m_CurrentTimerQuery;
        glBeginQuery(GL_TIME_ELAPSED, s_Data.m_TimerQueries[queryIdx]);

        // 1. Intensity interpolation
        s_Data.m_TargetIntensity = settings.Intensity;
        f32 lerpFactor = std::clamp(settings.TransitionSpeed * deltaTime, 0.0f, 1.0f);
        s_Data.m_CurrentIntensity = std::lerp(s_Data.m_CurrentIntensity, s_Data.m_TargetIntensity, lerpFactor);

        // 2. Apply emission reduction factor from frame budget
        f32 effectiveIntensity = s_Data.m_CurrentIntensity * s_Data.m_EmissionReductionFactor;

        // 3. Generate and emit near-field particles
        auto nearParticles = PrecipitationEmitter::GenerateSnowParticles(
            cameraPos, s_Data.m_LastCameraPos, settings, effectiveIntensity,
            PrecipitationLayer::NearField, windDir, windSpeed, deltaTime);

        if (!nearParticles.empty())
        {
            s_Data.m_NearFieldSystem->EmitParticles(std::span<const GPUParticle>(nearParticles));
        }

        // 4. Generate and emit far-field particles
        auto farParticles = PrecipitationEmitter::GenerateSnowParticles(
            cameraPos, s_Data.m_LastCameraPos, settings, effectiveIntensity,
            PrecipitationLayer::FarField, windDir, windSpeed, deltaTime);

        if (!farParticles.empty())
        {
            s_Data.m_FarFieldSystem->EmitParticles(std::span<const GPUParticle>(farParticles));
        }

        // 5. GPU simulation for both layers
        GPUSimParams simParams = {};
        simParams.DeltaTime = deltaTime;
        simParams.Gravity = glm::vec3(0.0f, -9.81f * settings.GravityScale, 0.0f);
        simParams.DragCoefficient = settings.DragCoefficient;
        simParams.EnableGravity = 1;
        simParams.EnableDrag = 1;
        simParams.EnableWind = 1;
        simParams.WindInfluence = settings.WindInfluence;
        simParams.EnableNoise = 1;
        simParams.NoiseStrength = settings.TurbulenceStrength;
        simParams.NoiseFrequency = settings.TurbulenceFrequency;
        simParams.EnableGroundCollision = settings.GroundCollisionEnabled ? 1 : 0;
        simParams.GroundY = settings.GroundY;
        simParams.CollisionBounce = 0.0f;     // Snow doesn't bounce
        simParams.CollisionFriction = 1.0f;   // Immediate stop on ground

        // Near-field
        simParams.MaxParticles = s_Data.m_NearFieldSystem->GetMaxParticles();
        s_Data.m_NearFieldSystem->Simulate(simParams);
        s_Data.m_NearFieldSystem->Compact();
        s_Data.m_NearFieldSystem->PrepareIndirectDraw();

        // Far-field
        simParams.MaxParticles = s_Data.m_FarFieldSystem->GetMaxParticles();
        s_Data.m_FarFieldSystem->Simulate(simParams);
        s_Data.m_FarFieldSystem->Compact();
        s_Data.m_FarFieldSystem->PrepareIndirectDraw();

        // 6. Accumulation bridge — feed landed particles to snow depth clipmap
        if (settings.FeedAccumulation && s_Data.m_FeedShader && SnowAccumulationSystem::IsInitialized())
        {
            s_Data.m_FeedShader->Bind();
            s_Data.m_FeedShader->SetFloat("u_AccumulationFeedRate", settings.AccumulationFeedRate);
            s_Data.m_FeedShader->SetFloat("u_GroundY", settings.GroundY);
            s_Data.m_FeedShader->SetFloat("u_GroundThreshold", 0.5f); // Tolerance for ground contact

            // Set clipmap parameters so the compute shader can convert world pos → clipmap UV
            glm::vec4 clipmapParams = SnowAccumulationSystem::GetClipmapParams();
            s_Data.m_FeedShader->SetFloat2("u_ClipmapCenter", glm::vec2(clipmapParams.x, clipmapParams.y));
            s_Data.m_FeedShader->SetFloat("u_ClipmapExtent", clipmapParams.z);
            s_Data.m_FeedShader->SetInt("u_Resolution", static_cast<i32>(SnowAccumulationSystem::GetTextureResolution()));

            // Bind snow depth texture as image for atomic writes via the clean API
            SnowAccumulationSystem::BindSnowDepthImage(1);

            // Feed from near-field system
            // The compute shader reads from the particle SSBO (already bound)
            // and writes to the snow depth image
            u32 nearAlive = s_Data.m_NearFieldSystem->GetMaxParticles();
            u32 groups = (nearAlive + 255) / 256;
            RenderCommand::DispatchCompute(groups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        }

        // --- End GPU timer ---
        glEndQuery(GL_TIME_ELAPSED);

        // Read back previous frame's timer result (async, no stall)
        u32 prevQueryIdx = 1 - queryIdx;
        if (s_Data.m_TimerQueryActive)
        {
            GLuint64 elapsedNs = 0;
            glGetQueryObjectui64v(s_Data.m_TimerQueries[prevQueryIdx], GL_QUERY_RESULT, &elapsedNs);
            s_Data.m_LastFrameTimeMs = static_cast<f32>(elapsedNs) / 1000000.0f;
        }
        s_Data.m_CurrentTimerQuery = prevQueryIdx;
        s_Data.m_TimerQueryActive = true;

        // 7. Frame budget control
        if (s_Data.m_LastFrameTimeMs > settings.FrameBudgetMs)
        {
            s_Data.m_EmissionReductionFactor *= 0.9f; // Reduce by 10%
            s_Data.m_EmissionReductionFactor = std::max(s_Data.m_EmissionReductionFactor, 0.1f);
        }
        else if (s_Data.m_LastFrameTimeMs < settings.FrameBudgetMs * 0.8f)
        {
            s_Data.m_EmissionReductionFactor = std::min(s_Data.m_EmissionReductionFactor * 1.05f, 1.0f);
        }

        s_Data.m_LastCameraPos = cameraPos;
    }

    void PrecipitationSystem::Render()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        ParticleBatchRenderer::Flush();

        // Render near-field particles (detailed, closer)
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_NearFieldSystem, s_Data.m_SnowflakeTexture);

        // Render far-field particles (atmospheric, dimmer)
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_FarFieldSystem, s_Data.m_SnowflakeTexture);
    }

    void PrecipitationSystem::UpdateScreenEffectsUBO(const PrecipitationSettings& settings,
                                                     const glm::vec2& windDirScreen,
                                                     f32 time)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !s_Data.m_PrecipitationUBO)
        {
            return;
        }

        auto& gpu = s_Data.m_GPUData;
        gpu.IntensityAndScreenFX = glm::vec4(
            s_Data.m_CurrentIntensity,
            settings.WindInfluence,
            settings.ScreenStreaksEnabled ? settings.ScreenStreakIntensity : 0.0f,
            settings.ScreenStreakLength);
        gpu.LensParams = glm::vec4(
            settings.LensImpactsEnabled ? settings.LensImpactRate : 0.0f,
            settings.LensImpactLifetime,
            settings.LensImpactSize,
            settings.Enabled ? 1.0f : 0.0f);
        gpu.ScreenWindAndTime = glm::vec4(
            windDirScreen.x, windDirScreen.y,
            time,
            settings.ScreenStreaksEnabled ? 1.0f : 0.0f);
        gpu.ParticleColor = settings.ParticleColor;

        s_Data.m_PrecipitationUBO->SetData(&gpu, PrecipitationUBOData::GetSize());
    }

    void PrecipitationSystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            if (s_Data.m_NearFieldSystem)
            {
                u32 maxNear = s_Data.m_NearFieldSystem->GetMaxParticles();
                s_Data.m_NearFieldSystem->Shutdown();
                s_Data.m_NearFieldSystem->Init(maxNear);
            }
            if (s_Data.m_FarFieldSystem)
            {
                u32 maxFar = s_Data.m_FarFieldSystem->GetMaxParticles();
                s_Data.m_FarFieldSystem->Shutdown();
                s_Data.m_FarFieldSystem->Init(maxFar);
            }
            s_Data.m_CurrentIntensity = 0.0f;
            s_Data.m_TargetIntensity = 0.0f;
            s_Data.m_AccumulatedTime = 0.0f;
            s_Data.m_EmissionReductionFactor = 1.0f;
        }
    }

    void PrecipitationSystem::SetIntensity(f32 intensity)
    {
        s_Data.m_TargetIntensity = std::clamp(intensity, 0.0f, 1.0f);
    }

    void PrecipitationSystem::SetIntensityImmediate(f32 intensity)
    {
        f32 clamped = std::clamp(intensity, 0.0f, 1.0f);
        s_Data.m_CurrentIntensity = clamped;
        s_Data.m_TargetIntensity = clamped;
    }

    f32 PrecipitationSystem::GetCurrentIntensity()
    {
        return s_Data.m_CurrentIntensity;
    }

    PrecipitationStats PrecipitationSystem::GetStatistics()
    {
        PrecipitationStats stats;
        if (s_Data.m_Initialized)
        {
            stats.EffectiveEmissionRate = PrecipitationEmitter::CalculateEmissionRate(
                4000, s_Data.m_CurrentIntensity) * s_Data.m_EmissionReductionFactor;
            stats.GPUTimeMs = s_Data.m_LastFrameTimeMs;
        }
        return stats;
    }
} // namespace OloEngine
