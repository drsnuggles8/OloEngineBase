#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandRenderPass.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    /**
     * @brief Command-based render pass for the final screen output.
     * 
     * This pass takes the output from a previous pass (typically the scene pass) and
     * renders it to the default framebuffer (screen) using a fullscreen triangle.
     * It can optionally apply post-processing effects.
     */
    class CommandFinalRenderPass : public CommandRenderPass
    {
    public:
        CommandFinalRenderPass();
        ~CommandFinalRenderPass() override = default;

        /**
         * @brief Initialize the final render pass.
         * @param spec The specification for the framebuffer (mostly ignored as we render to default framebuffer)
         */
        void Init(const FramebufferSpecification& spec) override;
        
        /**
         * @brief Set the input framebuffer that will be rendered to the screen.
         * @param input The input framebuffer to render
         */
        void SetInputFramebuffer(const Ref<Framebuffer>& input);
        
        /**
         * @brief Get the current input framebuffer.
         * @return A reference to the input framebuffer
         */
        [[nodiscard]] Ref<Framebuffer> GetInputFramebuffer() const { return m_InputFramebuffer; }

        /**
         * @brief Set up the framebuffer for this pass with the specified dimensions.
         * @param width The width of the viewport
         * @param height The height of the viewport
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

        /**
         * @brief Enable or disable tone mapping.
         * @param enabled Whether tone mapping should be enabled
         */
        void SetToneMappingEnabled(bool enabled) { m_ToneMappingEnabled = enabled; }
        
        /**
         * @brief Set the exposure value for tone mapping.
         * @param exposure The exposure value (typically between 0.1 and 5.0)
         */
        void SetExposure(float exposure) { m_Exposure = exposure; }

    protected:
        /**
         * @brief Build the command bucket for the final pass.
         * @param bucket Reference to the command bucket to populate
         */
        void BuildCommandBucket(CommandBucket& bucket) override;

        /**
         * @brief Begin rendering to the screen.
         * Sets up the viewport and binds the default framebuffer.
         */
        void BeginRender() override;

        /**
         * @brief End rendering to the screen.
         */
        void EndRender() override;

    private:
        /**
         * @brief Create a fullscreen triangle mesh for efficient screen rendering.
         * 
         * Uses a single triangle that extends beyond the screen boundaries to cover
         * the entire viewport with fewer vertices than a quad would require.
         */
        void CreateFullscreenTriangle();

    private:
        Ref<Framebuffer> m_InputFramebuffer;         // The framebuffer to render to the screen
        Ref<Shader> m_BlitShader;                    // Shader for blitting the framebuffer to the screen
        Ref<VertexArray> m_FullscreenTriangleVA;     // Vertex array for the fullscreen triangle
        
        // Post-processing parameters
        bool m_ToneMappingEnabled = true;            // Whether to apply tone mapping
        float m_Exposure = 1.0f;                     // Exposure value for tone mapping
    };
}