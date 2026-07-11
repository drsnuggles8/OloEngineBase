#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    void TiledForwardPlus::Initialize(u32 screenWidth, u32 screenHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_LightGrid.Initialize(screenWidth, screenHeight, m_GridConfig);
        m_LightBuffer.Initialize(1024, 256, 64);
        m_CullingPass.Initialize();

        m_ForwardPlusUBO = UniformBuffer::Create(
            UBOStructures::ForwardPlusUBO::GetSize(),
            ShaderBindingLayout::UBO_FORWARD_PLUS);

        // Only mark as initialized if all sub-components succeeded
        if (!m_LightGrid.IsInitialized() || !m_LightBuffer.IsInitialized() || !m_CullingPass.IsValid() || !m_ForwardPlusUBO)
        {
            OLO_CORE_ERROR("TiledForwardPlus: Initialization failed — one or more sub-components are invalid");
            m_Initialized = false;
            return;
        }

        m_Initialized = true;
        OLO_CORE_INFO("TiledForwardPlus: Initialized ({}x{}, clusters {}x{}x{})",
                      screenWidth, screenHeight,
                      m_GridConfig.ClusterCountX, m_GridConfig.ClusterCountY, m_GridConfig.ClusterCountZ);
    }

    void TiledForwardPlus::Shutdown()
    {
        m_ForwardPlusUBO.Reset();
        m_LightGrid.Shutdown();
        m_LightBuffer.Shutdown();
        m_CullingPass.Shutdown();
        m_ActiveThisFrame = false;
        m_Initialized = false;
    }

    void TiledForwardPlus::Resize(u32 screenWidth, u32 screenHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Initialized)
        {
            m_LightGrid.Resize(screenWidth, screenHeight);
        }
    }

    void TiledForwardPlus::SetLights(const std::vector<GPUPointLight>& pointLights,
                                     const std::vector<GPUSpotLight>& spotLights,
                                     const std::vector<GPUSphereAreaLight>& sphereAreaLights)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        // The scene iterates its light components once per frame to build the
        // MultiLightUBO; it packs these Forward+ SSBO vectors in that same pass
        // (Scene::ProcessScene3DSharedLogic) and hands them here, so the cull
        // path no longer re-iterates the scene.
        m_LightBuffer.Update(pointLights, spotLights, sphereAreaLights);

        // Decide if Forward+ should be active
        const u32 totalLights = static_cast<u32>(pointLights.size() + spotLights.size() + sphereAreaLights.size());
        switch (m_Mode)
        {
            case ForwardPlusMode::Always:
                m_ActiveThisFrame = (totalLights > 0);
                break;
            case ForwardPlusMode::Never:
                m_ActiveThisFrame = false;
                break;
            case ForwardPlusMode::Auto:
            default:
                // Hysteresis: once active, stay active until the light count
                // falls to the lower "downgrade" threshold. Prevents path
                // oscillation when counts hover at the upgrade boundary.
                // Treat an ill-configured down threshold (>= upper) as no
                // hysteresis — use the upper bound on both sides.
                if (m_ActiveThisFrame)
                {
                    const u32 downThreshold = (m_LightCountThresholdDown < m_LightCountThreshold)
                                                  ? m_LightCountThresholdDown
                                                  : m_LightCountThreshold;
                    m_ActiveThisFrame = (totalLights > downThreshold);
                }
                else
                {
                    m_ActiveThisFrame = (totalLights > m_LightCountThreshold);
                }
                break;
        }
    }

    void TiledForwardPlus::DispatchCulling(const glm::mat4& viewMatrix,
                                           const glm::mat4& projectionMatrix)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ActiveThisFrame || !m_Initialized)
        {
            return;
        }

        // The camera clip planes drive the exponential depth-slice mapping;
        // capture them for BindForShading's UBO upload (and the froxel-fog
        // pass) so cull and consumption always agree on the slicing.
        ClusteredLighting::ExtractClipPlanes(projectionMatrix, m_NearPlane, m_FarPlane);

        // The cull SSBO light positions are uploaded render-RELATIVE
        // (LightCullingBuffer::Update shifts them by the render origin, issue
        // #429), so the compute's view matrix must map RELATIVE world to view.
        // Renderer3D::GetViewMatrix() is the WORLD view — shift it here. No-op
        // near origin.
        const glm::mat4 viewRelative = MakeViewRelative(viewMatrix, Renderer3D::GetRenderOrigin());

        m_CullingPass.Dispatch(m_LightGrid, m_LightBuffer,
                               viewRelative, projectionMatrix,
                               m_NearPlane, m_FarPlane);
    }

    void TiledForwardPlus::BindForShading()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ActiveThisFrame)
        {
            return;
        }

        m_LightBuffer.Bind();
        m_LightGrid.Bind();

        // Upload Forward+ clustered parameters UBO
        if (m_ForwardPlusUBO)
        {
            const f32 screenW = static_cast<f32>(m_LightGrid.GetScreenWidth());
            const f32 screenH = static_cast<f32>(m_LightGrid.GetScreenHeight());
            const auto slicing = ClusteredLighting::ComputeDepthSliceParams(
                m_LightGrid.GetClusterCountZ(), m_NearPlane, m_FarPlane);

            UBOStructures::ForwardPlusUBO uboData{};
            uboData.Params = glm::uvec4(
                m_LightGrid.GetClusterCountX(),
                m_LightGrid.GetClusterCountY(),
                1u, // Enabled
                m_LightGrid.GetClusterCountZ());
            uboData.TileScale = glm::vec4(
                screenW > 0.0f ? static_cast<f32>(m_LightGrid.GetClusterCountX()) / screenW : 0.0f,
                screenH > 0.0f ? static_cast<f32>(m_LightGrid.GetClusterCountY()) / screenH : 0.0f,
                0.0f, 0.0f);
            uboData.DepthSlicing = glm::vec4(slicing.Scale, slicing.Bias, m_NearPlane, m_FarPlane);
            m_ForwardPlusUBO->SetData(&uboData, sizeof(uboData));
            m_ForwardPlusUBO->Bind();
        }
    }

    void TiledForwardPlus::UnbindAfterShading() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ActiveThisFrame)
        {
            return;
        }

        m_LightGrid.Unbind();
        m_LightBuffer.Unbind();
    }

    void TiledForwardPlus::UploadDisabledUBO()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ForwardPlusUBO)
        {
            return;
        }

        UBOStructures::ForwardPlusUBO uboData{};
        uboData.Params = glm::uvec4(0u, 0u, 0u, 0u); // Enabled = 0
        uboData.TileScale = glm::vec4(0.0f);
        uboData.DepthSlicing = glm::vec4(0.0f);
        m_ForwardPlusUBO->SetData(&uboData, sizeof(uboData));
        m_ForwardPlusUBO->Bind();
    }

    bool TiledForwardPlus::ShouldUseForwardPlus() const
    {
        return m_ActiveThisFrame && m_Initialized;
    }

    void TiledForwardPlus::RenderDebugOverlay(u32 fullscreenQuadVAO, const Ref<Shader>& debugShader) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_DebugVisualization || !m_Initialized || !m_ActiveThisFrame || !debugShader)
        {
            return;
        }

        // The Forward+ UBO and grid SSBO must already be bound
        debugShader->Bind();

        // Enable alpha blending for the overlay
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);

        glBindVertexArray(fullscreenQuadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // Restore state
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        debugShader->Unbind();
    }
} // namespace OloEngine
