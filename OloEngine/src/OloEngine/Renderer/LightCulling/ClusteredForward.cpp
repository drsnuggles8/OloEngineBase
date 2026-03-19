#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/ClusteredForward.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    void ClusteredForward::Initialize(u32 screenWidth, u32 screenHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_GridConfig.TileSizePixels = 16;
        m_GridConfig.MaxLightsPerTile = 256;
        m_GridConfig.DepthSlices = 1;

        m_LightGrid.Initialize(screenWidth, screenHeight, m_GridConfig);
        m_LightBuffer.Initialize(1024, 256);
        m_CullingPass.Initialize();

        m_ForwardPlusUBO = UniformBuffer::Create(
            UBOStructures::ForwardPlusUBO::GetSize(),
            ShaderBindingLayout::UBO_FORWARD_PLUS);

        m_Initialized = true;
        OLO_CORE_INFO("ClusteredForward: Initialized ({}x{}, tile={}px)",
                      screenWidth, screenHeight, m_GridConfig.TileSizePixels);
    }

    void ClusteredForward::Shutdown()
    {
        m_ForwardPlusUBO.Reset();
        m_LightGrid.Shutdown();
        m_Initialized = false;
    }

    void ClusteredForward::Resize(u32 screenWidth, u32 screenHeight)
    {
        if (m_Initialized)
        {
            m_LightGrid.Resize(screenWidth, screenHeight);
        }
    }

    void ClusteredForward::GatherLights(const Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        std::vector<GPUPointLight> pointLights;
        std::vector<GPUSpotLight> spotLights;

        // Gather point lights
        auto pointLightView = scene.GetAllEntitiesWith<TransformComponent, PointLightComponent>();
        for (auto entity : pointLightView)
        {
            auto& transform = pointLightView.template get<TransformComponent>(entity);
            auto& light = pointLightView.template get<PointLightComponent>(entity);

            GPUPointLight gpu;
            gpu.PositionAndRadius = glm::vec4(transform.Translation, light.m_Range);
            gpu.ColorAndIntensity = glm::vec4(light.m_Color, light.m_Intensity);
            pointLights.push_back(gpu);
        }

        // Gather spot lights
        auto spotLightView = scene.GetAllEntitiesWith<TransformComponent, SpotLightComponent>();
        for (auto entity : spotLightView)
        {
            auto& transform = spotLightView.template get<TransformComponent>(entity);
            auto& light = spotLightView.template get<SpotLightComponent>(entity);

            GPUSpotLight gpu;
            gpu.PositionAndRadius = glm::vec4(transform.Translation, light.m_Range);
            gpu.DirectionAndAngle = glm::vec4(
                glm::normalize(light.m_Direction),
                glm::cos(glm::radians(light.m_OuterCutoff)));
            gpu.ColorAndIntensity = glm::vec4(light.m_Color, light.m_Intensity);
            gpu.SpotParams = glm::vec4(
                glm::cos(glm::radians(light.m_InnerCutoff)),
                light.m_Attenuation,
                0.0f, 0.0f);
            spotLights.push_back(gpu);
        }

        // Upload to GPU
        m_LightBuffer.Update(pointLights, spotLights);

        // Decide if Forward+ should be active
        const u32 totalLights = static_cast<u32>(pointLights.size() + spotLights.size());
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
                m_ActiveThisFrame = (totalLights > m_LightCountThreshold);
                break;
        }
    }

    void ClusteredForward::DispatchCulling(const glm::mat4& viewMatrix,
                                           const glm::mat4& projectionMatrix,
                                           u32 depthTextureID)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ActiveThisFrame || !m_Initialized)
        {
            return;
        }

        m_CullingPass.Dispatch(m_LightGrid, m_LightBuffer,
                               viewMatrix, projectionMatrix, depthTextureID);
    }

    void ClusteredForward::BindForShading()
    {
        if (!m_ActiveThisFrame)
        {
            return;
        }

        m_LightBuffer.Bind();
        m_LightGrid.Bind();

        // Upload Forward+ parameters UBO
        if (m_ForwardPlusUBO)
        {
            UBOStructures::ForwardPlusUBO uboData{};
            uboData.Params = glm::uvec4(
                m_LightGrid.GetTileSizePixels(),
                m_LightGrid.GetTileCountX(),
                1u, // Enabled
                0u  // Reserved
            );
            m_ForwardPlusUBO->SetData(&uboData, sizeof(uboData));
            m_ForwardPlusUBO->Bind();
        }
    }

    void ClusteredForward::UnbindAfterShading() const
    {
        if (!m_ActiveThisFrame)
        {
            return;
        }

        m_LightGrid.Unbind();
        m_LightBuffer.Unbind();
    }

    void ClusteredForward::UploadDisabledUBO()
    {
        if (!m_ForwardPlusUBO)
        {
            return;
        }

        UBOStructures::ForwardPlusUBO uboData{};
        uboData.Params = glm::uvec4(0u, 0u, 0u, 0u); // Enabled = 0
        m_ForwardPlusUBO->SetData(&uboData, sizeof(uboData));
        m_ForwardPlusUBO->Bind();
    }

    bool ClusteredForward::ShouldUseForwardPlus() const
    {
        return m_ActiveThisFrame && m_Initialized;
    }

    void ClusteredForward::SetTileSize(u32 tileSize)
    {
        if (tileSize != m_GridConfig.TileSizePixels && tileSize > 0)
        {
            m_GridConfig.TileSizePixels = tileSize;
            if (m_Initialized)
            {
                m_LightGrid.Initialize(m_LightGrid.GetScreenWidth(),
                                       m_LightGrid.GetScreenHeight(),
                                       m_GridConfig);
            }
        }
    }

    void ClusteredForward::SetDepthSlices(u32 slices)
    {
        if (slices != m_GridConfig.DepthSlices && slices > 0)
        {
            m_GridConfig.DepthSlices = slices;
            if (m_Initialized)
            {
                m_LightGrid.Initialize(m_LightGrid.GetScreenWidth(),
                                       m_LightGrid.GetScreenHeight(),
                                       m_GridConfig);
            }
        }
    }

    void ClusteredForward::RenderDebugOverlay(u32 fullscreenQuadVAO, const Ref<Shader>& debugShader)
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
