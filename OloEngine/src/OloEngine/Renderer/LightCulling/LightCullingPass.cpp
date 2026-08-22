#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightCullingPass.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    void LightCullingPass::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        m_CullingShader = ComputeShader::Create("assets/shaders/compute/LightCulling.comp");

        if (!m_CullingShader || !m_CullingShader->IsValid())
        {
            OLO_CORE_ERROR("LightCullingPass: Failed to load LightCulling compute shader!");
        }
        else
        {
            OLO_CORE_INFO("LightCullingPass: Initialized successfully.");
        }
    }

    void LightCullingPass::Reload()
    {
        OLO_PROFILE_FUNCTION();

        if (m_CullingShader)
        {
            m_CullingShader->Reload();
        }
    }

    void LightCullingPass::Shutdown()
    {
        m_CullingShader.Reset();
    }

    void LightCullingPass::Dispatch(LightGrid& grid,
                                    const LightCullingBuffer& lightBuffer,
                                    const glm::mat4& viewMatrix,
                                    const glm::mat4& projectionMatrix,
                                    f32 nearPlane,
                                    f32 farPlane)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_CullingShader || !m_CullingShader->IsValid())
        {
            return;
        }

        if (!grid.IsInitialized() || !lightBuffer.IsInitialized())
        {
            return;
        }

        const u32 pointLightCount = lightBuffer.GetPointLightCount();
        const u32 spotLightCount = lightBuffer.GetSpotLightCount();
        const u32 sphereAreaLightCount = lightBuffer.GetSphereAreaLightCount();
        if (pointLightCount == 0 && spotLightCount == 0 && sphereAreaLightCount == 0)
        {
            // Clear stale grid data from the previous frame
            grid.ClearLightGrid();
            grid.ResetAtomicCounter();
            return;
        }

        // Reset atomic counter
        grid.ResetAtomicCounter();

        // Clear the light grid (zero out all offset/count pairs)
        grid.ClearLightGrid();

        // Bind all SSBOs
        lightBuffer.Bind();
        grid.Bind();

        // Bind and set uniforms on the compute shader. The shader derives the
        // exponential slice bounds from near/far directly, mirroring
        // ClusteredLighting::SliceNearDepth — the fragment-side scale/bias
        // pair rides the ForwardPlusUBO (BindForShading), not this dispatch.
        m_CullingShader->Bind();

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
        cullParams.InverseProjectionMatrix = glm::inverse(projectionMatrix);
        cullParams.PointLightCount = pointLightCount;
        cullParams.SpotLightCount = spotLightCount;
        cullParams.SphereAreaLightCount = sphereAreaLightCount;
        cullParams.MaxLightsPerCluster = grid.GetMaxLightsPerCluster();
        cullParams.NearPlane = std::max(nearPlane, ClusteredLighting::kMinNearPlane);
        cullParams.FarPlane = std::max(farPlane, nearPlane * (1.0f + 1e-3f));
        m_ParamsUBO->SetData(&cullParams, sizeof(cullParams));
        m_ParamsUBO->Bind();

        // Dispatch one workgroup per froxel cluster
        RenderCommand::DispatchCompute(
            grid.GetClusterCountX(), grid.GetClusterCountY(), grid.GetClusterCountZ());

        // Memory barrier: ensure SSBO writes are visible to fragment shaders
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        m_CullingShader->Unbind();
    }

    bool LightCullingPass::IsValid() const
    {
        return m_CullingShader && m_CullingShader->IsValid();
    }
} // namespace OloEngine
