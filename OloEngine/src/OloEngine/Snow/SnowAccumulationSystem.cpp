#include "OloEnginePCH.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    static constexpr u32 kSnowDepthResolution = 2048;

    SnowAccumulationSystem::SnowAccumulationData SnowAccumulationSystem::s_Data;

    void SnowAccumulationSystem::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("SnowAccumulationSystem::Init called when already initialized");
            return;
        }

        // Create R32F snow depth texture
        TextureSpecification spec;
        spec.Width = kSnowDepthResolution;
        spec.Height = kSnowDepthResolution;
        spec.Format = ImageFormat::R32F;
        spec.GenerateMips = false;

        s_Data.m_SnowDepthTexture = Texture2D::Create(spec);

        // Create accumulation UBO at binding 16
        s_Data.m_AccumulationUBO = UniformBuffer::Create(
            SnowAccumulationUBOData::GetSize(),
            ShaderBindingLayout::UBO_SNOW_ACCUMULATION);

        // Load compute shaders
        s_Data.m_AccumulateShader = ComputeShader::Create("assets/shaders/compute/Snow_Accumulate.comp");
        s_Data.m_ClearShader = ComputeShader::Create("assets/shaders/compute/Snow_Clear.comp");
        s_Data.m_DeformShader = ComputeShader::Create("assets/shaders/compute/Snow_Deform.comp");

        // Create deformer SSBO (binding 7) — start with space for 64 stamps
        constexpr u32 initialStampCapacity = 64;
        constexpr u32 stampSize = 2 * sizeof(glm::vec4); // 32 bytes per stamp
        s_Data.m_DeformerSSBO = StorageBuffer::Create(
            initialStampCapacity * stampSize,
            ShaderBindingLayout::SSBO_SNOW_DEFORMERS);

        // Verify all resources were created successfully
        if (!s_Data.m_SnowDepthTexture || !s_Data.m_AccumulationUBO ||
            !s_Data.m_AccumulateShader || !s_Data.m_ClearShader || !s_Data.m_DeformShader ||
            !s_Data.m_DeformerSSBO)
        {
            OLO_CORE_ERROR("SnowAccumulationSystem::Init failed — one or more GPU resources could not be created");
            s_Data.m_AccumulateShader = nullptr;
            s_Data.m_ClearShader = nullptr;
            s_Data.m_DeformShader = nullptr;
            s_Data.m_SnowDepthTexture = nullptr;
            s_Data.m_AccumulationUBO = nullptr;
            s_Data.m_DeformerSSBO = nullptr;
            return;
        }

        s_Data.m_AccumulatedTime = 0.0f;
        s_Data.m_NeedsClear = true;
        s_Data.m_Initialized = true;

        OLO_CORE_INFO("SnowAccumulationSystem initialized (2048x2048 R32F snow depth)");
    }

    void SnowAccumulationSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_AccumulateShader = nullptr;
        s_Data.m_ClearShader = nullptr;
        s_Data.m_DeformShader = nullptr;
        s_Data.m_SnowDepthTexture = nullptr;
        s_Data.m_AccumulationUBO = nullptr;
        s_Data.m_DeformerSSBO = nullptr;

        s_Data.m_Initialized = false;

        OLO_CORE_INFO("SnowAccumulationSystem shut down");
    }

    bool SnowAccumulationSystem::IsInitialized()
    {
        OLO_PROFILE_FUNCTION();

        return s_Data.m_Initialized;
    }

    void SnowAccumulationSystem::ComputeClipmapMatrices(
        const glm::vec3& center,
        const SnowAccumulationSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        auto& gpu = s_Data.m_GPUData;
        u32 numRings = std::clamp(settings.NumClipmapRings, 1u, SnowAccumulationUBOData::MAX_CLIPMAP_RINGS);
        f32 baseExtent = settings.ClipmapExtent;
        f32 resolution = static_cast<f32>(settings.ClipmapResolution);

        for (u32 ring = 0; ring < numRings; ++ring)
        {
            // Each ring doubles in extent
            f32 extent = baseExtent * static_cast<f32>(1u << ring);
            f32 halfExtent = extent * 0.5f;

            // Texel snapping: snap center to texel grid to prevent shimmer
            f32 texelSize = extent / resolution;
            f32 snappedX = std::floor(center.x / texelSize) * texelSize;
            f32 snappedZ = std::floor(center.z / texelSize) * texelSize;

            // Top-down orthographic projection (Y-up)
            glm::mat4 orthoProj = glm::ortho(
                -halfExtent, halfExtent,
                -halfExtent, halfExtent,
                -500.0f, 500.0f); // Large near/far for tall terrain

            // View: look straight down from above the snapped center
            glm::mat4 view = glm::lookAt(
                glm::vec3(snappedX, 100.0f, snappedZ),
                glm::vec3(snappedX, 0.0f, snappedZ),
                glm::vec3(0.0f, 0.0f, -1.0f));

            gpu.ClipmapViewProj[ring] = orthoProj * view;
            gpu.ClipmapCenterAndExtent[ring] = glm::vec4(snappedX, snappedZ, extent, 1.0f / extent);
        }
    }

    void SnowAccumulationSystem::Update(
        const SnowAccumulationSettings& settings,
        const glm::vec3& cameraPos,
        Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            return;
        }

        s_Data.m_AccumulatedTime += static_cast<f32>(dt);

        // Update clipmap matrices
        ComputeClipmapMatrices(cameraPos, settings);

        // Pack UBO data
        auto& gpu = s_Data.m_GPUData;
        gpu.AccumulationParams = glm::vec4(
            settings.AccumulationRate,
            settings.MaxDepth,
            settings.MeltRate,
            settings.RestorationRate);
        gpu.DisplacementParams = glm::vec4(
            settings.DisplacementScale,
            settings.SnowDensity,
            settings.Enabled ? 1.0f : 0.0f,
            static_cast<f32>(std::clamp(settings.NumClipmapRings, 1u, SnowAccumulationUBOData::MAX_CLIPMAP_RINGS)));

        // Upload UBO
        s_Data.m_AccumulationUBO->SetData(&gpu, SnowAccumulationUBOData::GetSize());

        if (!settings.Enabled)
        {
            return; // UBO uploaded with Enabled=0; consumers will skip
        }

        // Clear depth buffer if needed (scene load, reset)
        if (s_Data.m_NeedsClear)
        {
            s_Data.m_ClearShader->Bind();
            RenderCommand::BindImageTexture(0, s_Data.m_SnowDepthTexture->GetRendererID(),
                                            0, false, 0, GL_WRITE_ONLY, GL_R32F);
            u32 groups = (kSnowDepthResolution + 15) / 16;
            RenderCommand::DispatchCompute(groups, groups, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
            s_Data.m_NeedsClear = false;
        }

        // --- Dispatch Snow_Accumulate compute ---
        s_Data.m_AccumulateShader->Bind();

        s_Data.m_AccumulateShader->SetFloat("u_DeltaTime", static_cast<f32>(dt));
        s_Data.m_AccumulateShader->SetFloat("u_AccumulationRate", settings.AccumulationRate);
        s_Data.m_AccumulateShader->SetFloat("u_MaxDepth", settings.MaxDepth);
        s_Data.m_AccumulateShader->SetFloat("u_MeltRate", settings.MeltRate);
        s_Data.m_AccumulateShader->SetFloat("u_RestorationRate", settings.RestorationRate);
        s_Data.m_AccumulateShader->SetFloat("u_SnowDensity", settings.SnowDensity);
        s_Data.m_AccumulateShader->SetInt("u_Resolution", static_cast<i32>(kSnowDepthResolution));

        // Clipmap center and extent for ring 0 (innermost)
        const auto& ce = gpu.ClipmapCenterAndExtent[0];
        s_Data.m_AccumulateShader->SetFloat2("u_ClipmapCenter", glm::vec2(ce.x, ce.y));
        s_Data.m_AccumulateShader->SetFloat("u_ClipmapExtent", ce.z);

        RenderCommand::BindImageTexture(0, s_Data.m_SnowDepthTexture->GetRendererID(),
                                        0, false, 0, GL_READ_WRITE, GL_R32F);

        u32 groups = (kSnowDepthResolution + 15) / 16;
        RenderCommand::DispatchCompute(groups, groups, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        s_Data.m_PrevClipmapCenter = cameraPos;
    }

    void SnowAccumulationSystem::SubmitDeformers(const glm::vec4* stamps, u32 count)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || count == 0 || !stamps)
        {
            return;
        }

        constexpr u32 stampSize = 2 * sizeof(glm::vec4); // 32 bytes per stamp
        u32 dataSize = count * stampSize;

        // Upload stamp data to SSBO and bind
        s_Data.m_DeformerSSBO->SetData(stamps, std::min(dataSize, 64u * stampSize));
        s_Data.m_DeformerSSBO->Bind();

        // Dispatch deformation compute
        s_Data.m_DeformShader->Bind();
        s_Data.m_DeformShader->SetInt("u_StampCount", static_cast<i32>(std::min(count, 64u)));
        s_Data.m_DeformShader->SetInt("u_Resolution", static_cast<i32>(kSnowDepthResolution));

        const auto& ce = s_Data.m_GPUData.ClipmapCenterAndExtent[0];
        s_Data.m_DeformShader->SetFloat2("u_ClipmapCenter", glm::vec2(ce.x, ce.y));
        s_Data.m_DeformShader->SetFloat("u_ClipmapExtent", ce.z);

        RenderCommand::BindImageTexture(0, s_Data.m_SnowDepthTexture->GetRendererID(),
                                        0, false, 0, GL_READ_WRITE, GL_R32F);

        u32 groups = (kSnowDepthResolution + 15) / 16;
        RenderCommand::DispatchCompute(groups, groups, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
    }

    void SnowAccumulationSystem::BindSnowDepthTexture()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized && s_Data.m_SnowDepthTexture)
        {
            s_Data.m_SnowDepthTexture->Bind(ShaderBindingLayout::TEX_SNOW_DEPTH);
        }
    }

    u32 SnowAccumulationSystem::GetSnowDepthTextureID()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized && s_Data.m_SnowDepthTexture)
        {
            return s_Data.m_SnowDepthTexture->GetRendererID();
        }
        return 0;
    }

    void SnowAccumulationSystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_NeedsClear = true;
        s_Data.m_AccumulatedTime = 0.0f;
    }
} // namespace OloEngine
