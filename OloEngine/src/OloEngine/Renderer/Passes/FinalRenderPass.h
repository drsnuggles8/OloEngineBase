#pragma once

#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    /**
     * @brief The final render pass that presents the rendered scene to the screen.
     * 
     * This pass takes the output from a previous pass (typically the scene pass) and
     * renders it to the default framebuffer (screen) using a fullscreen triangle.
     * It can also apply post-processing effects like tone mapping, color grading, etc.
     */
    class FinalRenderPass : public RenderPass
    {
    public:
        FinalRenderPass();
        ~FinalRenderPass() override = default;

        /**
         * @brief Initialize the final render pass with a framebuffer specification.
         * @param spec The specification for the framebuffer (though this pass renders to the default framebuffer)
         */
        void Init(const FramebufferSpecification& spec) override;
        
        /**
         * @brief Execute the final pass, rendering the input framebuffer to the screen.
         */
        void Execute() override;
        
        /**
         * @brief Get the target framebuffer of this pass (null since it renders to the default framebuffer).
         * @return A reference to the target framebuffer (null for this pass)
         */
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override { return m_Target; }
        
        /**
         * @brief Set the input framebuffer that will be rendered to the screen.
         * @param input The input framebuffer to render
         */
        void SetInputFramebuffer(const Ref<Framebuffer>& input) { m_InputFramebuffer = input; }
        
        /**
         * @brief Get the current input framebuffer.
         * @return A reference to the input framebuffer
         */
        [[nodiscard]] Ref<Framebuffer> GetInputFramebuffer() const { return m_InputFramebuffer; }

        /**
         * @brief Set up the framebuffer for this pass with the specified dimensions.
         * @param width The width of the framebuffer
         * @param height The height of the framebuffer
         * @note This pass doesn't create a framebuffer, but stores the dimensions for viewport setup
         */
        void SetupFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Update the dimensions when the window is resized.
         * @param width The new width 
         * @param height The new height
         */
        void ResizeFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Reset the pass, recreating the fullscreen triangle and shader if needed.
         */
        void OnReset() override;

    private:
        /**
         * @brief Create a fullscreen triangle mesh for efficient screen rendering.
         * 
         * Uses a single triangle that extends beyond the screen boundaries to cover
         * the entire viewport with fewer vertices than a quad would require.
         */
        void CreateFullscreenTriangle();

    private:
        Ref<Framebuffer> m_InputFramebuffer; // The framebuffer to render to the screen
        Ref<Shader> m_BlitShader;            // Shader for blitting the framebuffer to the screen
        Ref<VertexArray> m_FullscreenTriangleVA; // Vertex array for the fullscreen triangle
    };
} 