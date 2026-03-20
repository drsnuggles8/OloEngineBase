#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightCullingPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <glad/gl.h>

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
                                    u32 depthTextureID)
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
        if (pointLightCount == 0 && spotLightCount == 0)
        {
            // Clear stale grid data from the previous frame
            grid.GetLightGridSSBO()->ClearData();
            grid.ResetAtomicCounter();
            return;
        }

        // Reset atomic counter
        grid.ResetAtomicCounter();

        // Clear the light grid (zero out all offset/count pairs)
        grid.GetLightGridSSBO()->ClearData();

        // Bind all SSBOs
        lightBuffer.Bind();
        grid.Bind();

        // Bind depth texture for tile frustum depth range extraction
        glBindTextureUnit(0, depthTextureID);

        // Bind and set uniforms on the compute shader
        m_CullingShader->Bind();
        m_CullingShader->SetMat4("u_ViewMatrix", viewMatrix);
        m_CullingShader->SetMat4("u_InverseProjectionMatrix", glm::inverse(projectionMatrix));
        m_CullingShader->SetUint("u_ScreenWidth", grid.GetScreenWidth());
        m_CullingShader->SetUint("u_ScreenHeight", grid.GetScreenHeight());
        m_CullingShader->SetUint("u_PointLightCount", pointLightCount);
        m_CullingShader->SetUint("u_SpotLightCount", spotLightCount);
        m_CullingShader->SetUint("u_TileSizePixels", grid.GetTileSizePixels());
        m_CullingShader->SetUint("u_MaxLightsPerTile", grid.GetMaxLightsPerTile());

        // Dispatch one workgroup per tile
        const u32 groupsX = grid.GetTileCountX();
        const u32 groupsY = grid.GetTileCountY();
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);

        // Memory barrier: ensure SSBO writes are visible to fragment shaders
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        m_CullingShader->Unbind();
    }

    bool LightCullingPass::IsValid() const
    {
        return m_CullingShader && m_CullingShader->IsValid();
    }
} // namespace OloEngine
