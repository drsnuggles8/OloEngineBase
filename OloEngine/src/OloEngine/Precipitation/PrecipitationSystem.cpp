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

    void PrecipitationSystem::Init(u32 maxNearField, u32 maxFarField)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("PrecipitationSystem::Init called when already initialized");
            return;
        }

        // Create near-field GPU particle system
        s_Data.m_NearFieldSystem = CreateScope<GPUParticleSystem>(maxNearField);
        if (!s_Data.m_NearFieldSystem->IsInitialized())
        {
            OLO_CORE_ERROR("PrecipitationSystem: Near-field GPUParticleSystem failed to initialize");
            s_Data.m_NearFieldSystem.reset();
            return;
        }

        // Create far-field GPU particle system
        s_Data.m_FarFieldSystem = CreateScope<GPUParticleSystem>(maxFarField);
        if (!s_Data.m_FarFieldSystem->IsInitialized())
        {
            OLO_CORE_ERROR("PrecipitationSystem: Far-field GPUParticleSystem failed to initialize");
            s_Data.m_NearFieldSystem.reset();
            s_Data.m_FarFieldSystem.reset();
            return;
        }

        // Generate procedural precipitation textures (64x64 each)
        constexpr u32 texSize = 64;
        constexpr f32 center = static_cast<f32>(texSize) * 0.5f;
        constexpr f32 invRadius = 1.0f / center;

        auto createTex = [&](auto pixelGen) -> Ref<Texture2D>
        {
            std::vector<u32> pixels(texSize * texSize);
            for (u32 y = 0; y < texSize; ++y)
            {
                for (u32 x = 0; x < texSize; ++x)
                {
                    pixels[y * texSize + x] = pixelGen(x, y);
                }
            }
            TextureSpecification spec;
            spec.Width = texSize;
            spec.Height = texSize;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            auto tex = Texture2D::Create(spec);
            if (tex)
            {
                tex->SetData(pixels.data(), texSize * texSize * sizeof(u32));
            }
            return tex;
        };

        // Snow: soft radial with 6-fold crystalline modulation
        s_Data.m_SnowflakeTexture = createTex([&](u32 x, u32 y) -> u32
                                              {
            f32 dx = (static_cast<f32>(x) + 0.5f - center) * invRadius;
            f32 dy = (static_cast<f32>(y) + 0.5f - center) * invRadius;
            f32 dist = std::sqrt(dx * dx + dy * dy);
            f32 angle = std::atan2(dy, dx);
            f32 crystalMod = 0.9f + 0.1f * std::cos(angle * 6.0f);
            f32 alpha = std::clamp((1.0f - dist / crystalMod), 0.0f, 1.0f);
            alpha *= alpha;
            auto a = static_cast<u8>(std::min(alpha, 1.0f) * 255.0f);
            return (static_cast<u32>(a) << 24u) | 0x00FFFFFFu; });

        // Rain: vertically elongated teardrop
        s_Data.m_RaindropTexture = createTex([&](u32 x, u32 y) -> u32
                                             {
            f32 dx = (static_cast<f32>(x) + 0.5f - center) * invRadius;
            f32 dy = (static_cast<f32>(y) + 0.5f - center) * invRadius;
            // Stretch vertically: compress Y, widen X
            f32 distX = dx;
            f32 distY = dy * 0.4f; // Elongate vertically
            f32 dist = std::sqrt(distX * distX + distY * distY);
            // Teardrop: narrower at top, wider at bottom
            f32 tapering = 1.0f - 0.3f * std::max(dy, 0.0f);
            f32 alpha = std::clamp((1.0f - dist / std::max(tapering, 0.3f)), 0.0f, 1.0f);
            alpha = alpha * alpha * alpha; // Sharper falloff — glassy look
            // Slight blue tint (ABGR byte order for RGBA8)
            auto a = static_cast<u8>(std::min(alpha, 1.0f) * 255.0f);
            auto r = static_cast<u8>(200);
            auto g = static_cast<u8>(210);
            auto b = static_cast<u8>(240);
            return (static_cast<u32>(a) << 24u) | (static_cast<u32>(b) << 16u)
                 | (static_cast<u32>(g) << 8u)  | static_cast<u32>(r); });

        // Hail: hard-edged sphere (bright, opaque center)
        s_Data.m_HailstoneTexture = createTex([&](u32 x, u32 y) -> u32
                                              {
            f32 dx = (static_cast<f32>(x) + 0.5f - center) * invRadius;
            f32 dy = (static_cast<f32>(y) + 0.5f - center) * invRadius;
            f32 dist = std::sqrt(dx * dx + dy * dy);
            // Sharp circular edge with a specular highlight
            f32 alpha = std::clamp((1.0f - dist) * 3.0f, 0.0f, 1.0f); // Hard edge
            // Specular highlight at upper-left
            f32 highlight = std::max(1.0f - std::sqrt((dx + 0.3f) * (dx + 0.3f) + (dy + 0.3f) * (dy + 0.3f)) * 2.5f, 0.0f);
            f32 luminance = std::clamp(0.85f + highlight * 0.15f, 0.0f, 1.0f);
            auto a = static_cast<u8>(std::min(alpha, 1.0f) * 255.0f);
            auto c = static_cast<u8>(luminance * 255.0f);
            return (static_cast<u32>(a) << 24u) | (static_cast<u32>(c) << 16u)
                 | (static_cast<u32>(c) << 8u)  | static_cast<u32>(c); });

        // Sleet: rounded flake — between snow and rain (less crystalline)
        s_Data.m_SleetTexture = createTex([&](u32 x, u32 y) -> u32
                                          {
            f32 dx = (static_cast<f32>(x) + 0.5f - center) * invRadius;
            f32 dy = (static_cast<f32>(y) + 0.5f - center) * invRadius;
            // Slight 4-fold symmetry (less than snow's 6-fold)
            f32 angle = std::atan2(dy, dx);
            f32 mod = 0.95f + 0.05f * std::cos(angle * 4.0f);
            // Slight vertical elongation
            f32 adjustedDist = std::sqrt(dx * dx + dy * dy * 0.85f);
            f32 alpha = std::clamp((1.0f - adjustedDist / mod), 0.0f, 1.0f);
            alpha *= alpha;
            // Gray-white tint
            auto a = static_cast<u8>(std::min(alpha, 1.0f) * 255.0f);
            return (static_cast<u32>(a) << 24u) | 0x00E8E8F0u; });

        if (!s_Data.m_SnowflakeTexture || !s_Data.m_RaindropTexture || !s_Data.m_HailstoneTexture || !s_Data.m_SleetTexture)
        {
            OLO_CORE_ERROR("PrecipitationSystem: failed to create precipitation textures");
            s_Data.m_NearFieldSystem.reset();
            s_Data.m_FarFieldSystem.reset();
            return;
        }

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
        s_Data.m_RaindropTexture = nullptr;
        s_Data.m_HailstoneTexture = nullptr;
        s_Data.m_SleetTexture = nullptr;
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
            // Still simulate existing particles to let them die naturally.
            // Use a drain timer to ensure we don't stop while particles are alive.
            if (s_Data.m_CurrentIntensity > 0.001f)
            {
                // Start/extend drain: worst-case particle lifetime plus some margin
                f32 maxLifetime = std::max(settings.NearFieldLifetime, settings.FarFieldLifetime) * 1.2f;
                s_Data.m_DrainTimeRemaining = std::max(s_Data.m_DrainTimeRemaining, maxLifetime);
            }

            if (s_Data.m_CurrentIntensity > 0.001f || s_Data.m_DrainTimeRemaining > 0.0f)
            {
                s_Data.m_TargetIntensity = 0.0f;
                s_Data.m_CurrentIntensity = std::lerp(s_Data.m_CurrentIntensity, 0.0f,
                                                      std::clamp(settings.TransitionSpeed * deltaTime, 0.0f, 1.0f));

                s_Data.m_DrainTimeRemaining = std::max(s_Data.m_DrainTimeRemaining - deltaTime, 0.0f);
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
        s_Data.m_LastBaseEmissionRate = settings.BaseEmissionRate;
        s_Data.m_TargetIntensity = settings.Intensity;
        f32 lerpFactor = std::clamp(settings.TransitionSpeed * deltaTime, 0.0f, 1.0f);
        s_Data.m_CurrentIntensity = std::lerp(s_Data.m_CurrentIntensity, s_Data.m_TargetIntensity, lerpFactor);

        // 2. Apply emission reduction factor from frame budget
        f32 effectiveIntensity = s_Data.m_CurrentIntensity * s_Data.m_EmissionReductionFactor;

        // 3. Generate and emit near-field particles
        auto nearParticles = PrecipitationEmitter::GenerateParticles(
            cameraPos, s_Data.m_LastCameraPos, settings, effectiveIntensity,
            PrecipitationLayer::NearField, windDir, windSpeed, deltaTime);

        if (!nearParticles.empty())
        {
            s_Data.m_NearFieldSystem->EmitParticles(std::span<const GPUParticle>(nearParticles));
        }

        // 4. Generate and emit far-field particles
        auto farParticles = PrecipitationEmitter::GenerateParticles(
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
        simParams.CollisionBounce = settings.CollisionBounce;
        simParams.CollisionFriction = settings.CollisionFriction;

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
        //    Only snow and sleet contribute to accumulation
        bool typeCanAccumulate = (settings.Type == PrecipitationType::Snow || settings.Type == PrecipitationType::Sleet);
        if (settings.FeedAccumulation && typeCanAccumulate && s_Data.m_FeedShader && SnowAccumulationSystem::IsInitialized())
        {
            s_Data.m_FeedShader->Bind();
            s_Data.m_FeedShader->SetFloat("u_AccumulationFeedRate", settings.AccumulationFeedRate);
            s_Data.m_FeedShader->SetFloat("u_GroundY", settings.GroundY);
            s_Data.m_FeedShader->SetFloat("u_GroundThreshold", 0.5f); // Tolerance for ground contact

            // Set clipmap parameters so the compute shader can convert world pos → clipmap UV
            glm::vec4 clipmapParams = SnowAccumulationSystem::GetClipmapParams();
            s_Data.m_FeedShader->SetFloat2("u_ClipmapCenter", glm::vec2(clipmapParams.x, clipmapParams.y));
            s_Data.m_FeedShader->SetFloat("u_ClipmapExtent", clipmapParams.z);
            s_Data.m_FeedShader->SetInt("u_ClipmapResolution", static_cast<i32>(SnowAccumulationSystem::GetTextureResolution()));

            // Bind snow depth texture as R32UI image for atomic CAS writes
            SnowAccumulationSystem::BindSnowDepthImageUint(1);

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

        // Read back previous frame's timer result (double-buffered to avoid stalls)
        u32 prevQueryIdx = 1 - queryIdx;
        if (s_Data.m_TimerQueryActive)
        {
            GLint available = GL_FALSE;
            glGetQueryObjectiv(s_Data.m_TimerQueries[prevQueryIdx], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available == GL_TRUE)
            {
                GLuint64 elapsedNs = 0;
                glGetQueryObjectui64v(s_Data.m_TimerQueries[prevQueryIdx], GL_QUERY_RESULT, &elapsedNs);
                s_Data.m_LastFrameTimeMs = static_cast<f32>(elapsedNs) / 1000000.0f;
            }
            // If not available yet, keep the previous value — avoids CPU stall
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

        // Select texture based on active precipitation type
        const auto& tex = [&]() -> const Ref<Texture2D>&
        {
            // We need to know the current type — peek at the UBO data
            i32 typeVal = static_cast<i32>(s_Data.m_GPUData.TypeParams.x);
            switch (static_cast<PrecipitationType>(typeVal))
            {
                case PrecipitationType::Rain:
                    return s_Data.m_RaindropTexture;
                case PrecipitationType::Hail:
                    return s_Data.m_HailstoneTexture;
                case PrecipitationType::Sleet:
                    return s_Data.m_SleetTexture;
                default:
                    return s_Data.m_SnowflakeTexture;
            }
        }();

        // Render near-field particles (detailed, closer)
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_NearFieldSystem, tex);

        // Render far-field particles (atmospheric, dimmer)
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_FarFieldSystem, tex);
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
        gpu.TypeParams = glm::vec4(
            static_cast<f32>(static_cast<i32>(settings.Type)),
            0.0f, 0.0f, 0.0f);

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
            s_Data.m_DrainTimeRemaining = 0.0f;
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
                                              s_Data.m_LastBaseEmissionRate, s_Data.m_CurrentIntensity) *
                                          s_Data.m_EmissionReductionFactor;
            stats.GPUTimeMs = s_Data.m_LastFrameTimeMs;
        }
        return stats;
    }
} // namespace OloEngine
