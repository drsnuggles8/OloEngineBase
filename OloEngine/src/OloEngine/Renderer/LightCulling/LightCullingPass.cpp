#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightCullingPass.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    void LightCullingPass::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        m_CullingShader = ComputeShader::Create("assets/shaders/compute/LightCulling.comp");
        m_DepthPrepareShader = ComputeShader::Create("assets/shaders/compute/DepthPrepare.comp");
        m_DepthAwareCullingShader = ComputeShader::Create("assets/shaders/compute/DepthAwareLightCulling.comp");

        if (!m_CullingShader || !m_CullingShader->IsValid())
        {
            OLO_CORE_ERROR("LightCullingPass: Failed to load LightCulling compute shader!");
        }
        else
        {
            OLO_CORE_INFO("LightCullingPass: Initialized successfully.");
        }

        if (!m_DepthPrepareShader || !m_DepthPrepareShader->IsValid() ||
            !m_DepthAwareCullingShader || !m_DepthAwareCullingShader->IsValid())
        {
            OLO_CORE_WARN("LightCullingPass: Depth-aware shaders unavailable; using fixed-grid fallback.");
        }
    }

    void LightCullingPass::Reload()
    {
        OLO_PROFILE_FUNCTION();

        if (m_CullingShader)
        {
            m_CullingShader->Reload();
        }
        if (m_DepthPrepareShader)
            m_DepthPrepareShader->Reload();
        if (m_DepthAwareCullingShader)
            m_DepthAwareCullingShader->Reload();
    }

    void LightCullingPass::Shutdown()
    {
        m_CullingShader.Reset();
        m_DepthPrepareShader.Reset();
        m_DepthAwareCullingShader.Reset();
        m_ParamsUBO.Reset();
        m_LastDispatchDepthAware = false;
    }

    void LightCullingPass::Dispatch(LightGrid& grid,
                                    const LightCullingBuffer& lightBuffer,
                                    const glm::mat4& viewMatrix,
                                    const glm::mat4& projectionMatrix,
                                    f32 nearPlane,
                                    f32 farPlane,
                                    RHI::ResourceHandle sceneDepth,
                                    bool depthPrepassAvailable)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_CullingShader || !m_CullingShader->IsValid())
        {
            m_LastDispatchDepthAware = false;
            return;
        }

        if (!grid.IsInitialized() || !lightBuffer.IsInitialized())
        {
            m_LastDispatchDepthAware = false;
            return;
        }

        const bool useDepthAware = depthPrepassAvailable && sceneDepth.IsValid() &&
                                   m_DepthPrepareShader && m_DepthPrepareShader->IsValid() &&
                                   m_DepthAwareCullingShader && m_DepthAwareCullingShader->IsValid();
        m_LastDispatchDepthAware = useDepthAware;

        const u32 pointLightCount = lightBuffer.GetPointLightCount();
        const u32 spotLightCount = lightBuffer.GetSpotLightCount();
        const u32 sphereAreaLightCount = lightBuffer.GetSphereAreaLightCount();
        if (pointLightCount == 0 && spotLightCount == 0 && sphereAreaLightCount == 0)
        {
            // Clear stale grid data from the previous frame
            grid.ClearLightGrid();
            grid.ResetCountersAndIndirectArgs();
            m_LastDispatchDepthAware = false;
            return;
        }

        // Reset the append counter and seed the indirect dispatch dimensions.
        grid.ResetCountersAndIndirectArgs();

        // Clear the light grid (zero out all offset/count pairs)
        grid.ClearLightGrid();

        // Bind all SSBOs
        lightBuffer.Bind();
        grid.Bind();

        // One std140 refill per dispatch. These were bare uniforms fed through
        // ComputeShader::Set*, which the Vulkan SPIR-V route cannot express and
        // whose Set* is a no-op there — so Forward+ culling would have read
        // zeros and left every cluster empty (issue #691).
        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::LightCullingUBO::GetSize(),
                                                ShaderBindingLayout::UBO_LIGHT_CULLING);
        }
        UBOStructures::LightCullingUBO cullParams{};
        cullParams.ViewMatrix = viewMatrix;
        cullParams.InverseProjectionMatrix = RHI::AdjustedInverseForShaderReconstruction(projectionMatrix);
        cullParams.PointLightCount = pointLightCount;
        cullParams.SpotLightCount = spotLightCount;
        cullParams.SphereAreaLightCount = sphereAreaLightCount;
        cullParams.MaxLightsPerCluster = grid.GetMaxLightsPerCluster();
        cullParams.NearPlane = std::max(nearPlane, ClusteredLighting::kMinNearPlane);
        cullParams.FarPlane = std::max(farPlane, cullParams.NearPlane * (1.0f + 1e-3f));
        cullParams.ScreenSize = glm::uvec2(grid.GetScreenWidth(), grid.GetScreenHeight());
        cullParams.ClusterParams = glm::uvec4(grid.GetClusterCountX(), grid.GetClusterCountY(),
                                              grid.GetClusterCountZ(), grid.GetActiveClusterListOffsetWords());
        cullParams.LayoutParams = glm::uvec4(grid.GetDepthTileMetadataOffsetWords(), 0u, 0u, 0u);
        m_ParamsUBO->SetData(&cullParams, sizeof(cullParams));
        m_ParamsUBO->Bind();

        if (useDepthAware)
        {
            // The depth prepass just wrote this attachment. Publish it at the
            // established scene-depth slot before the tile reduction samples it.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::Framebuffer | MemoryBarrierFlags::TextureFetch);
            m_DepthPrepareShader->Bind();
            HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepth,
                                             RHI::HeapSlotLifetime::FrameTransient);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(grid.GetClusterCountX(), grid.GetClusterCountY(), 1u);
            m_DepthPrepareShader->Unbind();

            // The prepare pass wrote the active-cluster suffix and indirect
            // uvec3. Both the next shader and the command processor consume it.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

            m_DepthAwareCullingShader->Bind();
            RenderCommand::DispatchComputeIndirect(grid.GetGlobalIndexSSBO()->GetRHIHandle(),
                                                   ClusteredLighting::kIndirectDispatchOffsetBytes);
            m_DepthAwareCullingShader->Unbind();
        }
        else
        {
            // Fixed-grid fallback: exact pre-#722 behavior when there was no
            // usable single-sample depth prepass.
            m_CullingShader->Bind();
            RenderCommand::DispatchCompute(
                grid.GetClusterCountX(), grid.GetClusterCountY(), grid.GetClusterCountZ());
            m_CullingShader->Unbind();
        }

        // Memory barrier: ensure SSBO writes are visible to fragment shaders
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    bool LightCullingPass::IsValid() const
    {
        return m_CullingShader && m_CullingShader->IsValid();
    }
} // namespace OloEngine
