#include "OloEnginePCH.h"
#include "OloEngine/Wind/WindSystem.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    WindSystem::WindSystemData WindSystem::s_Data;

    static glm::vec3 NormalizeOrFallback(const glm::vec3& dir)
    {
        if (glm::dot(dir, dir) < 1e-8f)
        {
            return glm::vec3(1.0f, 0.0f, 0.0f);
        }
        return glm::normalize(dir);
    }

    void WindSystem::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("WindSystem::Init called when already initialized");
            return;
        }

        // Create 3D wind-field texture (128³ RGBA16F)
        Texture3DSpecification spec;
        spec.Width = 128;
        spec.Height = 128;
        spec.Depth = 128;
        spec.Format = Texture3DFormat::RGBA16F;
        s_Data.m_WindField = Texture3D::Create(spec);

        // Create wind UBO at binding 15
        s_Data.m_WindUBO = UniformBuffer::Create(WindUBOData::GetSize(), ShaderBindingLayout::UBO_WIND);

        // Load the wind generation compute shader
        s_Data.m_GenerateShader = ComputeShader::Create("assets/shaders/compute/Wind_Generate.comp");

        // Verify all resources were created successfully
        if (!s_Data.m_WindField || !s_Data.m_WindUBO || !s_Data.m_GenerateShader)
        {
            OLO_CORE_ERROR("WindSystem::Init failed — one or more GPU resources could not be created");
            s_Data.m_GenerateShader = nullptr;
            s_Data.m_WindField = nullptr;
            s_Data.m_WindUBO = nullptr;
            return;
        }

        s_Data.m_AccumulatedTime = 0.0f;
        s_Data.m_Initialized = true;

        OLO_CORE_INFO("WindSystem initialized (128^3 RGBA16F wind field)");
    }

    void WindSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_GenerateShader = nullptr;
        s_Data.m_WindField = nullptr;
        s_Data.m_WindUBO = nullptr;
        s_Data.m_Initialized = false;

        OLO_CORE_INFO("WindSystem shut down");
    }

    bool WindSystem::IsInitialized()
    {
        OLO_PROFILE_FUNCTION();

        return s_Data.m_Initialized;
    }

    void WindSystem::Update(const WindSettings& settings, const glm::vec3& cameraPos, Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        s_Data.m_AccumulatedTime += static_cast<f32>(dt);

        // Compute grid AABB centered on camera
        f32 halfSize = settings.GridWorldSize * 0.5f;
        glm::vec3 gridMin = cameraPos - glm::vec3(halfSize);

        // Safe-normalize direction (fallback to +X if zero-length)
        glm::vec3 safeDir = NormalizeOrFallback(settings.Direction);

        // Clamp resolution to the allocated texture size (128)
        constexpr u32 kTextureSize = 128;
        u32 resolvedResolution = std::min(settings.GridResolution, kTextureSize);
        if (resolvedResolution == 0)
        {
            resolvedResolution = kTextureSize;
        }

        // Pack UBO data
        auto& gpu = s_Data.m_GPUData;
        gpu.DirectionAndSpeed = glm::vec4(safeDir, settings.Speed);
        gpu.GustAndTurbulence = glm::vec4(settings.GustStrength, settings.GustFrequency,
                                          settings.TurbulenceIntensity, settings.TurbulenceScale);
        gpu.GridMinAndSize = glm::vec4(gridMin, settings.GridWorldSize);
        gpu.TimeAndFlags = glm::vec4(s_Data.m_AccumulatedTime,
                                     settings.Enabled ? 1.0f : 0.0f,
                                     static_cast<f32>(resolvedResolution),
                                     0.0f);

        // Upload UBO
        s_Data.m_WindUBO->SetData(&gpu, WindUBOData::GetSize());

        if (!settings.Enabled)
        {
            return; // UBO uploaded with Enabled=0; consumers will skip sampling
        }

        // --- Dispatch compute shader to regenerate the wind field ---
        s_Data.m_GenerateShader->Bind();

        // Set uniforms for the compute shader
        s_Data.m_GenerateShader->SetFloat3("u_GridMin", gridMin);
        s_Data.m_GenerateShader->SetFloat("u_GridWorldSize", settings.GridWorldSize);
        s_Data.m_GenerateShader->SetInt("u_GridResolution", static_cast<i32>(resolvedResolution));
        s_Data.m_GenerateShader->SetFloat3("u_WindDirection", safeDir);
        s_Data.m_GenerateShader->SetFloat("u_WindSpeed", settings.Speed);
        s_Data.m_GenerateShader->SetFloat("u_GustStrength", settings.GustStrength);
        s_Data.m_GenerateShader->SetFloat("u_GustFrequency", settings.GustFrequency);
        s_Data.m_GenerateShader->SetFloat("u_TurbulenceIntensity", settings.TurbulenceIntensity);
        s_Data.m_GenerateShader->SetFloat("u_TurbulenceScale", settings.TurbulenceScale);
        s_Data.m_GenerateShader->SetFloat("u_Time", s_Data.m_AccumulatedTime);

        // Bind wind field as image for writing (unit 0, mip 0, layered for 3D)
        RenderCommand::BindImageTexture(0, s_Data.m_WindField->GetRendererID(),
                                        0, true, 0, GL_WRITE_ONLY, GL_RGBA16F);

        // Dispatch: local_size(8,8,8) → ceil(resolution/8) groups per axis
        u32 groups = (resolvedResolution + 7) / 8;
        RenderCommand::DispatchCompute(groups, groups, groups);

        // Barrier: ensure all image stores complete before consumers sample
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
    }

    void WindSystem::BindWindTexture()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized && s_Data.m_WindField)
        {
            s_Data.m_WindField->Bind(ShaderBindingLayout::TEX_WIND_FIELD);
        }
    }

    glm::vec3 WindSystem::GetWindAtPoint(const WindSettings& settings,
                                         const glm::vec3& worldPos,
                                         f32 time)
    {
        OLO_PROFILE_FUNCTION();

        if (!settings.Enabled)
        {
            return glm::vec3(0.0f);
        }

        glm::vec3 dir = NormalizeOrFallback(settings.Direction);
        f32 speed = settings.Speed;

        // Gust modulation: sine wave with spatial offset
        f32 gustPhase = time * settings.GustFrequency * 6.2831853f; // 2 * PI
        f32 spatialOffset = glm::dot(worldPos, dir) * 0.05f;
        f32 gust = 1.0f + settings.GustStrength * std::sin(gustPhase + spatialOffset);

        return dir * speed * gust;
    }
} // namespace OloEngine
