#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include <functional>

namespace OloEngine
{
    // Indicates which shadow type is being rendered in the current callback invocation
    enum class ShadowPassType : u8
    {
        CSM,  // Directional light cascaded shadow map
        Spot, // Spot light 2D shadow map
        Point // Point light cubemap face (callback also receives light pos/far)
    };

    // @brief Render pass for shadow map generation.
    //
    // Executes before SceneRenderPass. For each shadow-casting light,
    // renders scene geometry from the light's perspective into the
    // appropriate shadow map texture layer.
    //
    // Uses a callback pattern: the Scene sets a render callback that
    // draws all shadow-casting geometry using a depth-only shader.
    class ShadowRenderPass : public RenderPass
    {
      public:
        // Callback signature: (lightSpaceMatrix, layerOrFace, passType)
        // For Point passes, caller can query ShadowMap for light position/far.
        using ShadowRenderCallback = std::function<void(const glm::mat4& lightVP, u32 layer, ShadowPassType type)>;

        ShadowRenderPass();
        ~ShadowRenderPass() override;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetShadowMap(ShadowMap* shadowMap)
        {
            m_ShadowMap = shadowMap;
        }
        void SetRenderCallback(ShadowRenderCallback callback);

      private:
        ShadowMap* m_ShadowMap = nullptr;
        ShadowRenderCallback m_RenderCallback;
        Ref<Framebuffer> m_ShadowFramebuffer; // Depth-only FBO for shadow rendering
    };
} // namespace OloEngine
