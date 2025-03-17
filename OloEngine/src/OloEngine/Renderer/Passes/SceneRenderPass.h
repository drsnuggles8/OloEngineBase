#pragma once

#include "OloEngine/Renderer/Passes/RenderPass.h"

namespace OloEngine
{
    /**
     * @brief Render pass for the main 3D scene.
     * 
     * This pass renders the scene content from the RenderQueue to an offscreen framebuffer,
     * which can then be used as input to subsequent passes in the render graph.
     */
    class SceneRenderPass : public RenderPass
    {
    public:
        SceneRenderPass();
        ~SceneRenderPass() override = default;

        /**
         * @brief Initialize the scene render pass with a framebuffer specification.
         * @param spec The specification for the framebuffer
         */
        void Init(const FramebufferSpecification& spec) override;
        
        /**
         * @brief Execute the scene rendering, drawing all objects from the RenderQueue.
         */
        void Execute() override;
        
        /**
         * @brief Get the target framebuffer of this pass.
         * @return A reference to the target framebuffer
         */
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override { return m_Target; }

        /**
         * @brief Set up the framebuffer for this pass with the specified dimensions.
         * @param width The width of the framebuffer
         * @param height The height of the framebuffer
         */
        void SetupFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Resize the framebuffer when the window is resized.
         * @param width The new width of the framebuffer
         * @param height The new height of the framebuffer
         */
        void ResizeFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Reset the pass, recreating any necessary resources.
         */
        void OnReset() override;
    };
} 