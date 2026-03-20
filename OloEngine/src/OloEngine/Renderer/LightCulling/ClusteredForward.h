#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"
#include "OloEngine/Renderer/LightCulling/LightCullingPass.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;
    class Shader;

    // @brief Forward+ rendering mode
    enum class ForwardPlusMode : u8
    {
        Auto,   // Use Forward+ when light count exceeds threshold
        Always, // Always use Forward+ (even with few lights)
        Never   // Disabled — always use simple forward
    };

    // @brief High-level Forward+ integration for SceneRenderPass.
    //
    // Owns the LightGrid, LightCullingBuffer, and LightCullingPass.
    // Call GatherLights() to pack scene lights into SSBOs, then
    // DispatchCulling() after the depth pre-pass, and BindForShading()
    // before the color pass.
    class ClusteredForward
    {
      public:
        ClusteredForward() = default;
        ~ClusteredForward() = default;
        ClusteredForward(const ClusteredForward&) = delete;
        ClusteredForward& operator=(const ClusteredForward&) = delete;
        ClusteredForward(ClusteredForward&&) = delete;
        ClusteredForward& operator=(ClusteredForward&&) = delete;

        void Initialize(u32 screenWidth, u32 screenHeight);
        void Shutdown();
        void Resize(u32 screenWidth, u32 screenHeight);

        // Gather point/spot lights from the scene into GPU SSBOs
        void GatherLights(const Scene& scene);

        // Dispatch the light culling compute pass (after depth pre-pass)
        void DispatchCulling(const glm::mat4& viewMatrix,
                             const glm::mat4& projectionMatrix,
                             u32 depthTextureID);

        // Bind light grid + light data SSBOs for the forward color pass
        void BindForShading();

        // Unbind after color pass
        void UnbindAfterShading() const;

        // Upload UBO with Enabled=0 (called by BindSceneUBOs as baseline)
        void UploadDisabledUBO();

        // Should Forward+ be active this frame?
        [[nodiscard]] bool ShouldUseForwardPlus() const;

        // Configuration
        void SetMode(ForwardPlusMode mode)
        {
            m_Mode = mode;
        }
        [[nodiscard]] ForwardPlusMode GetMode() const
        {
            return m_Mode;
        }

        void SetTileSize(u32 tileSize);
        void SetDepthSlices(u32 slices);
        void SetLightCountThreshold(u32 threshold)
        {
            m_LightCountThreshold = threshold;
        }

        // Debug
        void SetDebugVisualization(bool enabled)
        {
            m_DebugVisualization = enabled;
        }
        [[nodiscard]] bool IsDebugVisualization() const
        {
            return m_DebugVisualization;
        }
        void RenderDebugOverlay(u32 fullscreenQuadVAO, const Ref<Shader>& debugShader);

        // Stats
        [[nodiscard]] u32 GetPointLightCount() const
        {
            return m_LightBuffer.GetPointLightCount();
        }
        [[nodiscard]] u32 GetSpotLightCount() const
        {
            return m_LightBuffer.GetSpotLightCount();
        }
        [[nodiscard]] u32 GetTotalLightCount() const
        {
            return m_LightBuffer.GetPointLightCount() + m_LightBuffer.GetSpotLightCount();
        }
        [[nodiscard]] u32 GetTileCountX() const
        {
            return m_LightGrid.GetTileCountX();
        }
        [[nodiscard]] u32 GetTileCountY() const
        {
            return m_LightGrid.GetTileCountY();
        }
        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }
        [[nodiscard]] bool IsActive() const
        {
            return m_ActiveThisFrame;
        }

        // Access grid for debug visualization
        [[nodiscard]] const LightGrid& GetLightGrid() const
        {
            return m_LightGrid;
        }

      private:
        LightGrid m_LightGrid;
        LightCullingBuffer m_LightBuffer;
        LightCullingPass m_CullingPass;
        Ref<UniformBuffer> m_ForwardPlusUBO;

        ForwardPlusMode m_Mode = ForwardPlusMode::Auto;
        u32 m_LightCountThreshold = 8;
        bool m_DebugVisualization = false;
        bool m_Initialized = false;
        bool m_ActiveThisFrame = false;

        LightGridConfig m_GridConfig;
    };
} // namespace OloEngine
