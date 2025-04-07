#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandRenderPass.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    /**
     * @brief Command-based render pass for the main 3D scene.
     * 
     * This pass handles the rendering of 3D scene objects to an offscreen framebuffer
     * using the command bucket system for efficient batching and sorting.
     */
    class CommandSceneRenderPass : public CommandRenderPass
    {
    public:
        CommandSceneRenderPass();
        ~CommandSceneRenderPass() override = default;

        /**
         * @brief Initialize the scene render pass with a framebuffer specification.
         * @param spec The specification for the framebuffer
         */
        void Init(const FramebufferSpecification& spec) override;
		
		/**
		 * @brief Execute the scene rendering, drawing all objects from the command bucket.
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
        
        /**
         * @brief Submit a mesh for rendering.
         * @param mesh The mesh to render
         * @param transform The transformation matrix
         * @param material The material to use
         * @param commandAllocator The allocator for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, CommandAllocator& commandAllocator);
        
        /**
         * @brief Submit a cube for rendering.
         * @param transform The transformation matrix
         * @param material The material to use
         * @param commandAllocator The allocator for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitCube(const Ref<Mesh>& cubeMesh, const glm::mat4& transform, const Material& material, CommandAllocator& commandAllocator);
        
        /**
         * @brief Submit a textured quad for rendering.
         * @param transform The transformation matrix
         * @param texture The texture to use
         * @param commandAllocator The allocator for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, CommandAllocator& commandAllocator);
        
        /**
         * @brief Submit a transparent textured quad for rendering.
         * @param transform The transformation matrix
         * @param texture The texture to use 
         * @param shader The shader to use for transparent rendering
         * @param commandAllocator The allocator for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitTransparentQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, 
                                  const Ref<Shader>& shader, CommandAllocator& commandAllocator);
        
        /**
         * @brief Submit a light cube for rendering.
         * @param cubeMesh The mesh to use for the light cube
         * @param transform The transformation matrix
         * @param lightCubeShader The shader to use for the light cube
         * @param commandAllocator The allocator for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitLightCube(const Ref<Mesh>& cubeMesh, const glm::mat4& transform, 
                            const Ref<Shader>& lightCubeShader, CommandAllocator& commandAllocator);
	};
}