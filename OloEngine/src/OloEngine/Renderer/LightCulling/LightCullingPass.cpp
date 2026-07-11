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
        m_CullingShader->SetMat4("u_ViewMatrix", viewMatrix);
        m_CullingShader->SetMat4("u_InverseProjectionMatrix", glm::inverse(projectionMatrix));
        m_CullingShader->SetUint("u_PointLightCount", pointLightCount);
        m_CullingShader->SetUint("u_SpotLightCount", spotLightCount);
        m_CullingShader->SetUint("u_SphereAreaLightCount", sphereAreaLightCount);
        m_CullingShader->SetUint("u_MaxLightsPerCluster", grid.GetMaxLightsPerCluster());
        m_CullingShader->SetFloat("u_NearPlane", std::max(nearPlane, ClusteredLighting::kMinNearPlane));
        m_CullingShader->SetFloat("u_FarPlane", std::max(farPlane, nearPlane * (1.0f + 1e-3f)));

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
